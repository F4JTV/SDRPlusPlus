#pragma once
// FLDIGI-faithful Viterbi decoder (ported verbatim from fldigi src/filters/viterbi.cxx,
// src/include/viterbi.h; (C) Dave Freese W1HKJ / gmfsk Tomi Manninen OH2BNS, GPL).
#include <cstring>
#include <climits>

namespace mfsk {

static inline int vit_parity(unsigned long w) { w^=w>>16; w^=w>>8; w^=w>>4; w^=w>>2; w^=w>>1; return (int)(w&1); }

#define MFSK_PATHMEM 256

class Viterbi {
public:
    Viterbi(int k, int poly1, int poly2) {
        outsize = 1 << k; nstates = 1 << (k - 1);
        _k = k; _poly1 = poly1; _poly2 = poly2;
        output = new int[outsize];
        for (int i = 0; i < MFSK_PATHMEM; i++) { metrics[i] = new int[nstates]; history[i] = new int[nstates]; }
        init();
    }
    ~Viterbi() {
        delete[] output;
        for (int i = 0; i < MFSK_PATHMEM; i++) { delete[] metrics[i]; delete[] history[i]; }
    }
    void init() {
        _traceback = _k * 12; _chunksize = 8;
        for (int i = 0; i < outsize; i++) output[i] = vit_parity(_poly1 & i) | (vit_parity(_poly2 & i) << 1);
        for (int i = 0; i < 256; i++) { mettab[0][i] = 128 - i; mettab[1][i] = i - 128; }
        memset(sequence, 0, sizeof(sequence)); reset();
    }
    void reset() {
        for (int i = 0; i < MFSK_PATHMEM; i++) { memset(metrics[i], 0, nstates*sizeof(int)); memset(history[i], 0, nstates*sizeof(int)); }
        ptr = 0;
    }
    int settraceback(int t) { _traceback = t; return 0; }
    int setchunksize(int c) { _chunksize = c; return 0; }
    int decode(unsigned char* sym, int* metric) {
        unsigned int currptr = ptr, prevptr = (currptr - 1) % MFSK_PATHMEM;
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
        ptr = (ptr + 1) % MFSK_PATHMEM;
        if ((ptr % _chunksize) == 0) return traceback(metric);
        if (metrics[currptr][0] > INT_MAX/2) for (int i=0;i<MFSK_PATHMEM;i++) for (int j=0;j<nstates;j++) metrics[i][j] -= INT_MAX/2;
        if (metrics[currptr][0] < INT_MIN/2) for (int i=0;i<MFSK_PATHMEM;i++) for (int j=0;j<nstates;j++) metrics[i][j] += INT_MIN/2;
        return -1;
    }
private:
    int traceback(int* metric) {
        int bestmetric = INT_MIN, beststate = 0; unsigned int p = (ptr - 1) % MFSK_PATHMEM, c = 0;
        for (int i = 0; i < nstates; i++) if (metrics[p][i] > bestmetric) { bestmetric = metrics[p][i]; beststate = i; }
        sequence[p] = beststate;
        for (int i = 0; i < _traceback; i++) { unsigned int prev = (p - 1) % MFSK_PATHMEM; sequence[prev] = history[p][sequence[p]]; p = prev; }
        if (metric) *metric = metrics[p][sequence[p]];
        for (int i = 0; i < _chunksize; i++) { c = (c << 1) | (sequence[p] & 1); p = (p + 1) % MFSK_PATHMEM; }
        if (metric) *metric = metrics[p][sequence[p]] - *metric;
        return c;
    }
    int _traceback, _chunksize, nstates, *output, outsize;
    int *metrics[MFSK_PATHMEM], *history[MFSK_PATHMEM], sequence[MFSK_PATHMEM], mettab[2][256];
    unsigned int ptr; int _k, _poly1, _poly2;
};

} // namespace mfsk
