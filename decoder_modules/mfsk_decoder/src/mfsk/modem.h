#pragma once
// FLDIGI-faithful MFSK modem. Tone layout, interleaver, dual-Viterbi FEC and
// IZ8BLY varicode all match fldigi (validated by decoding a real MFSK16 .wav).
// The front-end (tone correlation + symbol-clock sync) runs at the 48 kHz VFO
// rate directly; the FLDIGI back-end is sample-rate independent.
#include <vector>
#include <complex>
#include <cmath>
#include <functional>
#include <string>
#include <cstdint>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "viterbi.h"
#include "interleave.h"
#include "varicode.h"

namespace mfsk {

static constexpr double AUDIO_SR = 48000.0;   // modem runs at the VFO audio rate

struct MFSKMode {
    const char* name;
    int    samplerate;  // FLDIGI native rate -> defines tonespacing = samplerate/symlen
    int    symlen;      // FLDIGI symbol length (samples) at that native rate
    int    symbits;     // bits per tone symbol
    int    depth;       // interleaver depth (cascade of symbits x symbits stages)
    int    numtones;
};

// Index order is part of the saved config / combo box -> do not reorder.
static const MFSKMode MFSK_MODES[] = {
    {"MFSK4",    8000, 2048, 5,   5, 32},
    {"MFSK8",    8000, 1024, 5,   5, 32},
    {"MFSK11",  11025, 1024, 4,  10, 16},
    {"MFSK16",   8000,  512, 4,  10, 16},
    {"MFSK22",  11025,  512, 4,  10, 16},
    {"MFSK31",   8000,  256, 3,  10,  8},
    {"MFSK32",   8000,  256, 4,  10, 16},
    {"MFSK64",   8000,  128, 4,  10, 16},
    {"MFSK128",  8000,   64, 4,  20, 16},
    {"MFSK64L",  8000,  128, 4, 400, 16},
    {"MFSK128L", 8000,   64, 4, 800, 16},
};
static constexpr int MFSK_MODE_COUNT = 11;

class MFSKModem {
public:
    MFSKModem() : dec1(7, 0x6d, 0x4f), dec2(7, 0x6d, 0x4f) {
        dec1.settraceback(45); dec2.settraceback(45);
        dec1.setchunksize(1);  dec2.setchunksize(1);
        scanThread = std::thread(&MFSKModem::scanLoop, this);
        configure(3, 1000.0);
    }
    ~MFSKModem() {
        { std::lock_guard<std::mutex> lk(scanMtx); scanQuit = true; }
        scanCv.notify_one();
        if (scanThread.joinable()) scanThread.join();
    }
    MFSKModem(const MFSKModem&) = delete;
    MFSKModem& operator=(const MFSKModem&) = delete;

    void configure(int idx, double af) {
        modeIdx = std::clamp(idx, 0, MFSK_MODE_COUNT - 1);
        const MFSKMode& M = MFSK_MODES[modeIdx];
        N           = M.numtones;
        symbits     = M.symbits;
        tonespacing = (double)M.samplerate / M.symlen;       // Hz
        L           = (int)std::lround(AUDIO_SR / tonespacing); // window @ 48 kHz
        if (L < 8) L = 8;
        hop         = std::max(1, L / OS);
        afFreq      = af;
        // Auto-AFC only works where the tones are resolvable at the correlation
        // window. The very slow modes (MFSK4/8: 32 tones ~4/8 Hz apart) are not,
        // so they use the manual AF and must decode in real time (no capture).
        autoCapable = !(tonespacing < 10.0 || N > 16);
        buildOsc();

        win.assign(L, 0.0f); wp = 0;
        specWin.assign(L, 0.0f); swp = 0; specHop = 0;
        specMag.clear(); specCos.clear();   // rebuilt lazily for the new L/band
        subEnergy.assign(OS, 0.0); subBins.assign(OS, std::vector<std::complex<float>>(N));
        sub = 0; hopcnt = 0;

        rxinlv.init(symbits, M.depth, MFSK_INTERLEAVE_REV);
        dec1.reset(); dec2.reset();
        datashreg = 1; symcounter = 0; met1 = 0.0; met2 = 0.0;
        symbolpair[0] = symbolpair[1] = 0;

        curTone = 0; curSNR = 0.0f;
        for (int i = 0; i < SCOPE_LEN; i++) scopeRing[i] = std::complex<float>(0, 0);
        scopePos = 0; scopeMax = 1e-6f;

        // AFC state
        afcMetric = 0.0; freqErr = 0.0; prevSymbol = -1;
        prevVec = std::complex<float>(0, 0);
        baseAF = afFreq;
        acquired = false; acqFeed = 0; lockScore = 0.0;
        resultReady.store(false);
        replayBuf.clear(); replayBuf.shrink_to_fit();
        replayDone = !(afcEnabled && autoCapable);  // capture only when auto-AFC is operative
        replaying = false; replayPos = 0;
        initAcq();
    }

