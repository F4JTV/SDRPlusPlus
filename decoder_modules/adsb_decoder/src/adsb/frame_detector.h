#pragma once
#include <stdint.h>
#include <vector>
#include <cmath>
#include <cstring>
#include <functional>
#include "adsb.h"

// ----------------------------------------------------------------------------
// Mode S / ADS-B frame detector operating on a magnitude stream sampled at
// 2 MHz (2 samples per 1 us bit), following the classic dump1090 approach:
//
//   * 8 us preamble correlation (pulses at samples 0, 2, 7, 9).
//   * PPM bit slicing (first half-bit high = 1, second half-bit high = 0).
//   * Length from Downlink Format (DF >= 16 -> 112 bits, else 56 bits).
//   * CRC-24 syndrome check (must be 0).
//
// It buffers samples internally so frames spanning chunk boundaries are handled.
// ----------------------------------------------------------------------------

namespace adsb {

    static const int SAMPLES_PER_BIT = 2;          // at 2 MHz
    static const int PREAMBLE_SAMPLES = 16;        // 8 us
    static const int LONG_BITS  = 112;
    static const int SHORT_BITS = 56;
    static const int LONG_SAMPLES  = LONG_BITS  * SAMPLES_PER_BIT; // 224
    static const int FRAME_MAX_SAMPLES = PREAMBLE_SAMPLES + LONG_SAMPLES; // 240

    class FrameDetector {
    public:
        // Callback invoked for every frame that passes the CRC check.
        std::function<void(const RawFrame&)> onFrame;

        FrameDetector() {
            buffer.reserve(1 << 16);
        }

        void reset() { buffer.clear(); }

        // Feed a chunk of magnitude samples (linear amplitude, any positive scale).
        void process(const float* mag, int count) {
            // Append to the working buffer.
            size_t oldSize = buffer.size();
            buffer.resize(oldSize + count);
            for (int i = 0; i < count; i++) buffer[oldSize + i] = mag[i];

            // Scan for frames, leaving a tail long enough to hold a full long frame.
            if (buffer.size() < (size_t)FRAME_MAX_SAMPLES) return;
            size_t scanEnd = buffer.size() - FRAME_MAX_SAMPLES;
            const float* m = buffer.data();

            for (size_t j = 0; j <= scanEnd; ) {
                if (!detectPreamble(m, j)) { j++; continue; }
                RawFrame f{};
                if (decodeFrame(m + j + PREAMBLE_SAMPLES, f)) {
                    if (onFrame) onFrame(f);
                    // Skip past this frame to avoid re-triggering on its body.
                    j += PREAMBLE_SAMPLES + f.len * 8 * SAMPLES_PER_BIT;
                } else {
                    j++;
                }
            }

            // Retain the last FRAME_MAX_SAMPLES samples as carry-over.
            size_t keep = FRAME_MAX_SAMPLES;
            if (buffer.size() > keep) {
                size_t drop = buffer.size() - keep;
                buffer.erase(buffer.begin(), buffer.begin() + drop);
            }
        }

    private:
        std::vector<float> buffer;

        // Classic dump1090 preamble shape test.
        static bool detectPreamble(const float* m, size_t j) {
            if (!(m[j]   > m[j+1] &&
                  m[j+1] < m[j+2] &&
                  m[j+2] > m[j+3] &&
                  m[j+3] < m[j]   &&
                  m[j+4] < m[j]   &&
                  m[j+5] < m[j]   &&
                  m[j+6] < m[j]   &&
                  m[j+7] > m[j+8] &&
                  m[j+8] < m[j+9] &&
                  m[j+9] > m[j+6])) {
                return false;
            }
            // The energy of the pulses, averaged.
            float high = (m[j] + m[j+2] + m[j+7] + m[j+9]) / 6.0f;
            // Samples 4,5 must be quiet (between the 2nd and 3rd pulses).
            if (m[j+4] >= high || m[j+5] >= high) return false;
            // Samples 11..14 (data start guard) must be quiet too.
            if (m[j+11] >= high || m[j+12] >= high ||
                m[j+13] >= high || m[j+14] >= high) return false;
            return true;
        }

        // Slice PPM bits and validate CRC. `data` points at the first data sample.
        static bool decodeFrame(const float* data, RawFrame& out) {
            uint8_t bytes[14] = {0};
            // Decode the first 56 bits to read the DF and decide the length.
            for (int i = 0; i < SHORT_BITS; i++) {
                float a = data[i * 2];
                float b = data[i * 2 + 1];
                int bit = (a > b) ? 1 : 0;
                bytes[i >> 3] |= bit << (7 - (i & 7));
            }
            int df = (bytes[0] >> 3) & 0x1F;
            int lenBits = (df >= 16) ? LONG_BITS : SHORT_BITS;

            if (lenBits == LONG_BITS) {
                for (int i = SHORT_BITS; i < LONG_BITS; i++) {
                    float a = data[i * 2];
                    float b = data[i * 2 + 1];
                    int bit = (a > b) ? 1 : 0;
                    bytes[i >> 3] |= bit << (7 - (i & 7));
                }
            }

            int lenBytes = lenBits / 8;
            if (crc(bytes, lenBytes) != 0) return false;

            memcpy(out.bytes, bytes, lenBytes);
            out.len = lenBytes;
            return true;
        }
    };

} // namespace adsb
