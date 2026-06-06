/*
 * main.cpp — SDR++ "drm_image_decoder" module.
 *
 * Décodeur d'images numériques HamDRM (DRM bande étroite / SSTV numérique,
 * type EasyPal / DigTRX). Coque SDR++ autour du moteur d'acquisition
 * RXAMADRM de M. Bos (PA0MBO), vendorisé depuis QSSTV (ON4QZ), GPL.
 *
 * Chaîne DSP (selon le mode choisi) :
 *
 *   USB / LSB :  VFO (complexe, 24 kHz, BW 2.7 kHz)
 *                 ─▶ SSB<float>(USB|LSB) ─▶ resampler 24k→12k ─▶ moteur DRM
 *
 *   NFM     :    VFO (complexe, 25 kHz, BW 12.5 kHz)
 *                 ─▶ FM<float>(low-pass) ─▶ resampler 25k→12k ─▶ moteur DRM
 *
 * Auteur de la coque SDR++ : F4JTV (ADRASEC 06).
 * Moteur DRM : RXAMADRM (c) M. Bos PA0MBO / QSSTV, licence GPL.
 */

#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <utils/optionlist.h>
#include <gui/widgets/constellation_diagram.h>
#include <gui/widgets/folder_select.h>
#include <utils/opengl_include_code.h>

#include <dsp/demod/ssb.h>
#include <dsp/demod/fm.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>

#include <complex>
#include <cstring>
#include <cfloat>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include "drm_decoder.h"
#include "image_decode.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "drm_image_decoder",
    /* Description:     */ "HamDRM digital image (DRM SSTV) decoder",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 7, 0,
    /* Max instances    */ 1   // le moteur porte un état global -> instance unique
};

ConfigManager config;

// Modes de démodulation
enum DemodMode {
    DM_USB = 0,
    DM_LSB,
    DM_NFM
};

// Caractéristiques par mode :
//   sample_rate du flux complexe VFO et largeur de bande "canal"
struct ModeParams {
    double vfoSampleRate;
    double vfoBandwidth;
    double minBw, maxBw;
};
static const ModeParams MODE_PARAMS[3] = {
    /* USB */ { 24000.0,  2700.0,  2300.0,  3000.0 },
    /* LSB */ { 24000.0,  2700.0,  2300.0,  3000.0 },
    /* NFM */ { 25000.0, 12500.0,  6250.0, 16000.0 }
};

#define IMG_W 320
#define IMG_H 240

