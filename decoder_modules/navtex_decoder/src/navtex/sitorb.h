#pragma once
#include <vector>
#include <functional>
#include <climits>
#include "ccir476.h"

// SITOR collective B-mode (FEC) decoder, as used by NAVTEX.
//
// In FEC mode every character is transmitted twice with time diversity: the
// "alpha" (DX) copy and, five characters earlier in the stream, an identical
// "rep" (RX) copy. The transmitter interleaves them:
//
//     rep alpha rep alpha rep alpha ...
//
// so DX and its matching RX copy are 7 characters apart on the air but, because
// of the interleave, the rep that protects a given alpha sits 5 character
// periods (35 bits) before it.
//
// This class consumes ONE soft bit per call (a signed confidence: > 0 means
// mark/1, < 0 means space/0, magnitude = certainty). It maintains a rolling
// bit buffer, locks onto the alpha/rep phase, and for each alpha character runs
// the FEC chain: take the alpha copy if it is a valid 4-of-7 group, otherwise
// fall back to the rep copy, otherwise try soft combinations of the two.
//
// Logic ported from FLDIGI (src/navtex/navtex.cxx).

namespace navtex {

    class SitorB {
    public:
        // Emits decoded control/character codes. code_ltrs/code_figs/etc. are
        // passed through so the caller can react; printable glyphs arrive as
        // their ASCII value.
        std::function<void(int /*decodedChar*/)> onChar;

        SitorB() { reset(); }

        void reset() {
            m_bit_values.assign(BUF_BITS, 0);
            m_bit_cursor = 0;
            m_state = SYNC;
            m_alpha_phase = false;
            m_shift = false;
            m_error_count = 0;
            m_last_char = 0;
        }

        // Feed one soft bit (signed confidence value).
        void pushBit(int confidence) {
            int n = (int)m_bit_values.size();

            // Shift the rolling buffer left and append the new sample.
            for (int i = 0; i < n - 1; i++) { m_bit_values[i] = m_bit_values[i + 1]; }
            m_bit_values[n - 1] = confidence;
            if (m_bit_cursor > 0) { m_bit_cursor--; }

            // While unsynced, search for the alpha/rep framing.
            if (m_state == SYNC) {
                int offset = find_alpha_characters();
                if (offset >= 0) {
                    m_state = READ_DATA;
                    m_bit_cursor = offset;
                    m_alpha_phase = true;
                } else {
                    m_state = SYNC;
                }
            }

            // Consume 7-bit characters as they become available, decoding only
            // the alpha (DX) positions and skipping the rep (RX) positions.
            if (m_state == READ_DATA) {
                if (m_bit_cursor < n - 7) {
                    if (m_alpha_phase) {
                        int ret = process_bytes(m_bit_cursor);
                        m_error_count -= ret;
                        if (m_error_count > 5) { m_state = SYNC; m_error_count = 0; }
                        if (m_error_count < 0) { m_error_count = 0; }
                    }
                    m_alpha_phase = !m_alpha_phase;
                    m_bit_cursor += 7;
                }
            }
        }

        bool synced() const { return m_state == READ_DATA; }

    private:
        enum State { SYNC, READ_DATA };

        // One second of bits at 100 baud: fits 14 interleaved characters.
        static const int BUF_BITS = 100;

        std::vector<int> m_bit_values;
        int   m_bit_cursor;
        State m_state;
        bool  m_alpha_phase;
        bool  m_shift;
        int   m_error_count;
        int   m_last_char;

        // The rep protecting an alpha sits 35 bits (5 characters) earlier.
        static int fec_offset(int offset) { return offset - 35; }

        // Flip the least-certain bit so a 5-mark or 4-space group becomes a
        // valid 4-of-7 group.
        static void flip_smallest_bit(int* pos) {
            int min_zero = INT_MIN, min_one = INT_MAX;
            int min_zero_pos = -1, min_one_pos = -1;
            int count_zero = 0, count_one = 1;
            for (int i = 0; i < 7; i++) {
                int val = pos[i];
                if (val < 0) {
                    count_zero++;
                    if (val > min_zero) { min_zero = val; min_zero_pos = i; }
                } else {
                    count_one++;
                    if (val < min_one) { min_one = val; min_one_pos = i; }
                }
            }
            if (count_zero == 4 && min_zero_pos >= 0)      { pos[min_zero_pos] = -pos[min_zero_pos]; }
            else if (count_one == 5 && min_one_pos >= 0)   { pos[min_one_pos]  = -pos[min_one_pos];  }
        }

