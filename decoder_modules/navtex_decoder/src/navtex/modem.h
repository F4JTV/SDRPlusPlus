#pragma once
#include <math.h>
#include <string.h>
#include <vector>
#include <mutex>
#include <functional>
#include <algorithm>
#include "sitorb.h"

// NAVTEX (SITOR-B / CCIR 476 FEC) software modem.
//
// The FSK front-end is shared with the RTTY module: two sliding complex
// correlators tuned to the mark and space tones (boxcar integration over one
// symbol) operate on the complex VFO baseband. The same windowed DFT bank
// drives the GUI band view and the optional AFC.
//
// Unlike RTTY (asynchronous start/stop framing), NAVTEX is SYNCHRONOUS: a
// continuous 100-baud stream of 7-bit CCIR 476 codes with no start bits. So
// the UART sampler is replaced by a transition-locked DPLL that recovers the
// bit clock and emits one soft confidence value per symbol, mid-bit. Those
// soft bits feed the SitorB FEC decoder.
//
//   - USB : audio tone at frequency f  -> IQ component at +f
//   - LSB : audio tone at frequency f  -> IQ component at -f  (we conjugate)
//   - NFM : FM-discriminate to a real audio sample, fed as {audio, 0}

namespace navtex {

    static constexpr int    SPEC_BINS   = 128;
    static constexpr double SPEC_FLO    = 0.0;
    static constexpr double SPEC_FHI    = 3000.0;
    static constexpr int    SPEC_WINDOW = 1536;
    static constexpr int    SPEC_HOP    = 1024;

    class Modem {
    public:
        std::function<void(char)> onChar;

        Modem() {
            sitor.onChar = [this](int c) {
                if (c >= 0 && c <= 0xFF && onChar) { onChar((char)c); }
            };
        }

        void configure(double sampleRate, double baud, double shiftHz,
                       bool reverse) {
            std::lock_guard<std::mutex> lck(mtx);
            fs           = sampleRate;
            this->baud   = baud;
            this->shift  = shiftHz;
            this->reverse= reverse;
            recompute();
            resetState();
        }

        void setSideband(int sb) { std::lock_guard<std::mutex> lck(mtx); sideband = sb; resetState(); }
        void setAFFreq(double af) { std::lock_guard<std::mutex> lck(mtx); afFreq = af; recompute(); }
        double getAFFreq() const { return afFreq; }
        void setAFCEnabled(bool en) { std::lock_guard<std::mutex> lck(mtx); afc = en; }
        bool getAFCEnabled() const { return afc; }
        double getTrackedAFFreq() const { return afFreq; }
        double getShift() const { return shift; }
        double getBaud()  const { return baud; }
        bool   getSynced() const { return sitor.synced(); }

        // GUI band view ------------------------------------------------------
        double getBandFlo() const { return SPEC_FLO; }
        double getBandFhi() const { return SPEC_FHI; }
        double getSignalBandwidth() const { return shift + baud; }
        int getBandSpectrum(float* out, int n) const {
            std::lock_guard<std::mutex> lck(specMtx);
            int c = std::min(n, SPEC_BINS);
            for (int i = 0; i < c; i++) { out[i] = spec[i]; }
            return c;
        }

        // Feed complex VFO samples (interleaved re,im). -----------------------
        void process(const float* iq, int count) {
            std::lock_guard<std::mutex> lck(mtx);
            for (int i = 0; i < count; i++) {
                float re = iq[2*i];
                float im = iq[2*i + 1];

                float pre, pim;
                if (sideband == 1) {            // LSB: mirror spectrum
                    pre = re; pim = -im;
                }
                else if (sideband == 2) {       // NFM: FM discriminator
                    float d = atan2f(im * prevRe - re * prevIm, re * prevRe + im * prevIm);
                    prevRe = re; prevIm = im;
                    pre = d; pim = 0.0f;
                }
                else {                          // USB
                    pre = re; pim = im;
                }
                processSample(pre, pim);
            }
        }

        void reset() { std::lock_guard<std::mutex> lck(mtx); resetState(); }

    private:
        // --- configuration ---
        double fs = 24000.0, baud = 100.0, shift = 170.0, afFreq = 1000.0;
        bool   reverse = false, afc = false;
        int    sideband = 0;

