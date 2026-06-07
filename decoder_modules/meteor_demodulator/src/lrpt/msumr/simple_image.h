#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>

/*
 * Minimal 8-bit grayscale image, a drop-in replacement for the subset of
 * SatDump's image::Image API used by the MSU-MR LRPT reader. Keeps the port
 * dependency-free (no SatDump image library, no OpenCV).
 */
namespace meteorimg {
    class SimpleImage {
    public:
        SimpleImage() : d_width(0), d_height(0) {}

        // Signature kept identical to image::Image(bit_depth, width, height, channels)
        // bit_depth and channels are ignored (always 8-bit, 1 channel here).
        SimpleImage(int /*bit_depth*/, size_t width, size_t height, int /*channels*/)
            : d_width(width), d_height(height) {
            d_data.assign(width * height, 0);
        }

        inline void set(size_t idx, uint8_t val) {
            if (idx < d_data.size()) d_data[idx] = val;
        }
        inline uint8_t get(size_t idx) const {
            return idx < d_data.size() ? d_data[idx] : 0;
        }
        inline size_t size() const { return d_data.size(); }
        inline size_t width() const { return d_width; }
        inline size_t height() const { return d_height; }
        inline uint8_t* data() { return d_data.data(); }
        inline const uint8_t* data() const { return d_data.data(); }

    private:
        size_t d_width, d_height;
        std::vector<uint8_t> d_data;
    };

    // Replacement for satdump::most_common (used for per-line timestamps).
    template <typename Iter, typename T>
    inline T most_common(Iter begin, Iter end, T fallback) {
        if (begin == end) return fallback;
        T best = fallback;
        size_t bestCount = 0;
        for (Iter i = begin; i != end; ++i) {
            size_t c = 0;
            for (Iter j = begin; j != end; ++j)
                if (*j == *i) c++;
            if (c > bestCount) { bestCount = c; best = *i; }
        }
        return best;
    }
}
