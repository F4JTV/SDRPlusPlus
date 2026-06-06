#pragma once
// FLDIGI-faithful MFSK (de)interleaver (ported verbatim from fldigi src/mfsk/interleave.cxx;
// (C) Dave Freese W1HKJ / gmfsk Tomi Manninen OH2BNS, GPL).
#include <cstring>

namespace mfsk {

#define MFSK_INTERLEAVE_FWD 0
#define MFSK_INTERLEAVE_REV 1
#define MFSK_PUNCTURE       128

class Interleaver {
public:
    Interleaver() : size(0), depth(0), len(0), direction(MFSK_INTERLEAVE_REV), table(nullptr) {}
    ~Interleaver() { delete[] table; }
    void init(int _size, int _depth, int dir) {
        delete[] table;
        size = _size; depth = _depth; direction = dir;
        len = size * size * depth;
        table = new unsigned char[len];
        flush();
    }
    void flush() {
        if (!table) return;
        memset(table, direction == MFSK_INTERLEAVE_REV ? MFSK_PUNCTURE : 0, len);
    }
    void symbols(unsigned char* psyms) {
        if (!table) return;
        for (int k = 0; k < depth; k++) {
            for (int i = 0; i < size; i++)
                for (int j = 0; j < size - 1; j++)
                    *tab(k, i, j) = *tab(k, i, j + 1);
            for (int i = 0; i < size; i++)
                *tab(k, i, size - 1) = psyms[i];
            for (int i = 0; i < size; i++) {
                if (direction == MFSK_INTERLEAVE_FWD) psyms[i] = *tab(k, i, size - i - 1);
                else                                  psyms[i] = *tab(k, i, i);
            }
        }
    }
private:
    unsigned char* tab(int i, int j, int k) { return &table[(size*size*i) + (size*j) + k]; }
    int size, depth, len, direction;
    unsigned char* table;
};

} // namespace mfsk
