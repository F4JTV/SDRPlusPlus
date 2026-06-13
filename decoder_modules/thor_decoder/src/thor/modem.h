#pragma once
// FLDIGI-faithful THOR modem (receive path).
//
// THOR is an IFK+ (incremental frequency keying) MFSK mode derived from
// DominoEX, with a rate-1/2 convolutional FEC, a 4x4 diagonal interleaver and
// the IZ8BLY/THOR varicode. This is a direct port of fldigi src/thor/thor.cxx
// (hard-decision path) on to a self-contained, sample-rate-independent class so
// it can be unit-tested without the SDR++ runtime.
//
// The SDR++ front-end delivers real audio at the 48 kHz VFO rate. Each mode is
// run at an internal rate that is an exact integer division of 48 kHz
// (8000 = /6, 12000 = /4, 16000 = /3); the tone spacing and baud are preserved
// in Hz, so the demodulator is equivalent to fldigi running at the mode's
// native 8000/11025/16000 rate.
//
// (C) port: structure & algorithm from Dave Freese W1HKJ et al., GPL.
#include <vector>
#include <memory>
#include <complex>
#include <cmath>
#include <cstring>
#include <functional>
#include <algorithm>
#include "modem_dsp.h"
#include "viterbi.h"
#include "interleave.h"
#include "varicode.h"

namespace thor {

static constexpr double AUDIO_SR     = 48000.0; // VFO audio rate fed to the modem
static constexpr int    THORNUMTONES = 18;
static constexpr double THORBASEFREQ = 1500.0;
static constexpr double THORFIRSTIF  = 2000.0;
static constexpr int    THOR_SCOPE_LEN = 1024;

// NASA K=7 polynomials (low-speed modes) and IEEE K=15 (high-speed modes).
#define THOR_K        7
#define THOR_POLY1    0x6d
#define THOR_POLY2    0x4f
#define THOR_K15      15
#define THOR_K15_P1   044735   /* octal */
#define THOR_K15_P2   063057   /* octal */

struct ThorMode {
    const char* name;
    int    nativeSR;    // fldigi native rate (defines the canonical tone spacing)
    int    symlen;      // fldigi symbol length at the native rate
    int    doublespaced;
    int    depth;       // interleaver depth
    int    k;           // viterbi constraint length (7 or 15)
};

// Order is part of the saved config / combo box -> do not reorder.
// (matches the dropdown: Micro, 4, 5, 8, 11, 16, 22, 32, 44, 56, 25x4, 50x1, 50x2, 100)
static const ThorMode THOR_MODES[] = {
    {"THOR Micro",  8000, 4000, 1,  4,  7},
    {"THOR 4",      8000, 2048, 2, 10,  7},
    {"THOR 5",     11025, 2048, 2, 10,  7},
    {"THOR 8",      8000, 1024, 2, 10,  7},
    {"THOR 11",    11025, 1024, 1, 10,  7},
    {"THOR 16",     8000,  512, 1, 10,  7},
    {"THOR 22",    11025,  512, 1, 10,  7},
    {"THOR 32",     8000,  256, 1, 10,  7},
    {"THOR 44",    11025,  256, 1, 10,  7},
    {"THOR 56",    16000,  290, 1, 10,  7},
    {"THOR 25x4",   8000,  320, 4, 50, 15},
    {"THOR 50x1",   8000,  160, 1, 50, 15},
    {"THOR 50x2",   8000,  160, 2, 50, 15},
    {"THOR 100",    8000,   80, 1, 50, 15},
};
static constexpr int THOR_MODE_COUNT = 14;

class ThorModem {
public:
    ThorModem() { configure(4, 1500.0); }   // default THOR 11
    ~ThorModem() = default;
    ThorModem(const ThorModem&) = delete;
    ThorModem& operator=(const ThorModem&) = delete;