    void setAFFreq(double f) { afFreq = f; baseAF = f; freqErr = 0.0; buildOsc(); }

    // Automatic Frequency Control (ported from fldigi mfsk::afc()).
    void setAFC(bool en) {
        afcEnabled = en;
        if (en && autoCapable) {
            acquired = false; acqFeed = 0; lockScore = 0.0;
            replayBuf.clear(); replayBuf.shrink_to_fit();
            replayDone = false; replaying = false; replayPos = 0;
            initAcq();
        } else {
            replayDone = true;             // manual / non-auto mode: decode in real time
            replaying = false; replayPos = 0;
            replayBuf.clear(); replayBuf.shrink_to_fit();
        }
    }
    bool getAFC() const { return afcEnabled; }
    // Effective tone-0 frequency the modem is currently tracking.
    double getTrackedAF() const { return baseAF + freqErr; }
    double lastAcqScore() const { return lastScore; }

private:
    // ---- automatic acquisition ---------------------------------------------
    // We buffer a few seconds of audio, then scan candidate tone-0 frequencies
    // across the SSB passband. For each candidate we measure, per symbol, the
    // dominant-tone peak/average (confidence) and which tone wins; the best
    // candidate maximises confidence x tone-occupancy-entropy. A real MFSK
    // signal visits all N tones (high entropy) with a sharp peak, so this
    // rejects single-tone carriers and noise. Validated on real on-air signals.
public:
    static void testScan(const std::vector<float>&b,int L,int N,double ts,double&af,double&sc){scanBuf(b,L,N,ts,af,sc);}
    void initAcq() {
        tuneBuf.clear();
        tuneBuf.reserve((size_t)TUNE_SYMS * L);
    }
    // Snapshot the current rolling buffer and wake the background scanner. Cheap:
    // a single vector copy on the audio thread, no DSP. The heavy scan runs in
    // scanLoop() on its own thread so the audio path (and waterfall) never stall.
    void requestScan() {
#ifdef MFSK_SYNC_SCAN
        { std::vector<float> snap=tuneBuf; double af=0,sc=-1; bool slow=(tonespacing<10.0||N>16); if(!slow) scanBuf(snap,L,N,tonespacing,af,sc); resultAF.store(af); resultScore.store(sc); resultReady.store(true); }
        return;
#endif
        {
            std::lock_guard<std::mutex> lk(scanMtx);
            if (scanBusy.load()) return;
            scanSnap = tuneBuf;          // copy current audio window
            scanL = L; scanN = N; scanTs = tonespacing;
            scanSlow = (tonespacing < 10.0 || N > 16);
            scanReq = true;
            scanBusy.store(true);
        }
        scanCv.notify_one();
    }
    // two-stage scan over a snapshot buffer: coarse bin-grid then fine correlation
    static void scanBuf(const std::vector<float>& buf, int L, int N, double ts,
                        double& outAf, double& outScore) {
        int nsym = (int)(buf.size() / L);
        if (nsym < 8) { outAf = 0; outScore = -1; return; }
        int skip = (int)(nsym * 0.35); if (skip < 2) skip = 2;
        int useSym = nsym - skip;
        if (useSym < 6) { skip = 0; useSym = nsym; }

        double FLO = 300.0, FHI = 2600.0;
        int bLo = (int)(FLO / ts), bHi = (int)(FHI / ts);
        int nbin = bHi + N + 2;
        int coarseSym = useSym > 40 ? 40 : useSym;   // cap coarse-stage cost
        std::vector<std::vector<float>> mags(coarseSym, std::vector<float>(nbin, 0.0f));
        for (int i = 0; i < coarseSym; i++) {
            const float* seg = &buf[(size_t)(skip + i) * L];
            for (int b = bLo; b < nbin; b++) {
                double w = 2.0 * M_PI * (b * ts) / AUDIO_SR;
                double cw = 2.0 * cos(w), s1 = 0, s2 = 0;
                for (int t = 0; t < L; t++) { double s0 = seg[t] + cw*s1 - s2; s2 = s1; s1 = s0; }
                double p = s1*s1 + s2*s2 - cw*s1*s2;
                mags[i][b] = (float)(p > 0 ? std::sqrt(p) : 0.0);
            }
        }
        double bestSc = -1; int bestB = bLo;
        for (int b0 = bLo; b0 + N <= nbin; b0++) {
            double conf = 0; std::vector<int> hist(N, 0);
            for (int i = 0; i < coarseSym; i++) {
                double mx = 0, sum = 0; int pk = 0;
                for (int k = 0; k < N; k++) { float m = mags[i][b0+k]; if (m > mx) { mx = m; pk = k; } sum += m; }
                conf += mx / (sum / N + 1e-9); hist[pk]++;
            }
            conf /= coarseSym;
            double ent = 0; for (int k = 0; k < N; k++) if (hist[k]) { double p = (double)hist[k]/coarseSym; ent -= p*log(p); }
            ent /= log((double)N);
            double s = conf * ent;
            if (s > bestSc) { bestSc = s; bestB = b0; }
        }
        double coarse = bestB * ts;
        double bestAf = coarse, fineSc = -1;
        // fine search is the costliest part: use a coarser AF step and fewer
        // symbols (enough for a stable metric) to keep the scan fast.
        int fineSym = useSym > 36 ? 36 : useSym;
        for (double af = coarse - 2.0*ts; af <= coarse + 2.0*ts + 1e-6; af += ts/8.0) {
            double s = scoreBuf(buf, L, N, ts, af, skip, fineSym);
            if (s > fineSc * 1.001) { fineSc = s; bestAf = af; }
        }
        outAf = bestAf; outScore = fineSc;
    }
    // confidence (mean peak/avg) x normalised tone-occupancy entropy at AF
    static double scoreBuf(const std::vector<float>& buf, int L, int N, double ts,
                           double af, int start, int nsym) {
        std::vector<int> hist(N, 0);
        double conf = 0; int cnt = 0;
        for (int s = start; s < start + nsym; s++) {
            const float* seg = &buf[(size_t)s * L];
            double mx = 0, sum = 0; int pk = 0;
            for (int k = 0; k < N; k++) {
                double fk = af + k * ts, w = 2.0 * M_PI * fk / AUDIO_SR;
                double re = 0, im = 0;
                for (int t = 0; t < L; t++) { double ph = w * t; re += seg[t]*cos(ph); im -= seg[t]*sin(ph); }
                double mag = std::sqrt(re*re + im*im);
                if (mag > mx) { mx = mag; pk = k; }
                sum += mag;
            }
            conf += mx / (sum / N + 1e-9); hist[pk]++; cnt++;
        }
        if (cnt == 0) return 0;
        conf /= cnt;
        double ent = 0; for (int k = 0; k < N; k++) if (hist[k]) { double p = (double)hist[k]/cnt; ent -= p*log(p); }
        ent /= log((double)N);
        return conf * ent;
    }
    // background scanner thread
    void scanLoop() {
        std::vector<float> snap; int sL, sN; double sTs; bool slow;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(scanMtx);
                scanCv.wait(lk, [&]{ return scanReq || scanQuit; });
                if (scanQuit) return;
                scanReq = false;
                snap.swap(scanSnap); sL = scanL; sN = scanN; sTs = scanTs; slow = scanSlow;
            }
            double af = 0, sc = -1;
            if (slow) { sc = -1; }                       // slow modes: no auto, keep manual
            else scanBuf(snap, sL, sN, sTs, af, sc);
            resultAF.store(af); resultScore.store(sc);
            resultReady.store(true);
            scanBusy.store(false);
        }
    }