class DRMImageDecoderModule : public ModuleManager::Instance {
public:
    DRMImageDecoderModule(std::string name) {
        this->name = name;

        modes.define("USB", DM_USB);
        modes.define("LSB", DM_LSB);
        modes.define("NFM", DM_NFM);

        // La texture OpenGL est créée lazy (au 1er render UI) pour s'assurer
        // que le contexte OpenGL est actif.

        // Chargement de la configuration
        config.acquire();
        if (config.conf[name].contains("mode")) {
            std::string s = config.conf[name]["mode"];
            if (modes.keyExists(s)) { modeId = modes.keyId(s); }
        }
        if (config.conf[name].contains("invertSpectrum")) {
            invertSpectrum = config.conf[name]["invertSpectrum"];
        }
        if (config.conf[name].contains("nfmLowPass")) {
            nfmLowPass = config.conf[name]["nfmLowPass"];
        }
        // Largeur par mode (clés séparées pour ne pas mélanger SSB et NFM)
        if (config.conf[name].contains("bandwidth_ssb")) {
            ssbBandwidth = config.conf[name]["bandwidth_ssb"];
        }
        if (config.conf[name].contains("bandwidth_nfm")) {
            nfmBandwidth = config.conf[name]["bandwidth_nfm"];
        }
        // Dossier de sauvegarde des images reçues
        if (config.conf[name].contains("savePath")) {
            std::string p = config.conf[name]["savePath"];
            if (!p.empty()) { folderSelect.setPath(p); }
        }
        config.release();

        mode = modes.value(modeId);
        decoder.init();
        decoder.setInvertSpectrum(invertSpectrum);

        buildChain();   // crée VFO + démod + resamp + sink
        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~DRMImageDecoderModule() {
        gui::menu.removeEntry(name);
        if (enabled) { tearDownChain(); }
        sigpath::sinkManager.unregisterStream(name);
        if (imageTexId != 0) {
            glDeleteTextures(1, &imageTexId);
            imageTexId = 0;
        }
    }

    void postInit() {}

    void enable() {
        buildChain();
        decoder.init();
        decoder.setInvertSpectrum(invertSpectrum);
        enabled = true;
    }

    void disable() {
        tearDownChain();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

private:
    // --- Construction / démantèlement de la chaîne selon le mode -----------

    void buildChain() {
        const ModeParams& p = MODE_PARAMS[mode];

        // 1) VFO with proper sideband reference (matches SDR++ radio module)
        //    USB: VFO anchored at lower edge (signal extends upward)
        //    LSB: VFO anchored at upper edge (signal extends downward)
        //    NFM: VFO centered on the signal
        int vfoRef = ImGui::WaterfallVFO::REF_CENTER;
        if      (mode == DM_USB) { vfoRef = ImGui::WaterfallVFO::REF_LOWER; }
        else if (mode == DM_LSB) { vfoRef = ImGui::WaterfallVFO::REF_UPPER; }

        double wfbw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, vfoRef,
                                            std::clamp<double>(0.0, -wfbw / 2.0, wfbw / 2.0),
                                            currentBandwidth(),
                                            p.vfoSampleRate,
                                            p.minBw, p.maxBw,
                                            false);

        // 2) Démodulateur du mode (construction neuve) + 3) resampler vers 12 kHz
        resamp.reset();
        sink.reset();
        ssbDemod.reset();
        fmDemod.reset();

        if (mode == DM_NFM) {
            fmDemod = std::make_unique<dsp::demod::FM<float>>();
            fmDemod->init(vfo->output, p.vfoSampleRate, currentBandwidth(), nfmLowPass);
            resamp = std::make_unique<dsp::multirate::RationalResampler<float>>();
            resamp->init(&fmDemod->out, p.vfoSampleRate, drm::ENGINE_SAMPLE_RATE);
        } else {
            ssbDemod = std::make_unique<dsp::demod::SSB<float>>();
            ssbDemod->init(vfo->output, ssbMode(), currentBandwidth(), p.vfoSampleRate,
                           50.0 / p.vfoSampleRate, 5.0 / p.vfoSampleRate);
            resamp = std::make_unique<dsp::multirate::RationalResampler<float>>();
            resamp->init(&ssbDemod->out, p.vfoSampleRate, drm::ENGINE_SAMPLE_RATE);
        }

        // 4) Sink -> moteur DRM
        sink = std::make_unique<dsp::sink::Handler<float>>();
        sink->init(&resamp->out, sinkHandler, this);

        // 5) Démarrage (toujours dans cet ordre : démod, resampler, sink)
        if (mode == DM_NFM) { fmDemod->start(); }
        else                { ssbDemod->start(); }
        resamp->start();
        sink->start();
        chainAlive = true;
    }

    void tearDownChain() {
        if (!chainAlive) { return; }
        // Stop dans l'ordre inverse, puis on détruit tous les Processor pour
        // garantir qu'un éventuel init() ultérieur ne ré-enregistre pas les
        // streams (ce qui crashait au switch SSB <-> NFM en version 0.2).
        if (sink)     { sink->stop(); }
        if (resamp)   { resamp->stop(); }
        if (ssbDemod) { ssbDemod->stop(); }
        if (fmDemod)  { fmDemod->stop(); }
        sink.reset();
        resamp.reset();
        ssbDemod.reset();
        fmDemod.reset();
        if (vfo) {
            sigpath::vfoManager.deleteVFO(vfo);
            vfo = nullptr;
        }
        chainAlive = false;
    }

    // Reconstruction propre lorsqu'on change de mode (ou de paramètre VFO-dépendant)
    void rebuildChain() {
        if (chainAlive) { tearDownChain(); }
        buildChain();
        // On réinitialise l'acquisition pour repartir sur un état sain
        decoder.init();
        decoder.setInvertSpectrum(invertSpectrum);
    }

    dsp::demod::SSB<float>::Mode ssbMode() {
        return (mode == DM_LSB) ? dsp::demod::SSB<float>::Mode::LSB
                                : dsp::demod::SSB<float>::Mode::USB;
    }

    double currentBandwidth() const {
        return (mode == DM_NFM) ? (double)nfmBandwidth : (double)ssbBandwidth;
    }

    // (Re)load the received image: fetch bytes + decode to RGBA + upload to
    // a managed OpenGL texture. Must be called from the UI thread (OpenGL
    // context must be active).
    void refreshImage() {
        std::vector<uint8_t> bytes;
        std::string name;
        if (!decoder.getLastImageBytes(bytes, name)) {
            flog::warn("[drm_image_decoder] hasNewImage but getLastImageBytes returned false");
            return;
        }
        if (bytes.empty()) {
            flog::warn("[drm_image_decoder] received image buffer is empty");
            return;
        }
        const char* fmt = drm::sniffFormat(bytes.data(), (int)bytes.size());
        flog::info("[drm_image_decoder] new image: name='{}' size={} bytes, format={}",
                   name, bytes.size(), fmt);

        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!drm::decodeImageToRGBA(bytes.data(), (int)bytes.size(), rgba, w, h) ||
            w <= 0 || h <= 0) {
            flog::error("[drm_image_decoder] RGBA decode FAILED ({} bytes, format={})",
                        bytes.size(), fmt);
            decoder.noteImageDecodeFailure();
            decoder.markImageConsumed();
            return;
        }
        flog::info("[drm_image_decoder] RGBA decoded: {}x{} px", w, h);

        imageW = w;
        imageH = h;
        imageFilename = name;
        imageFormat   = fmt;
        lastImageBytes = bytes;
        lastDecodedRGBA = rgba;

        // Upload to a managed OpenGL texture. Created lazily on first call
        // (so the OpenGL context is guaranteed active — we're on the UI thread).
        if (imageTexId == 0) {
            glGenTextures(1, &imageTexId);
        }
        glBindTexture(GL_TEXTURE_2D, imageTexId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

        decoder.markImageConsumed();
        flog::info("[drm_image_decoder] image uploaded to texture id={} ({}x{} px)",
                   (unsigned)imageTexId, w, h);
    }

    // Save the image to the folder selected via folderSelect.
    //  format = "bmp" | "png" | "jpeg" | "raw"
    void saveImageAs(const std::string& format) {
        if (lastImageBytes.empty()) { return; }
        if (!folderSelect.pathIsValid()) {
            lastSaveMsg = "Invalid save folder";
            return;
        }
        std::string dir = folderSelect.path;
        // base = received name without extension
        std::string base = imageFilename;
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) { base.resize(dot); }
        if (base.empty()) { base = "drm_image"; }

        std::string ext = (format == "jpeg") ? "jpg" :
                          (format == "raw")  ? imageFormat : format;
        if (ext.empty() || ext == "?") ext = "bin";
        std::string path = dir + "/" + base + "." + ext;

        bool ok = false;
        if (format == "raw") {
            ok = drm::writeRawBytesToFile(path, lastImageBytes.data(),
                                          (int)lastImageBytes.size());
        } else if (!lastDecodedRGBA.empty() && imageW > 0 && imageH > 0) {
            ok = drm::writeRGBAToFile(path, lastDecodedRGBA.data(),
                                      imageW, imageH, format);
        }
        char buf[400];
        snprintf(buf, sizeof(buf), ok ? "Saved: %s" : "Save failed: %s",
                 path.c_str());
        lastSaveMsg = buf;
    }