    // --- configuration ------------------------------------------------------
    void configure(int idx, double af) {
        modeIdx = std::clamp(idx, 0, THOR_MODE_COUNT - 1);
        const ThorMode& M = THOR_MODES[modeIdx];
        afFreq = af;

        // internal rate = exact integer division of 48 kHz
        if      (M.nativeSR == 8000)  { internalSR = 8000.0;  decim = 6; }
        else if (M.nativeSR == 11025) { internalSR = 12000.0; decim = 4; }
        else                          { internalSR = 16000.0; decim = 3; }

        doublespaced = M.doublespaced;
        symlen = (int)std::lround((double)M.symlen * internalSR / M.nativeSR);
        if (symlen < 8) symlen = 8;
        tonespacing = internalSR * doublespaced / symlen;
        bandwidth   = THORNUMTONES * tonespacing;

        extones = THORNUMTONES / 2;   // fast-cpu path
        paths   = 5;

        basetone = (int)floor(THORBASEFREQ * symlen / internalSR + 0.5);
        lotone   = basetone - extones * doublespaced;
        hitone   = basetone + THORNUMTONES * doublespaced + extones * doublespaced;
        numbins  = hitone - lotone;
        stride   = paths * numbins;

        // anti-alias decimator 48k -> internalSR (cutoff ~3.4 kHz, well above the
        // <600 Hz THOR signal and the 3 kHz SSB passband edge)
        decimator = std::make_unique<C_FIR_filter>();
        decimator->init_lowpass(128, decim, 3400.0 / AUDIO_SR);

        // Hilbert + sliding-FFT bank
        hilbert = std::make_unique<C_FIR_filter>();
        hilbert->init_hilbert(37, 1);
        binsfft.clear();
        for (int i = 0; i < paths; i++)
            binsfft.push_back(std::make_unique<sfft>(symlen, lotone, hitone));

        syncfilter = std::make_unique<Cmovavg>(8);

        // pipe of sliding-FFT vectors
        twosym = 2 * symlen;
        pipe.assign((size_t)twosym * stride, cmplx(0.0, 0.0));
        pipeptr = 0;

        // FEC + interleaver
        if (M.k == 15) {
            dec = std::make_unique<Viterbi>(THOR_K15, THOR_K15_P1, THOR_K15_P2);
            dec->settraceback(15 * 12);
        } else {
            dec = std::make_unique<Viterbi>(THOR_K, THOR_POLY1, THOR_POLY2);
            dec->settraceback(45);
        }
        dec->setchunksize(1);
        rxinlv = std::make_unique<Interleaver>();
        rxinlv->init(4, M.depth, THOR_INTERLEAVE_REV);

        // running state
        for (int i = 0; i < 9; i++) phase[i] = 0.0;
        synccounter = 0; symcounter = 0;
        currsymbol = prev1symbol = prev2symbol = 0;
        currmag = prev1mag = prev2mag = 0.0;
        datashreg = 1;
        symbolpair[0] = symbolpair[1] = 0;
        sig = noise = 6.0; s2n = 0.0; metric = 0.0;
        staticburst = false; fecConfidence = 0;
        preChk = 0; twoCnt = 0; neg16 = false;

        curTone = 0; curSNR = 0.0f;
        scopeRing.assign(THOR_SCOPE_LEN, cmplx(0, 0));
        scopePos = 0; scopeMax = 1e-6;
        emaSym = 0.0; afcInited = false;
    }

    void setAFFreq(double f) { afFreq = f; afcInited = false; }
    double getTrackedAF() const { return afFreq; }
    void setAFC(bool en) { afcEnabled = en; }
    bool getAFC() const { return afcEnabled; }
    void setReverse(bool r) { reverse = r; }

    int   getTone() const { return curTone; }
    float getSNR()  const { return curSNR; }

    double getToneSpan() const { return THORNUMTONES * tonespacing; }
    double getEdgeFreq() const { return afFreq - 0.5 * bandwidth; }
    double getBandFlo()  const { return 300.0; }
    double getBandFhi()  const { return 2700.0; }

    // Band spectrum: magnitude of the current sliding-FFT bins folded over paths.
    int getBandSpectrum(float* out, int n) const {
        int bins = numbins;
        if (bins <= 0) { for (int i = 0; i < n; i++) out[i] = 0; return n; }
        std::vector<double> mag(bins, 0.0);
        const cmplx* v = &pipe[(size_t)pipeptr * stride];
        for (int b = 0; b < bins; b++) {
            double m = 0;
            for (int k = 0; k < paths; k++) m += std::abs(v[b * paths + k]);
            mag[b] = m / paths;
        }
        for (int i = 0; i < n; i++) {
            double fb = (double)i * (bins - 1) / std::max(1, n - 1);
            int b0 = (int)fb; int b1 = std::min(b0 + 1, bins - 1);
            double t = fb - b0;
            out[i] = (float)(mag[b0] * (1 - t) + mag[b1] * t);
        }
        return n;
    }

    void getScope(std::complex<float>* buf, int n) const {
        for (int i = 0; i < n; i++) {
            int idx = (scopePos - n + i + 2 * THOR_SCOPE_LEN) % THOR_SCOPE_LEN;
            cmplx c = scopeRing[idx];
            double s = (scopeMax > 1e-9) ? scopeMax : 1.0;
            buf[i] = std::complex<float>((float)(c.real() / s), (float)(c.imag() / s));
        }
    }