public:

    // Feed real audio (48 kHz); emit() is called with each decoded ASCII char.
    void process(const float* audio, int count, const std::function<void(char)>& emit) {
        // Apply any acquisition result the background worker has published.
        bool justLocked = false;
        if (afcEnabled && autoCapable && resultReady.exchange(false)) {
            double af = resultAF.load();
            double sc = resultScore.load();
            // decay the held confidence each time a fresh scan completes, so a
            // better (or equal) window can always take over the lock
            lockScore *= LOCK_DECAY;
            if (sc >= ACQ_THRESHOLD && sc >= lockScore) {
                if (!acquired || std::fabs(af - baseAF) > tonespacing / 32.0) {
                    baseAF = af; freqErr = 0.0; afFreq = baseAF; buildOsc();
                }
                if (!acquired) justLocked = true;   // first successful lock
                acquired = true; lockScore = sc;
            }
        }

        // On the FIRST lock, arrange to replay everything captured since
        // reception started (at the now-correct frequency) so the beginning of
        // the transmission is not lost. The replay is done incrementally below
        // (a bounded number of samples per call) to avoid a one-shot CPU burst
        // that would hitch the waterfall.
        if (justLocked && !replayBuf.empty()) {
            replaying = true; replayPos = 0;
            resetDecodeState();
        }

        // Drain part of the replay buffer each call (catches up the start of the
        // transmission over a few audio blocks instead of all at once).
        if (replaying) {
            int budget = REPLAY_CHUNK;
            while (replayPos < replayBuf.size() && budget-- > 0)
                decodeSample(replayBuf[replayPos++], emit);
            if (replayPos >= replayBuf.size()) {
                replaying = false; replayDone = true;
                replayBuf.clear(); replayBuf.shrink_to_fit();
            }
        }

        for (int s = 0; s < count; s++) {
            bool autoAfc = afcEnabled && autoCapable;

            // Keep the GUI band spectrum live regardless of decode/capture state,
            // so the user always sees the trace (and can align the AF on it).
            specWin[swp] = audio[s]; swp = (swp + 1) % L;
            if (++specHop >= hop * SPEC_DECIM) { specHop = 0; updateBandSpectrum(); }

            // Rolling buffer for (re)acquisition (only when auto-AFC is operative).
            if (autoAfc) {
                tuneBuf.push_back(audio[s]);
                size_t cap = (size_t)TUNE_SYMS * L;
                if (tuneBuf.size() > cap) tuneBuf.erase(tuneBuf.begin(), tuneBuf.begin() + L);
                acqFeed++;
            }

            // Before the first lock, capture audio for later replay instead of
            // decoding it (decoding now, at the wrong frequency, would be garbage).
            if (autoAfc && !replayDone) {
                if (replayBuf.size() < (size_t)(REPLAY_MAX_SEC * (int)AUDIO_SR)) {
                    replayBuf.push_back(audio[s]);
                } else if (!replaying) {
                    // capture window exhausted without a lock: give up on replay
                    // and start decoding live so we are not silent indefinitely.
                    replayDone = true;
                    replayBuf.clear(); replayBuf.shrink_to_fit();
                }
                if (tuneBuf.size() >= (size_t)(ACQ_SYMS * L)
                    && acqFeed >= (size_t)(RESCAN_SYMS * L) && !scanBusy.load()) {
                    acqFeed = 0; requestScan();
                }
                continue;
            }

            // Locked, or AFC off, or a manual-only mode (MFSK4/8): decode live.
            if (autoAfc
                && tuneBuf.size() >= (size_t)(ACQ_SYMS * L)
                && acqFeed >= (size_t)(RESCAN_SYMS * L)
                && !scanBusy.load()) {
                acqFeed = 0;
                requestScan();
            }
            decodeSample(audio[s], emit);
        }
    }