        // --- correlator NCOs (mark / space) ---
        double markFreq = 0, spaceFreq = 0;
        float mCos = 1, mSin = 0, mStepC = 1, mStepS = 0;
        float sCos = 1, sSin = 0, sStepC = 1, sStepS = 0;
        int   intLen = 240;

        std::vector<float> mBufR, mBufI, sBufR, sBufI;
        int   bufPos = 0;
        float mSumR = 0, mSumI = 0, sSumR = 0, sSumI = 0;

        // --- synchronous bit-clock DPLL ---
        double clk = 0.0;       // symbol phase; advances 1.0 per bit period
        double inc = 0.0;       // baud / fs
        long   bitIdx = 0;      // last emitted bit index (monotonic)
        int    prevSgn = 1;     // last hard mark/space sign, for the TED

        // --- FEC / character decoder ---
        SitorB sitor;

        // --- NFM discriminator memory ---
        float prevRe = 1.0f, prevIm = 0.0f;

        // --- spectrum / AFC window ---
        std::vector<float> winR, winI;
        int   winPos = 0, winFill = 0, hopCnt = 0;
        mutable std::mutex specMtx;
        float spec[SPEC_BINS] = {0};

        std::mutex mtx;

        void recompute() {
            // Mark is the higher tone by convention; reverse swaps them.
            double half = shift * 0.5;
            double m = afFreq + half;
            double s = afFreq - half;
            if (reverse) { std::swap(m, s); }
            markFreq  = m;
            spaceFreq = s;

            double wm = -2.0 * M_PI * markFreq  / fs;
            double ws = -2.0 * M_PI * spaceFreq / fs;
            mStepC = (float)cos(wm); mStepS = (float)sin(wm);
            sStepC = (float)cos(ws); sStepS = (float)sin(ws);
            mCos = 1; mSin = 0; sCos = 1; sSin = 0;

            intLen = std::max(8, (int)round(fs / baud));
            mBufR.assign(intLen, 0); mBufI.assign(intLen, 0);
            sBufR.assign(intLen, 0); sBufI.assign(intLen, 0);
            bufPos = 0; mSumR = mSumI = sSumR = sSumI = 0;

            inc = baud / fs;

            if ((int)winR.size() != SPEC_WINDOW) { winR.assign(SPEC_WINDOW, 0); winI.assign(SPEC_WINDOW, 0); }
        }

        void resetState() {
            clk = 0.0; bitIdx = 0; prevSgn = 1;
            sitor.reset();
        }

        inline void rotate(float& c, float& s, float stepC, float stepS) {
            float nc = c * stepC - s * stepS;
            float ns = c * stepS + s * stepC;
            c = nc; s = ns;
        }

        void processSample(float re, float im) {
            // ---- mark correlator ----
            float mr = re * mCos - im * mSin;
            float mi = re * mSin + im * mCos;
            mSumR += mr - mBufR[bufPos]; mSumI += mi - mBufI[bufPos];
            mBufR[bufPos] = mr; mBufI[bufPos] = mi;

            // ---- space correlator ----
            float sr = re * sCos - im * sSin;
            float si = re * sSin + im * sCos;
            sSumR += sr - sBufR[bufPos]; sSumI += si - sBufI[bufPos];
            sBufR[bufPos] = sr; sBufI[bufPos] = si;

            bufPos++; if (bufPos >= intLen) { bufPos = 0; }

            rotate(mCos, mSin, mStepC, mStepS);
            rotate(sCos, sSin, sStepC, sStepS);
            if (++renorm >= 4096) {
                renorm = 0;
                float gm = 1.0f / sqrtf(mCos*mCos + mSin*mSin); mCos *= gm; mSin *= gm;
                float gs = 1.0f / sqrtf(sCos*sCos + sSin*sSin); sCos *= gs; sSin *= gs;
            }

            float markMag  = mSumR*mSumR + mSumI*mSumI;
            float spaceMag = sSumR*sSumR + sSumI*sSumI;
            float total    = markMag + spaceMag + 1e-9f;

            // Normalised soft decision in [-1, 1]; >0 -> mark/1, <0 -> space/0.
            float norm = (markMag - spaceMag) / total;
            int   sgn  = (norm >= 0.0f) ? 1 : -1;

            // ---- transition-locked DPLL ----
            // Advance the symbol phase; pull transitions toward the half-bit
            // instant so the integer crossings (where we sample) land mid-bit.
            clk += inc;
            if (sgn != prevSgn) {
                if (total > 1e-6f) {                 // only track on real signal
                    double frac = clk - floor(clk);  // [0, 1)
                    double err  = frac - 0.5;
                    clk -= TED_GAIN * err;
                }
                prevSgn = sgn;
            }

            // Sample once per symbol, at the integer crossing (bit centre).
            long fl = (long)floor(clk);
            if (fl > bitIdx) {
                bitIdx = fl;
                int conf = (int)lrintf(norm * 1000.0f);
                sitor.pushBit(conf);
            }

            // ---- spectrum / AFC window ----
            winR[winPos] = re; winI[winPos] = im;
            winPos++; if (winPos >= SPEC_WINDOW) { winPos = 0; }
            if (winFill < SPEC_WINDOW) { winFill++; }
            if (++hopCnt >= SPEC_HOP) { hopCnt = 0; updateSpectrum(); }
        }

