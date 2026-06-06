/*
 * drm_decoder.cpp — implementation of the DRM acquisition wrapper.
 */

#include "drm_decoder.h"
#include "image_decode.h"

#include <cmath>
#include <cstring>
#include <algorithm>

// Vendored RXAMADRM / QSSTV engine (Qt-free via qt_shim.h)
#include "engine/drm.h"
#include "engine/demodulator.h"
#include "engine/drmproto.h"
#include "engine/drmdefs.h"
#include "engine/sourcedecoder.h"
#include "engine/shim_deps.h"     // drmprogress::*

// Engine globals defined in the vendored drm.cpp:
extern demodulator* demodulatorPtr;
extern sourceDecoder* srcDecoder;
extern int   runstate;
extern int   robustness_mode;
extern int   spectrum_occupancy;
extern float WMERFAC;
extern float transmission_frame_buffer[82980];
extern int   transmission_frame_buffer_wptr;
extern int   transmission_frame_buffer_data_valid;
extern float psd[513];
extern float samplerate_offset_estimation;
extern int   symbols_per_frame;
extern int   K_modulo, K_dc;

// Globales écrites par channeldecode.cpp (incrément 2) :
extern int   msc_mode_new;
extern int   interleaver_depth_new;
extern bool  callsignValid;
extern bool  MSCAvailable;
extern QString drmCallsign;   // QString = shim défini dans engine/qt_shim.h

namespace drm {

std::mutex DRMDecoder::s_engineMtx;

// Snapshots updated after each stripe (protected by mtx)
static Status                       g_statusSnap;
static std::vector<std::complex<float>> g_constSnap;
static int                          g_constRingPos = 0;
static std::atomic<uint64_t>        g_imageDecodeFailures{0};
static float                        g_psdSnap[513] = {0};

DRMDecoder::DRMDecoder() {
    buildHilbert();
    stripe.assign(2 * STRIPE_COMPLEX, 0.0f);
    g_constSnap.reserve(2048);
}

DRMDecoder::~DRMDecoder() {
    std::lock_guard<std::mutex> elk(s_engineMtx);
    if (ownsEngine) {
        delete demodulatorPtr; demodulatorPtr = nullptr;
        delete srcDecoder;     srcDecoder     = nullptr;
        ownsEngine = false;
    }
}

// ---- Hilbert transformer (type-III FIR, Hamming-windowed) -----------------
void DRMDecoder::buildHilbert() {
    hLen = 101;                 // odd
    hMid = hLen / 2;
    hTaps.assign(hLen, 0.0f);
    for (int i = 0; i < hLen; ++i) {
        int n = i - hMid;
        float h = 0.0f;
        if (n != 0 && (n & 1)) {          // odd n only
            h = (2.0f / (float)M_PI) / (float)n;
        }
        // Hamming window
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (float)(hLen - 1));
        hTaps[i] = h * w;
    }
    delayLine.assign(hLen, 0.0f);
    dlPos = 0;
}

void DRMDecoder::init() {
    std::lock_guard<std::mutex> elk(s_engineMtx);
    std::lock_guard<std::mutex> lk(mtx);

    if (!demodulatorPtr) demodulatorPtr = new demodulator();
    if (!srcDecoder)     srcDecoder     = new sourceDecoder();
    ownsEngine = true;

    // Mirror QSSTV drmRx::init() acquisition bring-up (no Qt).
    demodulatorPtr->init();
    initGetmode(DRMBUFSIZE / 4);
    samplerate_offset_estimation = 0.0f;
    runstate = RUN_STATE_POWER_ON; channel_decoding();
    runstate = RUN_STATE_INIT;     channel_decoding();
    runstate = RUN_STATE_NORMAL;
    srcDecoder->init();

    // Reset Hilbert + stripe + SNR state
    std::fill(delayLine.begin(), delayLine.end(), 0.0f);
    dlPos = 0;
    stripeCount = 0;
    avgSNR = 0.0f;
    avgSNRAvailable = false;
    g_statusSnap = Status{};
    g_constSnap.clear();
    std::memset(g_psdSnap, 0, sizeof(g_psdSnap));
    // Reset suivi de progression
    drmprogress::frameCounter    = 0;
    drmprogress::currentSegments = 0;
    drmprogress::totalSegments   = 0;
    drmprogress::bodySize        = 0;
    drmprogress::segmentSize     = 0;
    {
        std::lock_guard<std::mutex> lk(drmprogress::nameMtx);
        drmprogress::objectName.clear();
    }
    initialized = true;
}