private:
    // One audio sample through the front-end correlator + symbol decoder.
    void decodeSample(float x, const std::function<void(char)>& emit) {
        win[wp] = x; wp = (wp + 1) % L;
        if (++hopcnt < hop) return;
        hopcnt = 0;

        double e = 0;
        std::vector<std::complex<float>>& bins = subBins[sub];
        for (int k = 0; k < N; k++) {
            double re = 0, im = 0; int idx = wp;
            for (int m = 0; m < L; m++) { float v = win[idx]; re += v*oc[k][m]; im += v*osn[k][m]; if (++idx >= L) idx = 0; }
            bins[k] = std::complex<float>((float)re, (float)im);
            double mag = std::sqrt(re*re + im*im);
            if (mag > e) e = mag;
        }
        subEnergy[sub] = 0.90 * subEnergy[sub] + 0.10 * e;

        if (++sub >= OS) {
            sub = 0;
            int bestp = 0; double bm = -1;
            for (int p = 0; p < OS; p++) if (subEnergy[p] > bm) { bm = subEnergy[p]; bestp = p; }
            processSymbol(subBins[bestp], emit);
        }
    }
    // Light Goertzel bank over the audio passband, smoothed, for the GUI band
    // display (lets the user see the MFSK trace and align the AF on its edge).
    void updateBandSpectrum() {
        // analysis length: capped so the cost stays low (resolution ~ AUDIO_SR/SL)
        int SL = std::min(L, SPEC_ANALYSIS_LEN);
        if (specCos.empty()) {
            specMag.assign(SPEC_BINS, 0.0f);
            specCos.resize(SPEC_BINS);
            for (int b = 0; b < SPEC_BINS; b++) {
                double f = SPEC_FLO + b * ((SPEC_FHI - SPEC_FLO) / (SPEC_BINS - 1));
                specCos[b] = (float)(2.0 * cos(2.0 * M_PI * f / AUDIO_SR));
            }
        }
        // start SL samples back from the newest sample in specWin
        int start = (swp - SL); while (start < 0) start += L;
        for (int b = 0; b < SPEC_BINS; b++) {
            double cw = specCos[b], s1 = 0, s2 = 0; int idx = start;
            for (int m = 0; m < SL; m++) { double s0 = specWin[idx] + cw*s1 - s2; s2 = s1; s1 = s0; if (++idx >= L) idx = 0; }
            double p = s1*s1 + s2*s2 - cw*s1*s2;
            float mag = (float)(p > 0 ? std::sqrt(p) : 0.0);
            specMag[b] = 0.6f * specMag[b] + 0.4f * mag;   // smooth
        }
    }
    // Reset the decoder front-end + FEC state (used before replaying the buffer).
    void resetDecodeState() {
        for (auto& w : win) w = 0.0f; wp = 0; sub = 0; hopcnt = 0;
        for (auto& se : subEnergy) se = 0.0;
        rxinlv.init(symbits, MFSK_MODES[modeIdx].depth, MFSK_INTERLEAVE_REV);
        dec1.reset(); dec2.reset();
        datashreg = 1; symcounter = 0; met1 = 0.0; met2 = 0.0;
        symbolpair[0] = symbolpair[1] = 0;
        prevSymbol = -1; prevVec = std::complex<float>(0, 0);
    }