    // --- main entry: real audio at AUDIO_SR ---------------------------------
    template <typename CharSink>
    void process(const float* audio, int count, CharSink sink) {
        for (int i = 0; i < count; i++) {
            double d;
            if (decimator->Irun((double)audio[i], d)) rxSample(d, sink);
        }
    }

private:
    // first-IF + path mixers (fldigi thor::mixer)
    cmplx mixer(int n, const cmplx& in) {
        double f;
        if (n == 0) f = afFreq - THORFIRSTIF;
        else f = THORFIRSTIF - THORBASEFREQ - bandwidth * 0.5
                 + (internalSR / symlen) * ((double)n / paths);
        cmplx z(cos(phase[n]), sin(phase[n]));
        z *= in;
        phase[n] -= THOR_TWOPI * f / internalSR;
        if (phase[n] < 0) phase[n] += THOR_TWOPI;
        return z;
    }

    template <typename CharSink>
    void rxSample(double s, CharSink& sink) {
        cmplx zref(s, s);
        hilbert->run(zref, zref);
        zref = mixer(0, zref);

        cmplx* pv = &pipe[(size_t)pipeptr * stride];
        for (int k = 0; k < paths; k++) {
            cmplx z = mixer(k + 1, zref);
            binsfft[k]->run(z, pv + k, paths);
        }

        if (--synccounter <= 0) {
            synccounter = symlen;
            currsymbol = harddecode();
            currmag = std::abs(pv[currsymbol]);
            evalS2N();
            decodesymbol(sink);
            synchronize();
            prev2symbol = prev1symbol; prev1symbol = currsymbol;
            prev2mag = prev1mag; prev1mag = currmag;

            // scope: dominant-tone vector
            scopeRing[scopePos] = pv[currsymbol];
            scopeMax = std::max(scopeMax * 0.999, std::abs(pv[currsymbol]));
            scopePos = (scopePos + 1) % THOR_SCOPE_LEN;
            curTone = currsymbol / std::max(1, paths);

            updateAFC();
        }

        if (++pipeptr >= (unsigned)twosym) pipeptr = 0;
    }

    int harddecode() {
        double max = 0.0, avg = 0.0;
        int symbol = 0;
        const cmplx* v = &pipe[(size_t)pipeptr * stride];
        for (int i = 0; i < stride; i++) avg += std::abs(v[i]);
        avg /= std::max(1, stride);
        if (avg < 1e-10) avg = 1e-10;
        for (int i = 0; i < stride; i++) {
            double x = std::abs(v[i]);
            if (x > max) { max = x; symbol = i; }
        }
        staticburst = (max / avg < 1.2);
        return symbol;
    }

    template <typename CharSink>
    void decodesymbol(CharSink& sink) {
        unsigned char symbols[4];
        bool outofrange = false;

        double fdiff = currsymbol - prev1symbol;
        if (reverse) fdiff = -fdiff;
        fdiff /= paths;
        fdiff /= doublespaced;
        if (fabs(fdiff) > 17) outofrange = true;

        int c = (int)floor(fdiff + 0.5);
        if (preambleEnabled && preambledetect(c)) { softflushrx(); return; }
        c -= 2;
        if (c < 0) c += THORNUMTONES;

        if (staticburst || outofrange) {
            symbols[3] = symbols[2] = symbols[1] = symbols[0] = THOR_PUNCTURE;
        } else {
            symbols[3] = (c & 1) ? 255 : 0; c /= 2;
            symbols[2] = (c & 1) ? 255 : 0; c /= 2;
            symbols[1] = (c & 1) ? 255 : 0; c /= 2;
            symbols[0] = (c & 1) ? 255 : 0; c /= 2;
        }
        rxinlv->symbols(symbols);
        for (int i = 0; i < 4; i++) decodePairs(symbols[i], sink);
    }

    template <typename CharSink>
    void decodePairs(unsigned char symbol, CharSink& sink) {
        int met;
        symbolpair[0] = symbolpair[1];
        symbolpair[1] = symbol;
        symcounter = symcounter ? 0 : 1;
        if (symcounter) return;

        int c = dec->decode(symbolpair, &met);
        if (c == -1) return;

        if (met < 255 / 2) fecConfidence -= 2 + fecConfidence / 2;
        else fecConfidence += 2;
        fecConfidence = std::clamp(fecConfidence, 0, 100);

        datashreg = (datashreg << 1) | (c ? 1 : 0);
        if ((datashreg & 7) == 1) {
            int ch = thor_varidec(datashreg >> 1);
            recvchar(ch, sink);
            datashreg = 1;
        }
    }

