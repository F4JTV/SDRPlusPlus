#pragma once
// THOR test-signal generator (transmit path), faithful port of fldigi
// src/thor/thor.cxx TX: varicode -> rate-1/2 FEC -> 4x4 interleaver ->
// IFK+ tone -> cosine. Produces real audio at AUDIO_SR (48 kHz) so the full
// decoder chain (including the 48k->internal decimator) is exercised.
#include <vector>
#include <string>
#include <cmath>
#include "modem.h"
#include "viterbi.h"
#include "interleave.h"
#include "varicode.h"

namespace thor {

class ThorGenerator {
public:
    // modeIdx: index into THOR_MODES; afFreq: signal centre frequency (Hz).
    ThorGenerator(int modeIdx, double afFreq)
        : idx(std::clamp(modeIdx, 0, THOR_MODE_COUNT - 1)), af(afFreq) {
        const ThorMode& M = THOR_MODES[idx];
        if      (M.nativeSR == 8000)  { internalSR = 8000.0;  decim = 6; }
        else if (M.nativeSR == 11025) { internalSR = 12000.0; decim = 4; }
        else                          { internalSR = 16000.0; decim = 3; }
        doublespaced = M.doublespaced;
        symlen = (int)std::lround((double)M.symlen * internalSR / M.nativeSR);
        if (symlen < 8) symlen = 8;
        tonespacing = internalSR * doublespaced / symlen;
        bandwidth = THORNUMTONES * tonespacing;
        sampPerSym = decim * symlen;   // 48k samples per symbol
        enc = std::make_unique<Encoder>(M.k == 15 ? THOR_K15 : THOR_K,
                                        M.k == 15 ? THOR_K15_P1 : THOR_POLY1,
                                        M.k == 15 ? THOR_K15_P2 : THOR_POLY2);
        txinlv = std::make_unique<Interleaver>();
        txinlv->init(4, M.depth, THOR_INTERLEAVE_FWD);
    }

    // Generate audio for a message. A preamble + idle flush bracket the text,
    // exactly like fldigi, so the receiver interleaver/viterbi fill correctly.
    std::vector<float> generate(const std::string& text) {
        out.clear();
        txprevtone = 0; bitstate = 0; bitshreg = 0; txphase = 0.0;

        // preamble
        clearbits();
        for (int j = 0; j < 16; j++) sendsymbol(0);
        sendchar(0, 0);                 // idle <NUL>
        // start
        sendchar('\r', 0);
        sendchar(2, 0);                 // STX
        sendchar('\r', 0);
        // data
        for (unsigned char c : text) sendchar(c, 0);
        // end
        sendchar('\r', 0);
        sendchar(4, 0);                 // EOT
        sendchar('\r', 0);
        // flush: enough idle symbols to push the tail through the deep
        // interleaver + long-constraint viterbi of the high-speed modes.
        int flush = std::max(40, THOR_MODES[idx].depth * 2 + THOR_MODES[idx].k * 4);
        for (int i = 0; i < flush; i++) sendchar(0, 0);

        return out;
    }

    double sampleRate() const { return AUDIO_SR; }

private:
    void clearbits() {
        int data = enc->encode(0);
        for (int k = 0; k < 1400; k++) {
            for (int i = 0; i < 2; i++) {
                bitshreg = (bitshreg << 1) | ((data >> i) & 1);
                if (++bitstate == 4) { txinlv->bits(&bitshreg); bitstate = 0; bitshreg = 0; }
            }
        }
    }
    void sendchar(unsigned char c, int sec) {
        const char* code = thor_varienc(c, sec);
        while (*code) {
            int data = enc->encode(*code++ - '0');
            for (int i = 0; i < 2; i++) {
                bitshreg = (bitshreg << 1) | ((data >> i) & 1);
                if (++bitstate == 4) {
                    txinlv->bits(&bitshreg);
                    sendsymbol(bitshreg);
                    bitstate = 0; bitshreg = 0;
                }
            }
        }
    }
    void sendsymbol(int sym) {
        int tone = (txprevtone + 2 + sym) % THORNUMTONES;
        txprevtone = tone;
        sendtone(tone);
    }
    void sendtone(int tone) {
        double f = (tone + 0.5) * tonespacing + af - bandwidth / 2.0;
        double phaseincr = THOR_TWOPI * f / AUDIO_SR;
        for (int i = 0; i < sampPerSym; i++) {
            out.push_back((float)cos(txphase));
            txphase -= phaseincr;
            if (txphase < 0) txphase += THOR_TWOPI;
        }
    }

    int idx;
    double af;
    double internalSR; int decim;
    int doublespaced, symlen, sampPerSym;
    double tonespacing, bandwidth;
    std::unique_ptr<Encoder> enc;
    std::unique_ptr<Interleaver> txinlv;
    int txprevtone = 0, bitstate = 0;
    unsigned int bitshreg = 0;
    double txphase = 0.0;
    std::vector<float> out;
};

} // namespace thor