public:

    int   getTone() const { return curTone; }
    float getSNR()  const { return curSNR; }

    // GUI band display: smoothed magnitude spectrum over [SPEC_FLO, SPEC_FHI].
    static constexpr int    SPEC_BINS = 160;
    static constexpr double SPEC_FLO  = 300.0;
    static constexpr double SPEC_FHI  = 2700.0;
    static constexpr int    SPEC_ANALYSIS_LEN = 1536; // Goertzel length (res ~31 Hz)
    static constexpr int    SPEC_DECIM = 4;           // update every 4 hops
    int  getBandSpectrum(float* out, int n) const {
        int c = std::min(n, (int)specMag.size());
        for (int i = 0; i < c; i++) out[i] = specMag.empty() ? 0.0f : specMag[i];
        return c;
    }
    double getBandFlo() const { return SPEC_FLO; }
    double getBandFhi() const { return SPEC_FHI; }
    // tone-0 (left edge) and full tone-span width, in Hz, for the marker overlay
    double getEdgeFreq() const { return baseAF + freqErr; }
    double getToneSpan() const { return (N - 1) * tonespacing; }

    static constexpr int SCOPE_LEN = 1024;
    void getScope(std::complex<float>* out, int n) const {
        int c = (n < SCOPE_LEN) ? n : SCOPE_LEN;
        for (int i = 0; i < c; i++) out[i] = scopeRing[i];
    }

