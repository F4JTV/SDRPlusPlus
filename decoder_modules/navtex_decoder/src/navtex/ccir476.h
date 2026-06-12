#pragma once
#include <stdint.h>
#include <string.h>

// CCIR 476 character set used by SITOR / AMTOR / NAVTEX.
//
// Each character is a 7-bit code containing EXACTLY four mark bits (1) and
// three space bits (0). That constant 4-of-7 ratio is the error-detection
// mechanism: any received 7-bit group whose set-bit count is not 4 is known to
// be corrupt. Decoding uses two shift sets (letters / figures), exactly like
// ITA2/Baudot.
//
// The lookup tables and helpers below are ported from FLDIGI's NAVTEX decoder
// (src/navtex/navtex.cxx) so behaviour matches a known-good reference.
//
// Special control codes:
//   code_ltrs  (0x5A) -> letters shift
//   code_figs  (0x36) -> figures shift
//   code_alpha (0x0F) -> phasing signal / idle ("alpha")
//   code_rep   (0x66) -> repetition (RQ) phasing signal ("rep")
//   code_beta  (0x33) -> phasing signal 2 ("beta")
//   code_char32(0x6A) -> unperforated tape / blank
//   char_bell  (0x07) -> bell (printed as a quote on some receivers)

namespace navtex {

    static const unsigned char code_to_ltrs[128] = {
        //0   1    2    3    4    5    6    7    8    9    a    b    c    d    e    f
        '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', // 0
        '_', '_', '_', '_', '_', '_', '_', 'J', '_', '_', '_', 'F', '_', 'C', 'K', '_', // 1
        '_', '_', '_', '_', '_', '_', '_', 'W', '_', '_', '_', 'Y', '_', 'P', 'Q', '_', // 2
        '_', '_', '_', '_', '_', 'G', '_', '_', '_', 'M', 'X', '_', 'V', '_', '_', '_', // 3
        '_', '_', '_', '_', '_', '_', '_', 'A', '_', '_', '_', 'S', '_', 'I', 'U', '_', // 4
        '_', '_', '_', 'D', '_', 'R', 'E', '_', '_', 'N', '_', '_', ' ', '_', '_', '_', // 5
        '_', '_', '_', 'Z', '_', 'L', '_', '_', '_', 'H', '_', '_', '\n', '_', '_', '_', // 6
        '_', 'O', 'B', '_', 'T', '_', '_', '_', '\r', '_', '_', '_', '_', '_', '_', '_'  // 7
    };

    static const unsigned char code_to_figs[128] = {
        //0   1    2    3    4    5    6    7    8    9    a    b    c    d    e    f
        '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', // 0
        '_', '_', '_', '_', '_', '_', '_', '\'', '_', '_', '_', '!', '_', ':', '(', '_', // 1
        '_', '_', '_', '_', '_', '_', '_', '2', '_', '_', '_', '6', '_', '0', '1', '_', // 2
        '_', '_', '_', '_', '_', '&', '_', '_', '_', '.', '/', '_', ';', '_', '_', '_', // 3
        '_', '_', '_', '_', '_', '_', '_', '-', '_', '_', '_', '\07', '_', '8', '7', '_', // 4
        '_', '_', '_', '$', '_', '4', '3', '_', '_', ',', '_', '_', ' ', '_', '_', '_', // 5
        '_', '_', '_', '"', '_', ')', '_', '_', '_', '#', '_', '_', '\n', '_', '_', '_', // 6
        '_', '9', '?', '_', '5', '_', '_', '_', '\r', '_', '_', '_', '_', '_', '_', '_'  // 7
    };

    static const int code_ltrs   = 0x5A;
    static const int code_figs   = 0x36;
    static const int code_alpha  = 0x0F;
    static const int code_beta   = 0x33;
    static const int code_char32 = 0x6A;
    static const int code_rep    = 0x66;
    static const int char_bell   = 0x07;

    class CCIR476 {
    public:
        // Count set bits; a CCIR 476 code is valid iff it has exactly four.
        static bool check_bits(int v) {
            int bc = 0;
            while (v != 0) { bc++; v &= v - 1; }
            return bc == 4;
        }

        // Build a 7-bit code from seven soft confidence values:
        // a value > 0 contributes a mark (1) in its bit position.
        static int bytes_to_code(const int* pos) {
            int code = 0;
            for (int i = 0; i < 7; i++) { code |= ((pos[i] > 0) << i); }
            return code;
        }

        // True when the seven confidence values map to a 4-of-7 (valid) group.
        static bool valid_char_at(const int* pos) {
            int count = 0;
            for (int i = 0; i < 7; i++) { if (pos[i] > 0) { count++; } }
            return count == 4;
        }

        // Map a code to a character given the current (letters/figures) shift.
        // Returns the printable char, or -code when the code has no glyph.
        static int code_to_char(int code, bool shift) {
            const unsigned char* target = shift ? code_to_figs : code_to_ltrs;
            if (target[code] != '_') { return target[code]; }
            return -code;
        }
    };

}