    template <typename CharSink>
    void recvchar(int c, CharSink& sink) {
        if (c == -1) return;
        if (c & 0x100) return;        // secondary channel (idle filler) - not emitted
        sink((char)(c & 0xFF));
    }

    bool preambledetect(int c) {
        if (twoCnt > 14) twoCnt = 0;
        if (c == -16 && twoCnt > 2) neg16 = true;
        else if (c != 2) neg16 = false;
        else if (c == 2) twoCnt++;
        if (c != -16 && c != 2) if (twoCnt > 1) twoCnt -= 2;
        if (twoCnt > 4 && neg16) { if (++preChk > 4) return true; }
        else preChk = 0;
        return false;
    }

    void softflushrx() {
        unsigned char puncture[2] = {THOR_PUNCTURE, THOR_PUNCTURE};
        unsigned char flush[4] = {THOR_PUNCTURE, THOR_PUNCTURE, THOR_PUNCTURE, THOR_PUNCTURE};
        for (int i = 0; i < 90; i++) rxinlv->symbols(flush);
        for (int j = 0; j < 128; j++) dec->decode(puncture, nullptr);
    }

    void synchronize() {
        if (staticburst) return;
        if (currsymbol == prev1symbol) return;
        if (prev1symbol == prev2symbol) return;
        double max = 0.0; double syn = -1;
        unsigned int j = pipeptr;
        for (unsigned int i = 0; i < (unsigned)twosym; i++) {
            double val = std::abs(pipe[(size_t)j * stride + prev1symbol]);
            if (val > max) { max = val; syn = i; }
            j = (j + 1) % twosym;
        }
        syn = syncfilter->run(syn);
        synccounter += (int)floor(1.0 * (syn - symlen) / THORNUMTONES + 0.5);
    }

    void evalS2N() {
        double s = std::abs(pipe[(size_t)pipeptr * stride + currsymbol]);
        double n = (THORNUMTONES - 1) *
                   std::abs(pipe[(size_t)((pipeptr + symlen) % twosym) * stride + currsymbol]);
        sig   = decayavg(sig, s, (s - sig) > 0 ? 4 : 20);
        noise = decayavg(noise, n, 64);
        s2n = (noise > 0) ? 20 * log10(sig / noise) : 0;
        double m = 6 * (s2n + 18.4);
        metric = std::clamp(m, 0.0, 100.0);
        curSNR = (float)s2n;
    }

    static double decayavg(double avg, double v, double weight) {
        if (weight <= 1.0) return v;
        return avg + (v - avg) / weight;
    }

    // NOTE: THOR is differential (IFK+) and tolerant of a few hundred Hz of
    // mistuning thanks to the wide tone-capture range (extones). fldigi itself
    // provides no AFC for THOR; the operator tunes to the signal centre. A
    // centroid tracker is unreliable here because the tone occupancy of an IFK+
    // stream is not uniform, so automatic adjustment is intentionally disabled.
    void updateAFC() { /* no automatic AF adjustment */ }

    // --- state ---
    int modeIdx = 4;
    double afFreq = 1500.0;
    double internalSR = 8000.0;
    int decim = 6;
    int symlen = 0, doublespaced = 1, paths = 5, extones = 9;
    int basetone = 0, lotone = 0, hitone = 0, numbins = 0, stride = 0;
    int twosym = 0;
    double tonespacing = 0, bandwidth = 0;

    std::unique_ptr<C_FIR_filter> decimator, hilbert;
    std::vector<std::unique_ptr<sfft>> binsfft;
    std::unique_ptr<Cmovavg> syncfilter;
    std::unique_ptr<Viterbi> dec;
    std::unique_ptr<Interleaver> rxinlv;

    std::vector<cmplx> pipe;
    unsigned int pipeptr = 0;

    double phase[9] = {0};
    int synccounter = 0, symcounter = 0;
    int currsymbol = 0, prev1symbol = 0, prev2symbol = 0;
    double currmag = 0, prev1mag = 0, prev2mag = 0;
    unsigned int datashreg = 1;
    unsigned char symbolpair[2] = {0, 0};
    double sig = 6, noise = 6, s2n = 0, metric = 0;
    bool staticburst = false;
    int fecConfidence = 0;
    bool reverse = false;
    bool afcEnabled = false;
    bool preambleEnabled = true;

    // preamble detector state
    int preChk = 0, twoCnt = 0;
    bool neg16 = false;

    int curTone = 0;
    float curSNR = 0.0f;
    std::vector<cmplx> scopeRing;
    int scopePos = 0;
    double scopeMax = 1e-6;
    double emaSym = 0.0;
    bool afcInited = false;
};

} // namespace thor
