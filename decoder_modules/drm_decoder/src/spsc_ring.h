#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>

/*
 * Single-producer / single-consumer lock-free ring buffer.
 *
 * The SDR++ DSP thread is the *producer* (it pushes baseband I/Q coming out
 * of the VFO). The Dream receiver thread is the *consumer* (it pulls samples
 * inside CSoundInInterface::Read). Keeping this path lock-free avoids any
 * mutex contention on the 48 kHz I/Q hot path.
 *
 * Overflow policy: LOSSLESS BACKPRESSURE.
 *
 * DRM is an OFDM waveform: its receiver locks onto the signal by tracking the
 * timing and frequency of the OFDM symbols. Any *discontinuity* in the sample
 * stream (a dropped block, a phase jump) destroys that synchronisation and the
 * receiver will never lock again - regardless of how strong the signal is.
 *
 * An earlier "drop oldest" policy kept SDR++ responsive but silently punched
 * holes in the I/Q whenever the decoder briefly fell behind, which prevented
 * the receiver from ever acquiring even on a 25 dB SNR signal. We therefore
 * apply backpressure instead: if the buffer is full, the producer waits for
 * the consumer to drain it rather than overwriting unread samples. The ring is
 * sized for several seconds of audio so this only ever happens on a transient
 * overrun, and the consumer's Read() is itself blocking, so the two stay in
 * lock-step without losing a single sample.
 */
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2 = (1u << 19)) {
        // Round up to the next power of two so we can mask instead of modulo.
        size_t cap = 1;
        while (cap < capacityPow2) cap <<= 1;
        mask = cap - 1;
        buf.resize(cap);
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
        aborted.store(false, std::memory_order_relaxed);
    }

    size_t capacity() const { return buf.size(); }

    size_t available() const {
        const size_t h = head.load(std::memory_order_acquire);
        const size_t t = tail.load(std::memory_order_acquire);
        return h - t;
    }

    size_t freeSpace() const {
        return (buf.size() - 1) - available();
    }

    /*
     * Producer side. LOSSLESS: blocks (briefly) until there is room rather than
     * discarding unread samples, so the OFDM stream stays continuous. Returns
     * early without writing if abort() was called (module shutting down).
     */
    void write(const T* data, size_t count) {
        const size_t cap = buf.size();
        size_t written = 0;
        while (written < count) {
            if (aborted.load(std::memory_order_acquire)) return;

            size_t h = head.load(std::memory_order_relaxed);
            const size_t t = tail.load(std::memory_order_acquire);
            size_t freeNow = (cap - 1) - (h - t);

            if (freeNow == 0) {
                /* Buffer full: yield and let the consumer catch up. This is the
                   backpressure that preserves stream continuity. */
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }

            size_t chunk = count - written;
            if (chunk > freeNow) chunk = freeNow;
            for (size_t i = 0; i < chunk; i++) {
                buf[(h + i) & mask] = data[written + i];
            }
            head.store(h + chunk, std::memory_order_release);
            written += chunk;
        }
    }

    /* Consumer side. Returns the number of samples actually read (may be < count). */
    size_t read(T* out, size_t count) {
        const size_t h = head.load(std::memory_order_acquire);
        size_t t = tail.load(std::memory_order_relaxed);
        size_t avail = h - t;
        if (avail < count) count = avail;
        for (size_t i = 0; i < count; i++) {
            out[i] = buf[(t + i) & mask];
        }
        tail.store(t + count, std::memory_order_release);
        return count;
    }

    /* Unblock a producer stuck in write() (call before shutting down). */
    void abort() { aborted.store(true, std::memory_order_release); }

    void clear() {
        aborted.store(false, std::memory_order_release);
        tail.store(head.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<T> buf;
    size_t mask;
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
    std::atomic<bool>   aborted;
};