        void updateSpectrum() {
            if (winFill < SPEC_WINDOW) { return; }
            float tmp[SPEC_BINS];
            float mx = 1e-9f;
            for (int b = 0; b < SPEC_BINS; b++) {
                double fk = SPEC_FLO + (SPEC_FHI - SPEC_FLO) * (double)b / (double)(SPEC_BINS - 1);
                double w  = -2.0 * M_PI * fk / fs;
                double cw = cos(w), sw = sin(w);
                double ph_c = 1.0, ph_s = 0.0;
                double accR = 0.0, accI = 0.0;
                int idx = winPos;
                for (int n = 0; n < SPEC_WINDOW; n++) {
                    float xr = winR[idx], xi = winI[idx];
                    accR += xr * ph_c - xi * ph_s;
                    accI += xr * ph_s + xi * ph_c;
                    double nc = ph_c * cw - ph_s * sw;
                    double ns = ph_c * sw + ph_s * cw;
                    ph_c = nc; ph_s = ns;
                    idx++; if (idx >= SPEC_WINDOW) { idx = 0; }
                }
                float m = (float)sqrt(accR*accR + accI*accI);
                tmp[b] = m;
                if (m > mx) { mx = m; }
            }
            {
                std::lock_guard<std::mutex> lck(specMtx);
                for (int b = 0; b < SPEC_BINS; b++) {
                    float v = tmp[b] / mx;
                    spec[b] = 0.6f * spec[b] + 0.4f * v;
                }
            }
            if (afc) { runAFC(tmp, mx); }
        }

        // Lock AF to the midpoint of the dominant mark/space pair (~shift apart).
        void runAFC(const float* mag, float mx) {
            auto binToFreq = [&](int b) {
                return SPEC_FLO + (SPEC_FHI - SPEC_FLO) * (double)b / (double)(SPEC_BINS - 1);
            };
            double binHz = (SPEC_FHI - SPEC_FLO) / (double)(SPEC_BINS - 1);
            int sepBins  = std::max(1, (int)round(shift / binHz));
            int tol      = std::max(1, sepBins / 3);

            double best = -1; int bi = -1, bj = -1;
            for (int i = 0; i < SPEC_BINS; i++) {
                if (mag[i] < 0.40f * mx) { continue; }
                for (int j = i + sepBins - tol; j <= i + sepBins + tol; j++) {
                    if (j < 0 || j >= SPEC_BINS) { continue; }
                    if (mag[j] < 0.40f * mx) { continue; }
                    float a = mag[i], b = mag[j];
                    float ratio = (a > b) ? (a / (b + 1e-9f)) : (b / (a + 1e-9f));
                    if (ratio > 3.0f) { continue; }
                    double score = (double)a + (double)b;
                    if (score > best) { best = score; bi = i; bj = j; }
                }
            }
            if (bi >= 0 && bj >= 0) {
                double center = 0.5 * (binToFreq(bi) + binToFreq(bj));
                if (center < 400)  center = 400;
                if (center > 2600) center = 2600;
                if (fabs(center - afFreq) > 1.0) {
                    afFreq = 0.8 * afFreq + 0.2 * center;
                    recompute();
                }
            }
        }

        static constexpr double TED_GAIN = 0.10;
        int renorm = 0;
    };

}