void DRMDecoder::pushComplexSample(float re, float im) {
    stripe[2 * stripeCount]     = re;
    stripe[2 * stripeCount + 1] = im;
    if (++stripeCount >= STRIPE_COMPLEX) {
        // Run one engine block under the engine lock.
        {
            std::lock_guard<std::mutex> elk(s_engineMtx);
            if (demodulatorPtr) {
                demodulatorPtr->demodulate(stripe.data(), STRIPE_COMPLEX);

                // ---- Snapshot status (mirror drmRx::run avgSNR smoothing) ----
                float mer = WMERFAC; if (mer < 0) mer = 0;
                bool facAvail = demodulatorPtr->isFACAvailable();
                if (facAvail) {
                    if (!avgSNRAvailable) { avgSNR = mer; avgSNRAvailable = true; }
                    else                  { avgSNR = 0.95f * avgSNR + 0.05f * mer; }
                }

                std::lock_guard<std::mutex> lk(mtx);
                g_statusSnap.timeSync     = demodulatorPtr->isTimeSync();
                g_statusSnap.freqSync     = demodulatorPtr->isFrequencySync();
                g_statusSnap.frameSync    = demodulatorPtr->isFrameSync();
                g_statusSnap.facAvailable = facAvail;
                g_statusSnap.robustness   = (robustness_mode >= 0 && robustness_mode <= 3)
                                              ? (Robustness)robustness_mode : Robustness::NONE;
                g_statusSnap.spectrumOcc  = spectrum_occupancy;
                g_statusSnap.wmer_dB      = mer;
                g_statusSnap.avgSNR_dB    = avgSNR;
                g_statusSnap.snrAvailable = avgSNRAvailable;

                // ---- Snapshot champs décodés (incrément 2) ----
                g_statusSnap.callsignValid     = callsignValid;
                {
                    const char* cs = drmCallsign.c_str();
                    if (cs) {
                        std::strncpy(g_statusSnap.callsign, cs, sizeof(g_statusSnap.callsign) - 1);
                        g_statusSnap.callsign[sizeof(g_statusSnap.callsign) - 1] = '\0';
                    }
                }
                g_statusSnap.mscMode            = msc_mode_new;
                g_statusSnap.interleaverDepth   = interleaver_depth_new;
                g_statusSnap.mscAvailable       = MSCAvailable;

                // ---- Progression MOT + compteur de trames OFDM ----
                drmprogress::frameCounter.fetch_add(1, std::memory_order_relaxed);
                g_statusSnap.frameCounter   = drmprogress::frameCounter.load();
                g_statusSnap.rxSegments     = drmprogress::currentSegments.load();
                g_statusSnap.totalSegments  = drmprogress::totalSegments.load();
                g_statusSnap.bodySize       = drmprogress::bodySize.load();
                g_statusSnap.segmentSize    = drmprogress::segmentSize.load();
                g_statusSnap.receivingObject = (g_statusSnap.rxSegments > 0);
                {
                    std::lock_guard<std::mutex> lk(drmprogress::nameMtx);
                    // Nettoyage identique à getLastImageBytes : couper aux
                    // premiers caractères non imprimables / juste après la
                    // première extension reconnue.
                    std::string n = drmprogress::objectName;
                    for (size_t i = 0; i < n.size(); ++i) {
                        unsigned char c = (unsigned char)n[i];
                        if (c < 0x20 || c == 0x7F) { n.resize(i); break; }
                    }
                    static const char* exts[] = { ".jp2", ".jpg", ".jpeg", ".png", ".bmp" };
                    size_t bestEnd = std::string::npos;
                    for (const char* e : exts) {
                        size_t p = n.find(e);
                        if (p != std::string::npos) {
                            size_t end = p + std::strlen(e);
                            if (bestEnd == std::string::npos || end < bestEnd) bestEnd = end;
                        }
                    }
                    if (bestEnd != std::string::npos) n.resize(bestEnd);
                    if (n.size() >= 2 && n[0] == '.' && n[1] == '/') n.erase(0, 2);

                    size_t cap = sizeof(g_statusSnap.objectName) - 1;
                    size_t cnt = std::min(n.size(), cap);
                    std::memcpy(g_statusSnap.objectName, n.data(), cnt);
                    g_statusSnap.objectName[cnt] = '\0';
                }

                // ---- Diagnostics image ----
                {
                    std::lock_guard<std::mutex> lk(drmcapture::mtx);
                    g_statusSnap.filesReceived = drmcapture::counter;
                }
                g_statusSnap.imageDecodeFailures = g_imageDecodeFailures.load();

                // ---- Snapshot constellation (cellules OFDM équalisées) ----
                // Accumulation glissante : on garde un buffer circulaire des
                // 4096 dernières cellules pour un rendu très fluide (le menu
                // UI est rafraîchi à ~60 Hz, l'engine produit une trame ~12 Hz,
                // soit ~3 trames d'historique permanent).
                if (g_statusSnap.frameSync && symbols_per_frame > 0 && K_modulo > 0) {
                    int Kmin = demodulatorPtr->getKmin();
                    int Kmax = demodulatorPtr->getKmax();
                    static const int CONST_RING = 4096;
                    if ((int)g_constSnap.size() < CONST_RING) {
                        g_constSnap.resize(CONST_RING, std::complex<float>(0.0f, 0.0f));
                    }
                    for (int i = 0; i < symbols_per_frame; ++i) {
                        for (int j = Kmin; j <= Kmax; ++j) {
                            int idx = i * K_modulo + K_dc + j;
                            if (idx < 0 || 2 * idx + 1 >= 82980) continue;
                            float re = transmission_frame_buffer[2 * idx];
                            float im = transmission_frame_buffer[2 * idx + 1];
                            if (re == 0.0f && im == 0.0f) continue;
                            g_constSnap[g_constRingPos] = std::complex<float>(re, im);
                            g_constRingPos = (g_constRingPos + 1) % CONST_RING;
                        }
                    }
                } else if (!g_statusSnap.frameSync) {
                    // Plus de sync : on vide progressivement (effet "fade out")
                    if (!g_constSnap.empty()) {
                        for (auto& p : g_constSnap) { p *= 0.9f; }
                    }
                }
                // ---- Snapshot PSD ----
                std::memcpy(g_psdSnap, psd, sizeof(g_psdSnap));
            }
        }
        stripeCount = 0;
    }
}

