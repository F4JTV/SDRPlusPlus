#pragma once
// FLDIGI-faithful (de)interleaver, ported from fldigi src/filters/interleave.cxx
// (C) Dave Freese W1HKJ / gmfsk Tomi Manninen OH2BNS, GPL.
//
// THOR uses interleave(size=4, depth, dir): a cascade of `depth` 4x4 diagonal
// interleaver stages. FWD is used on transmit, REV on receive.
#include <cstring>

namespace thor {

#define THOR_INTERLEAVE_FWD 0
#define THOR_INTERLEAVE_REV 1
#define THOR_PUNCTURE       128

class Interleaver {
public:
    Interleaver() : size(0), depth(0), len(0), direction(THOR_INTERLEAVE_REV), table(nullptr) {}
    ~Interleaver() { delete[] table; }
    Interleaver(const Interleaver&) = delete;
    Interleaver& operator=(const Interleaver&) = delete;

    void init(int _size, int _depth, int dir) {
        delete[] table;
        size = _size; depth = _depth; direction = dir;
        len = size * size * depth;
        table = new unsigned char[len];
        flush();
    }
    void flush() {
        if (!table) return;
        memset(table, direction == THOR_INTERLEAVE_REV ? THOR_PUNCTURE : 0, len);
    }

    // Permute `size` soft symbols in place through the interleaver cascade.
    void symbols(unsigned char* psyms) {
        if (!table) return;
        for (int k = 0; k < depth; k++) {
            for (int i = 0; i < size; i++)
                for (int j = 0; j < size - 1; j++)
                    *tab(k, i, j) = *tab(k, i, j + 1);
            for (int i = 0; i < size; i++)
                *tab(k, i, size - 1) = psyms[i];
            for (int i = 0; i < size; i++) {
                if (direction == THOR_INTERLEAVE_FWD) psyms[i] = *tab(k, i, size - i - 1);
                else                                  psyms[i] = *tab(k, i, i);
            }
        }
    }

    // Interleave the low `size` bits of *pbits (MSB first) in place.
    // Used by the transmit path (generator).
    void bits(unsigned int* pbits) {
        if (!table) return;
        unsigned char syms[64];
        for (int i = 0; i < size; i++)
            syms[i] = (*pbits & (1u << (size - i - 1))) ? 1 : 0;
        symbols(syms);
        *pbits = 0;
        for (int i = 0; i < size; i++)
            if (syms[i]) *pbits |= (1u << (size - i - 1));
    }

private:
    unsigned char* tab(int i, int j, int k) { return &table[(size * size * i) + (size * j) + k]; }
    int size, depth, len, direction;
    unsigned char* table;
};

} // namespace thor