    static void sinkHandler(float* data, int count, void* ctx) {
        DRMImageDecoderModule* _this = (DRMImageDecoderModule*)ctx;
        _this->decoder.feed(data, count);
    }

    // --- Petits utilitaires d'UI -------------------------------------------

    // Petit indicateur de synchro coloré : cercle plein vert si OK, contour
    // gris sinon. Dessin direct via ImDrawList — évite les caractères Unicode
    // ●/○ qui s'affichent en "?" avec la police par défaut de SDR++.
    static void syncDot(const char* label, bool ok) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFontSize();
        float r = h * 0.32f;
        ImVec2 ctr(pos.x + r + 1.0f, pos.y + h * 0.55f);
        ImU32 col = ok ? IM_COL32(75, 215, 100, 255)   // vert
                       : IM_COL32(115, 115, 130, 255); // gris
        if (ok) {
            dl->AddCircleFilled(ctr, r, col, 16);
        } else {
            dl->AddCircle(ctr, r, col, 16, 1.5f);
        }
        // Avancer le curseur après le cercle, puis afficher le label
        ImGui::Dummy(ImVec2(2.0f * r + 6.0f, h));
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
    }
    static const char* robustnessName(drm::Robustness r) {
        switch (r) {
        case drm::Robustness::A: return "A";
        case drm::Robustness::B: return "B";
        case drm::Robustness::C: return "C";
        case drm::Robustness::D: return "D";
        default:                 return "--";
        }
    }
    static const char* occupancyName(int occ) {
        switch (occ) {
        case 0: return "4.5 kHz";
        case 1: return "5 kHz";
        case 2: return "9 kHz";
        case 3: return "10 kHz";
        case 4: return "18 kHz";
        case 5: return "20 kHz";
        default: return "--";
        }
    }

    // --- Menu --------------------------------------------------------------

    static void menuHandler(void* ctx) {
        DRMImageDecoderModule* _this = (DRMImageDecoderModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // === Mode (USB / LSB / NFM) ===
        ImGui::LeftLabel("Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##drm_mode_", _this->name), &_this->modeId, _this->modes.txt)) {
            DemodMode newMode = _this->modes.value(_this->modeId);
            bool wasNFM = (_this->mode == DM_NFM);
            bool isNFM  = (newMode == DM_NFM);
            _this->mode = newMode;
            if (wasNFM != isNFM) {
                // SSB <-> NFM: sample rate AND VFO reference change.
                // Full chain rebuild required.
                _this->rebuildChain();
            } else if (!isNFM) {
                // USB <-> LSB: same sample rate, just flip the demodulator mode
                // and the VFO sideband reference (REF_LOWER for USB, REF_UPPER
                // for LSB) so the waterfall band selection follows.
                if (_this->ssbDemod) { _this->ssbDemod->setMode(_this->ssbMode()); }
                if (_this->vfo) {
                    _this->vfo->setReference(newMode == DM_USB
                        ? ImGui::WaterfallVFO::REF_LOWER
                        : ImGui::WaterfallVFO::REF_UPPER);
                }
            }
            config.acquire();
            config.conf[_this->name]["mode"] = _this->modes.key(_this->modeId);
            config.release(true);
        }

        // === Spectrum inversion ===
        if (ImGui::Checkbox(CONCAT("Invert spectrum##drm_inv_", _this->name), &_this->invertSpectrum)) {
            _this->decoder.setInvertSpectrum(_this->invertSpectrum);
            config.acquire();
            config.conf[_this->name]["invertSpectrum"] = _this->invertSpectrum;
            config.release(true);
        }

        // === Demodulation bandwidth (per mode) ===
        ImGui::LeftLabel("Bandwidth");
        ImGui::FillWidth();
        const ModeParams& p = MODE_PARAMS[_this->mode];
        float* bwPtr = (_this->mode == DM_NFM) ? &_this->nfmBandwidth : &_this->ssbBandwidth;
        if (ImGui::SliderFloat(CONCAT("##drm_bw_", _this->name), bwPtr,
                                (float)p.minBw, (float)p.maxBw, "%.0f Hz")) {
            if (_this->mode == DM_NFM) {
                if (_this->fmDemod) { _this->fmDemod->setBandwidth(*bwPtr); }
            } else {
                if (_this->ssbDemod) { _this->ssbDemod->setBandwidth(*bwPtr); }
            }
            if (_this->vfo) { _this->vfo->setBandwidth(*bwPtr); }
            config.acquire();
            config.conf[_this->name][(_this->mode == DM_NFM) ? "bandwidth_nfm" : "bandwidth_ssb"] = *bwPtr;
            config.release(true);
        }

        // === NFM option: audio low-pass ===
        if (_this->mode == DM_NFM) {
            if (ImGui::Checkbox(CONCAT("Low-pass filter (NFM)##drm_lp_", _this->name), &_this->nfmLowPass)) {
                if (_this->fmDemod) { _this->fmDemod->setLowPass(_this->nfmLowPass); }
                config.acquire();
                config.conf[_this->name]["nfmLowPass"] = _this->nfmLowPass;
                config.release(true);
            }
        }

        // State snapshot
        drm::Status st = _this->decoder.getStatus();

        ImGui::Separator();

        // === SNR / WMER gauge ===
        char snrStr[48];
        if (st.snrAvailable) {
            snprintf(snrStr, sizeof(snrStr), "SNR %.1f dB  |  WMER %.1f dB", st.avgSNR_dB, st.wmer_dB);
        } else {
            snprintf(snrStr, sizeof(snrStr), "SNR --  |  WMER --");
        }
        float norm = std::min(1.0f, std::max(0.0f, (st.avgSNR_dB + 5.0f) / 35.0f));
        ImGui::ProgressBar(st.snrAvailable ? norm : 0.0f, ImVec2(menuWidth, 0), snrStr);

        // === Sync indicators ===
        ImGui::BeginGroup();
        syncDot("Time", st.timeSync);
        ImGui::SameLine(menuWidth * 0.5f);
        syncDot("Freq.", st.freqSync);
        syncDot("Frame", st.frameSync);
        ImGui::SameLine(menuWidth * 0.5f);
        syncDot("FAC", st.facAvailable);
        ImGui::EndGroup();

        // === Robustness mode / occupancy ===
        ImGui::Text("Robust.: ");
        ImGui::SameLine();
        ImGui::TextColored(st.frameSync ? ImVec4(0.40f, 0.80f, 1.0f, 1.0f)
                                        : ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                           "%s", robustnessName(st.robustness));
        ImGui::SameLine(menuWidth * 0.5f);
        ImGui::Text("Occ.: %s", occupancyName(st.spectrumOcc));

        // === OFDM frame counter (free-running) + sync time ===
        ImGui::Text("Frames decoded: ");
        ImGui::SameLine();
        ImGui::TextColored(st.frameSync ? ImVec4(0.85f, 0.85f, 0.40f, 1.0f)
                                        : ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                           "%lu", (unsigned long)st.frameCounter);

        // Time elapsed since first frame-sync (resets when sync is lost)
        {
            using clk = std::chrono::steady_clock;
            static clk::time_point syncStart;
            static bool wasFrameSync = false;
            if (st.frameSync && !wasFrameSync) { syncStart = clk::now(); }
            wasFrameSync = st.frameSync;
            if (st.frameSync) {
                int sec = (int)std::chrono::duration<float>(clk::now() - syncStart).count();
                ImGui::Text("Sync time:");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.40f, 1.0f),
                                   "%dm %02ds", sec / 60, sec % 60);
            } else {
                ImGui::TextDisabled("Sync time: --");
            }
        }

        // === Reception progress ===
        if (st.totalSegments > 0) {
            using clk = std::chrono::steady_clock;
            static int     lastTotal       = 0;
            static int     lastSegSnapshot = 0;
            static clk::time_point startTs = clk::now();
            if (st.totalSegments != lastTotal || st.rxSegments < lastSegSnapshot) {
                startTs = clk::now();
                lastTotal = st.totalSegments;
            }
            lastSegSnapshot = st.rxSegments;

            float elapsed = std::chrono::duration<float>(clk::now() - startTs).count();
            float pct = (float)st.rxSegments / (float)st.totalSegments;
            float eta = (st.rxSegments > 1 && pct < 0.999f)
                            ? elapsed * (1.0f / pct - 1.0f)
                            : 0.0f;
            char buf[64];
            if (eta > 0.0f && st.rxSegments < st.totalSegments) {
                snprintf(buf, sizeof(buf), "%d / %d seg (ETA ~ %.0fs, elapsed %.0fs)",
                         st.rxSegments, st.totalSegments, eta, elapsed);
            } else if (st.rxSegments >= st.totalSegments) {
                snprintf(buf, sizeof(buf), "%d / %d seg (done, %.0fs)",
                         st.rxSegments, st.totalSegments, elapsed);
            } else {
                snprintf(buf, sizeof(buf), "%d / %d seg (%.0fs)",
                         st.rxSegments, st.totalSegments, elapsed);
            }
            ImGui::ProgressBar(pct, ImVec2(menuWidth, 0), buf);

            if (st.objectName[0]) {
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.55f, 1.0f),
                                   "Receiving: %s", st.objectName);
                if (st.bodySize > 0) {
                    ImGui::Text("(%d bytes, segment = %d B)",
                                st.bodySize, st.segmentSize);
                }
            }
        } else if (st.frameSync) {
            ImGui::TextDisabled("Waiting for MOT object...");
        }

        ImGui::Separator();

        // === Constellation des cellules OFDM ===
        ImGui::TextUnformatted("Constellation");
        {
            std::complex<float> tmp[1024];
            int n = _this->decoder.getConstellation(tmp, 1024);
            dsp::complex_t* cbuf = _this->constellation.acquireBuffer();
            for (int i = 0; i < 1024; i++) {
                if (i < n) { cbuf[i].re = tmp[i].real(); cbuf[i].im = tmp[i].imag(); }
                else       { cbuf[i].re = 0.0f;          cbuf[i].im = 0.0f; }
            }
            _this->constellation.releaseBuffer();
        }
        _this->constellation.draw(ImVec2(menuWidth, menuWidth));

        ImGui::Separator();

        // === Image reçue ====================================================
        // === Received image ====================================================
        // If a new image is available, decode it and (re)create ImageDisplay
        // at the correct dimensions, then transfer pixels.
        if (_this->decoder.hasNewImage()) {
            _this->refreshImage();   // safe to call from the UI thread
        }

        ImGui::TextUnformatted("Image");
        if (_this->imageTexId != 0 && _this->imageW > 0 && _this->imageH > 0) {
            float drawH = menuWidth * (float)_this->imageH / (float)_this->imageW;
            // Cap aspect-ratio so a very tall image doesn't take half the screen
            if (drawH > menuWidth * 1.5f) { drawH = menuWidth * 1.5f; }
            ImGui::Image((ImTextureID)(intptr_t)_this->imageTexId,
                         ImVec2(menuWidth, drawH));
        } else {
            // Empty placeholder if nothing received yet — dark slate rectangle
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = menuWidth * (float)IMG_H / (float)IMG_W;
            ImGui::GetWindowDrawList()->AddRectFilled(
                p, ImVec2(p.x + menuWidth, p.y + h),
                IM_COL32(30, 30, 46, 255));
            ImGui::Dummy(ImVec2(menuWidth, h));
            // Centered "Waiting..." label
            const char* msg = st.frameSync ? "Waiting for image..." : "No signal";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(p.x + (menuWidth - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                IM_COL32(150, 150, 170, 255), msg);
        }

        // Image metadata
        if (!_this->imageFilename.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                               "Received: %s", _this->imageFilename.c_str());
            ImGui::Text("(%d x %d px, format: %s)",
                        _this->imageW, _this->imageH, _this->imageFormat.c_str());
        }

        // Diagnostics (always shown — helps debug "decoded but no image" cases)
        ImVec4 diagCol(0.6f, 0.6f, 0.7f, 1.0f);
        ImGui::TextColored(diagCol, "Files captured: %lu  |  decode fails: %lu",
                           (unsigned long)st.filesReceived,
                           (unsigned long)st.imageDecodeFailures);

        // === Save folder selector ===
        ImGui::LeftLabel("Folder");
        ImGui::FillWidth();
        if (_this->folderSelect.render(CONCAT("##drm_folder_", _this->name))) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["savePath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        // Save buttons
        bool canSave = !_this->lastImageBytes.empty() && _this->folderSelect.pathIsValid();
        if (!canSave) { style::beginDisabled(); }
        float bw = (menuWidth - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
        if (ImGui::Button(CONCAT("BMP##drm_bmp_", _this->name), ImVec2(bw, 0))) {
            _this->saveImageAs("bmp");
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("PNG##drm_png_", _this->name), ImVec2(bw, 0))) {
            _this->saveImageAs("png");
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("JPEG##drm_jpg_", _this->name), ImVec2(bw, 0))) {
            _this->saveImageAs("jpeg");
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Raw##drm_raw_", _this->name), ImVec2(bw, 0))) {
            _this->saveImageAs("raw");
        }
        if (!canSave) { style::endDisabled(); }

        // Save feedback message
        if (!_this->lastSaveMsg.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.95f, 1.0f),
                               "%s", _this->lastSaveMsg.c_str());
        }

        if (!_this->enabled) { style::endDisabled(); }

        ImGui::TextDisabled("HamDRM: acquisition + FAC + image (JP2/JPEG/PNG/BMP).");
    }

    // --- Membres -----------------------------------------------------------

    std::string name;
    bool enabled = true;
    bool chainAlive = false;

    // Réglages
    OptionList<std::string, DemodMode> modes;
    int       modeId = DM_USB;
    DemodMode mode   = DM_USB;
    bool      invertSpectrum = false;
    bool      nfmLowPass     = true;
    float     ssbBandwidth   = 2700.0f;
    float     nfmBandwidth   = 12500.0f;

    // Chaîne DSP — reconstruite à chaque switch de mode pour éviter le
    // double init() des Processor (qui ré-enregistre les streams).
    VFOManager::VFO* vfo = nullptr;
    std::unique_ptr<dsp::demod::SSB<float>>                    ssbDemod;
    std::unique_ptr<dsp::demod::FM<float>>                     fmDemod;
    std::unique_ptr<dsp::multirate::RationalResampler<float>>  resamp;
    std::unique_ptr<dsp::sink::Handler<float>>                 sink;
    drm::DRMDecoder decoder;

    // Affichage
    ImGui::ConstellationDiagram constellation;
    GLuint imageTexId = 0;          // Texture OpenGL gérée directement
    int   imageW = 0;
    int   imageH = 0;
    std::string imageFilename;
    std::string imageFormat;
    std::vector<uint8_t> lastImageBytes;
    std::vector<uint8_t> lastDecodedRGBA;
    std::string lastSaveMsg;
    FolderSelect folderSelect{ "%ROOT%/drm_images" };
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/drm_image_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new DRMImageDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (DRMImageDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