        // Find the bit offset that yields the most valid characters with rep
        // copies in the expected positions. Returns -1 until confident.
        int find_alpha_characters() {
            int best_offset = 0;
            int best_score  = 0;
            int n = (int)m_bit_values.size();

            // The first alpha with a matching rep can be in any of 14 places.
            for (int offset = 35; offset < (35 + 14); offset++) {
                int score = 0;
                int reps  = 0;
                int limit = n - 7;
                for (int i = offset; i < limit; i += 7) {
                    if (CCIR476::valid_char_at(&m_bit_values[i])) {
                        int ri   = fec_offset(i);
                        int code = CCIR476::bytes_to_code(&m_bit_values[i]);
                        int rep  = (ri >= 0) ? CCIR476::bytes_to_code(&m_bit_values[ri]) : -1;
                        score++;
                        if (code == rep) {
                            // Wrong phase: alpha and rep land oddly.
                            if (code == code_alpha || code == code_rep) { score = 0; continue; }
                            reps++;
                        } else if (code == code_alpha) {
                            int ri2  = i - 7;
                            if (ri2 >= 0) {
                                int rep2 = CCIR476::bytes_to_code(&m_bit_values[ri2]);
                                if (rep2 == code_rep) { reps++; }
                            }
                        }
                    }
                }
                if (reps >= 3 && score + reps > best_score) {
                    best_score  = score + reps;
                    best_offset = offset;
                }
            }
            return (best_score > 8) ? best_offset : -1;
        }

        // Decode one alpha character using time-diversity FEC.
        //  1: alpha valid, 0: skipped rep, -1: recovered, -2: hard failure.
        int process_bytes(int cursor) {
            int code    = CCIR476::bytes_to_code(&m_bit_values[cursor]);
            int success = 0;

            if (CCIR476::check_bits(code)) { success = 1; emit(code); return success; }

            if (fec_offset(cursor) < 0) { return -1; }

            int reppos = fec_offset(cursor);
            int rep    = CCIR476::bytes_to_code(&m_bit_values[reppos]);
            if (CCIR476::check_bits(rep)) {
                if (rep == code_rep) { return 0; } // current code is probably alpha
                code = rep; emit(code); return 0;
            }

            // Neither copy valid: try the soft sum, then bit flips.
            int avg[7];
            for (int i = 0; i < 7; i++) { avg[i] = m_bit_values[cursor + i] + m_bit_values[reppos + i]; }

            int calc = CCIR476::bytes_to_code(avg);
            if (CCIR476::check_bits(calc)) { emit(calc); return -1; }

            flip_smallest_bit(&m_bit_values[cursor]);
            calc = CCIR476::bytes_to_code(&m_bit_values[cursor]);
            if (CCIR476::check_bits(calc)) { emit(calc); return -1; }

            flip_smallest_bit(&m_bit_values[reppos]);
            calc = CCIR476::bytes_to_code(&m_bit_values[reppos]);
            if (CCIR476::check_bits(calc)) { emit(calc); return -1; }

            flip_smallest_bit(avg);
            calc = CCIR476::bytes_to_code(avg);
            if (CCIR476::check_bits(calc)) { emit(calc); return -1; }

            return -2;
        }

        // Apply shift state and forward printable characters to the caller.
        void emit(int chr) {
            switch (chr) {
                case code_rep:
                    // Two reps in a row while in alpha phase means the rep/alpha
                    // phase slipped; nudge it back so FEC works again.
                    if (m_last_char == code_rep) { m_alpha_phase = false; }
                    break;
                case code_alpha:
                case code_beta:
                case code_char32:
                    break;
                case code_ltrs: m_shift = false; break;
                case code_figs: m_shift = true;  break;
                default: {
                    int c = CCIR476::code_to_char(chr, m_shift);
                    if (c >= 0) {
                        if (c == char_bell) { c = '\''; }
                        if (c != '\r' && onChar) { onChar(c); }
                    }
                    break;
                }
            }
            m_last_char = chr;
        }
    };

}