void DRMDecoder::feed(const float* audio, int count) {
    if (!initialized) return;
    const bool inv = invertSpectrum.load();
    for (int s = 0; s < count; ++s) {
        float x = audio[s];
        delayLine[dlPos] = x;

        // Hilbert FIR -> imaginary branch
        float im = 0.0f;
        int idx = dlPos;
        for (int k = 0; k < hLen; ++k) {
            im += hTaps[k] * delayLine[idx];
            if (--idx < 0) idx += hLen;
        }
        // Real branch: input delayed by hMid
        int rIdx = dlPos - hMid; if (rIdx < 0) rIdx += hLen;
        float re = delayLine[rIdx];

        if (++dlPos >= hLen) dlPos = 0;

        pushComplexSample(re, inv ? -im : im);
    }
}

Status DRMDecoder::getStatus() {
    std::lock_guard<std::mutex> lk(mtx);
    return g_statusSnap;
}

int DRMDecoder::getConstellation(std::complex<float>* out, int maxPoints) {
    std::lock_guard<std::mutex> lk(mtx);
    int n = std::min((int)g_constSnap.size(), maxPoints);
    for (int i = 0; i < n; ++i) out[i] = g_constSnap[i];
    return n;
}

void DRMDecoder::getPSD(float* out513) {
    std::lock_guard<std::mutex> lk(mtx);
    std::memcpy(out513, g_psdSnap, sizeof(float) * 513);
}

// ===========================================================================
// Incrément 3 : récupération de l'image décodée
// ===========================================================================

static uint64_t g_lastConsumedCounter = 0;

bool DRMDecoder::hasNewImage() {
    std::lock_guard<std::mutex> lk(drmcapture::mtx);
    return drmcapture::counter > g_lastConsumedCounter;
}

bool DRMDecoder::getLastImageBytes(std::vector<uint8_t>& outBytes,
                                   std::string& outName) {
    std::lock_guard<std::mutex> lk(drmcapture::mtx);
    if (drmcapture::files.empty()) return false;
    const auto& f = drmcapture::files.back();
    outBytes.assign((const uint8_t*)f.bytes.data(),
                    (const uint8_t*)f.bytes.data() + f.bytes.size());

    // Nettoyage du nom de fichier : le parser MOT lit parfois au-delà de la
    // fin réelle du nom (résidu du paquet précédent). On coupe au premier
    // caractère non-imprimable, et on garde le nom jusqu'à la 1re extension
    // reconnue (.jp2, .jpg, .jpeg, .png, .bmp).
    std::string n = f.name;
    // 1) couper au premier caractère non-imprimable
    for (size_t i = 0; i < n.size(); ++i) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c == 0x7F) { n.resize(i); break; }
    }
    // 2) tronquer juste après la première extension reconnue
    const char* exts[] = { ".jp2", ".jpg", ".jpeg", ".png", ".bmp" };
    size_t bestEnd = std::string::npos;
    for (const char* e : exts) {
        size_t p = n.find(e);
        if (p != std::string::npos) {
            size_t end = p + std::strlen(e);
            if (bestEnd == std::string::npos || end < bestEnd) bestEnd = end;
        }
    }
    if (bestEnd != std::string::npos) n.resize(bestEnd);
    // 3) retirer un éventuel "./" en tête
    if (n.size() >= 2 && n[0] == '.' && n[1] == '/') n.erase(0, 2);

    outName = n;
    return true;
}

void DRMDecoder::markImageConsumed() {
    std::lock_guard<std::mutex> lk(drmcapture::mtx);
    g_lastConsumedCounter = drmcapture::counter;
}

std::string DRMDecoder::getImageFormat() {
    std::lock_guard<std::mutex> lk(drmcapture::mtx);
    if (drmcapture::files.empty()) return "?";
    const auto& f = drmcapture::files.back();
    return sniffFormat((const uint8_t*)f.bytes.data(), f.bytes.size());
}

bool DRMDecoder::decodeImageRGBA(std::vector<uint8_t>& outRGBA,
                                 int& width, int& height) {
    std::vector<uint8_t> bytes;
    std::string name;
    if (!getLastImageBytes(bytes, name)) return false;
    return decodeImageToRGBA(bytes.data(), (int)bytes.size(), outRGBA, width, height);
}

void DRMDecoder::noteImageDecodeFailure() {
    g_imageDecodeFailures.fetch_add(1, std::memory_order_relaxed);
}

} // namespace drm