private:
    static constexpr int OS = 16;   // symbol-clock oversampling (sub-phases / symbol)

    void buildOsc() {
        oc.assign(N, std::vector<double>(L));
        osn.assign(N, std::vector<double>(L));
        for (int k = 0; k < N; k++) {
            double fk = afFreq + k * tonespacing;
            for (int m = 0; m < L; m++) { double ph = 2.0*M_PI*fk*m / AUDIO_SR; oc[k][m] = cos(ph); osn[k][m] = -sin(ph); }
        }
    }

    void processSymbol(std::vector<std::complex<float>>& bins, const std::function<void(char)>& emit) {
        // hard decision (max magnitude tone) + afcmetric / staticburst (fldigi harddecode)
        int currsymbol = 0; double mx = 0, avg = 0; int burstcount = 0;
        for (int i = 0; i < N; i++) avg += std::abs(bins[i]);
        avg /= (N > 0 ? N : 1); if (avg < 1e-20) avg = 1e-20;
        for (int i = 0; i < N; i++) {
            double x = std::abs(bins[i]);
            if (x > mx) { mx = x; currsymbol = i; }
            if (x > 2.0 * avg) burstcount++;
        }
        bool staticburst = (burstcount == N);
        afcMetric = staticburst ? 0.0 : (0.95 * afcMetric + 0.05 * (2.0 * mx / avg));
        curTone = currsymbol;

        // crude SNR for the GUI
        double sumAll = avg * N;
        double noise = (sumAll - mx) / (double)(N > 1 ? N - 1 : 1);
        curSNR = (noise > 1e-12) ? (float)(20.0 * log10(mx / noise)) : 0.0f;

        // --- AFC: track residual frequency error from inter-symbol phase of the
        //     dominant tone (fldigi mfsk::afc, adapted: our correlator is already
        //     centred on afFreq+k*tonespacing, so the phase slope IS the error). --
        // --- AFC: coarse spectral acquisition handles the bulk pull-in. A fine
        //     phase-tracking loop is intentionally NOT applied here: the symbol
        //     correlator already tolerates the small residual (+/- ~tonespacing/2),
        //     and an inter-symbol phase estimator is ill-posed on this front-end
        //     (it drifts on steady carriers). Acquisition + that tolerance give
        //     reliable automatic locking without degrading a correct signal. -----
        (void)mx; // afcMetric available if needed
        prevSymbol = currsymbol;
        prevVec = bins[currsymbol];

        // scope: dominant-tone vector, AGC normalised
        {
            std::complex<float> v = bins[currsymbol];
            float m = std::abs(v);
            scopeMax = std::max(scopeMax * 0.9995f, m);
            scopeRing[scopePos] = (scopeMax > 1e-9f) ? v / scopeMax : std::complex<float>(0, 0);
            scopePos = (scopePos + 1) % SCOPE_LEN;
        }

        // soft decode (FLDIGI: per-bit correlation across gray-mapped tones,
        // dominant tone gets a 2x vote)
        double b[8] = {0};
        double sum = sumAll; if (sum < 1e-10) sum = 1e-10;
        for (int i = 0; i < N; i++) {
            int j = mfsk_graydecode((unsigned char)i);
            double binmag = (i == currsymbol) ? 2.0 * std::abs(bins[i]) : std::abs(bins[i]);
            for (int k = 0; k < symbits; k++) b[k] += (j & (1 << (symbits - k - 1))) ? binmag : -binmag;
        }
        unsigned char symbols[8];
        for (int i = 0; i < symbits; i++) {
            double v = 128.0 + (b[i] / sum * 256.0);
            symbols[i] = (unsigned char)std::clamp(v, 0.0, 255.0);
        }
        rxinlv.symbols(symbols);
        for (int i = 0; i < symbits; i++) decodesymbol(symbols[i], emit);
    }

    void decodesymbol(unsigned char symbol, const std::function<void(char)>& emit) {
        int c, met;
        symbolpair[0] = symbolpair[1]; symbolpair[1] = symbol;
        symcounter = symcounter ? 0 : 1;
        if (symbits == 5 || symbits == 3) {           // odd: run both phases, keep best metric
            if (symcounter) {
                if ((c = dec1.decode(symbolpair, &met)) == -1) return;
                met1 = decayavg(met1, met, 50); if (met1 < met2) return;
            } else {
                if ((c = dec2.decode(symbolpair, &met)) == -1) return;
                met2 = decayavg(met2, met, 50); if (met2 < met1) return;
            }
        } else {                                      // even: one decoder, every 2nd channel bit
            if (symcounter) return;
            if ((c = dec2.decode(symbolpair, &met)) == -1) return;
            met2 = decayavg(met2, met, 50);
        }
        recvbit(c, emit);
    }

    void recvbit(int bit, const std::function<void(char)>& emit) {
        datashreg = (datashreg << 1) | (bit ? 1u : 0u);
        if ((datashreg & 7) == 1) {
            int c = mfsk_varidec(datashreg >> 1);
            if (c > 0 && c < 256) emit((char)c);
            datashreg = 1;
        }
    }

    static inline double decayavg(double avg, double in, int w) { return (w <= 1) ? in : (in - avg)/(double)w + avg; }

    // mode / front-end
    int modeIdx = 3, N = 16, symbits = 4, L = 3072, hop = 192;
    double tonespacing = 15.625, afFreq = 1000.0;
    std::vector<std::vector<double>> oc, osn;
    std::vector<float> win; int wp = 0;
    std::vector<double> subEnergy;
    std::vector<std::vector<std::complex<float>>> subBins;
    int sub = 0, hopcnt = 0;

    // back-end (FLDIGI)
    Viterbi dec1, dec2;
    Interleaver rxinlv;
    unsigned int datashreg = 1; int symcounter = 0; double met1 = 0, met2 = 0;
    unsigned char symbolpair[2] = {0, 0};

    // GUI status / scope
    int   curTone = 0; float curSNR = 0.0f;
    std::complex<float> scopeRing[SCOPE_LEN];
    int   scopePos = 0; float scopeMax = 1e-6f;

    // AFC (fldigi-style residual-frequency tracking)
    bool   afcEnabled = true;
    double afcMetric = 0.0;   // signal-present confidence (>=3 => track)
    double freqErr   = 0.0;   // accumulated Hz correction added to baseAF
    double baseAF    = 1000.0;// user-set tone-0 frequency before AFC pull-in
    int    prevSymbol = -1;
    std::complex<float> prevVec = std::complex<float>(0, 0);

    // Automatic acquisition (rolling buffer, quality-gated, re-acquiring)
    static constexpr int    TUNE_SYMS   = 96;    // rolling buffer length (symbols)
    static constexpr int    ACQ_SYMS    = 80;    // min symbols before a scan
    static constexpr int    RESCAN_SYMS = 16;    // retry cadence while unlocked
    static constexpr double ACQ_THRESHOLD = 3.5; // min score to lock (noise ~2, signal >=3.7)
    static constexpr double RELOCK_SCORE  = 5.2; // strong-lock score: stop scanning (RF reaches ~5.5-6)
    static constexpr double LOCK_DECAY    = 0.97;// slow decay so a weak lock keeps being re-challenged
    bool   acquired = false;
    double lastScore = 0.0;
    double lockScore = 0.0;
    size_t acqFeed = 0;
    std::vector<float>  tuneBuf;

    // Replay-on-lock: capture audio before the first AFC lock, then replay it
    // through the decoder once the correct frequency is found, so the start of
    // the transmission is recovered rather than lost during AFC pull-in.
    static constexpr int REPLAY_MAX_SEC = 30;     // cap captured audio (memory bound)
    static constexpr int REPLAY_CHUNK   = 12000;  // replay samples drained per call (>~2x real-time -> converges)
    std::vector<float>  replayBuf;
    bool   replayDone = false;
    bool   replaying  = false;
    size_t replayPos  = 0;
    bool   autoCapable = true;   // false for MFSK4/8 (tones unresolved -> manual AF)

    // GUI band spectrum (Goertzel bank, smoothed)
    std::vector<float> specMag;
    std::vector<float> specCos;
    std::vector<float> specWin;
    int swp = 0, specHop = 0;

    // background scanner (keeps the heavy full-band scan off the audio thread)
    std::thread             scanThread;
    std::mutex              scanMtx;
    std::condition_variable scanCv;
    std::vector<float>      scanSnap;
    int                     scanL = 0, scanN = 0;
    double                  scanTs = 0.0;
    bool                    scanSlow = false;
    bool                    scanReq = false;
    bool                    scanQuit = false;
    std::atomic<bool>       scanBusy{false};
    std::atomic<bool>       resultReady{false};
    std::atomic<double>     resultAF{0.0};
    std::atomic<double>     resultScore{-1.0};
};

} // namespace mfsk
