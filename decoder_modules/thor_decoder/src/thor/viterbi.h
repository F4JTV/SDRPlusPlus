#pragma once
// FLDIGI-faithful Viterbi decoder + convolutional encoder.
// Ported verbatim from fldigi src/filters/viterbi.cxx / src/include/viterbi.h
// (C) Dave Freese W1HKJ / gmfsk Tomi Manninen OH2BNS, GPL.
//
// Supports both the K=7 (THOR low-speed) and K=15 (THOR high-speed) codes.
// The decoder is generic in the constraint length; for K=15 the state count
// is 2^14 = 16384, which is comfortably handled here.
#include <cstring>
#include <climits>
#include <vector>

namespace thor {

static inline int vit_parity(unsigned long w) {
    w ^= w >> 16; w ^= w >> 8; w ^= w >> 4; w ^= w >> 2; w ^= w >> 1;
    return (int)(w & 1);
}

#define THOR_PATHMEM 256

class Viterbi {
public:
    Viterbi(int k, int poly1, int poly2) {
        _k = k; _poly1 = poly1; _poly2 = poly2;
        outsize = 1 << k;
        nstates = 1 << (k - 1);
        output = new int[outsize];
        for (int i = 0; i < THOR_PATHMEM; i++) {
            metrics[i] = new int[nstates];
            history[i] = new int[nstates];
        }
        sequence.assign(THOR_PATHMEM, 0);
        init();
    }
    ~Viterbi() {
        delete[] output;
        for (int i = 0; i < THOR_PATHMEM; i++) { delete[] metrics[i]; delete[] history[i]; }
    }
    Viterbi(const Viterbi&) = delete;
    Viterbi& operator=(const Viterbi&) = delete;

    void init() {
        _traceback = _k * 12;
        _chunksize = 8;
        for (int i = 0; i < outsize; i++)
            output[i] = vit_parity(_poly1 & i) | (vit_parity(_poly2 & i) << 1);
        for (int i = 0; i < 256; i++) { mettab[0][i] = 128 - i; mettab[1][i] = i - 128; }
        reset();
    }
    void reset() {
        for (int i = 0; i < THOR_PATHMEM; i++) {
            memset(metrics[i], 0, nstates * sizeof(int));
            memset(history[i], 0, nstates * sizeof(int));
        }
        for (size_t i = 0; i < sequence.size(); i++) sequence[i] = 0;
        ptr = 0;
    }
    int settraceback(int t) { _traceback = t; return 0; }
    int setchunksize(int c) { _chunksize = c; return 0; }

    int decode(unsigned char* sym, int* metric) {
        unsigned int currptr = ptr, prevptr = (currptr - 1) % THOR_PATHMEM;
        int met[4];
        met[0] = mettab[0][sym[1]] + mettab[0][sym[0]];
        met[1] = mettab[0][sym[1]] + mettab[1][sym[0]];
        met[2] = mettab[1][sym[1]] + mettab[0][sym[0]];
        met[3] = mettab[1][sym[1]] + mettab[1][sym[0]];
        for (int n = 0; n < nstates; n++) {
            int s0 = n, s1 = n + nstates, p0 = s0 >> 1, p1 = s1 >> 1;
            int m0 = metrics[prevptr][p0] + met[output[s0]];
            int m1 = metrics[prevptr][p1] + met[output[s1]];
            if (m0 > m1) { metrics[currptr][n] = m0; history[currptr][n] = p0; }
            else         { metrics[currptr][n] = m1; history[currptr][n] = p1; }
        }
        ptr = (ptr + 1) % THOR_PATHMEM;
        if ((ptr % _chunksize) == 0) return traceback(metric);
        if (metrics[currptr][0] > INT_MAX / 2)
            for (int i = 0; i < THOR_PATHMEM; i++) for (int j = 0; j < nstates; j++) metrics[i][j] -= INT_MAX / 2;
        if (metrics[currptr][0] < INT_MIN / 2)
            for (int i = 0; i < THOR_PATHMEM; i++) for (int j = 0; j < nstates; j++) metrics[i][j] += INT_MIN / 2;
        return -1;
    }

private:
    int traceback(int* metric) {
        int bestmetric = INT_MIN, beststate = 0;
        unsigned int p = (ptr - 1) % THOR_PATHMEM, c = 0;
        for (int i = 0; i < nstates; i++)
            if (metrics[p][i] > bestmetric) { bestmetric = metrics[p][i]; beststate = i; }
        sequence[p] = beststate;
        for (int i = 0; i < _traceback; i++) {
            unsigned int prev = (p - 1) % THOR_PATHMEM;
            sequence[prev] = history[p][sequence[p]];
            p = prev;
        }
        if (metric) *metric = metrics[p][sequence[p]];
        for (int i = 0; i < _chunksize; i++) {
            c = (c << 1) | (sequence[p] & 1);
            p = (p + 1) % THOR_PATHMEM;
        }
        if (metric) *metric = metrics[p][sequence[p]] - *metric;
        return c;
    }

    int _traceback, _chunksize, nstates, *output, outsize;
    int *metrics[THOR_PATHMEM], *history[THOR_PATHMEM];
    std::vector<int> sequence;
    int mettab[2][256];
    unsigned int ptr = 0;
    int _k, _poly1, _poly2;
};

// Convolutional encoder (rate 1/2). Used by the test-signal generator.
class Encoder {
public:
    Encoder(int k, int poly1, int poly2) {
        _k = k; _poly1 = poly1; _poly2 = poly2;
        int size = 1 << k;
        output = new int[size];
        for (int i = 0; i < size; i++)
            output[i] = vit_parity(_poly1 & i) | (vit_parity(_poly2 & i) << 1);
        shreg = 0;
        shregmask = size - 1;
    }
    ~Encoder() { delete[] output; }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Returns the 2-bit encoded output for one input bit.
    int encode(int bit) {
        shreg = (shreg << 1) | (bit ? 1 : 0);
        return output[shreg & shregmask];
    }
    void reset() { shreg = 0; }

private:
    int* output;
    unsigned int shreg;
    unsigned int shregmask;
    int _k, _poly1, _poly2;
};

} // namespace thor
