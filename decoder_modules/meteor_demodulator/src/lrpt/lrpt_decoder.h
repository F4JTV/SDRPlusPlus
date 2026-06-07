#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

#include "codings/correlator.h"
#include "codings/rotation.h"
#include "codings/randomization.h"
#include "codings/nrzm.h"
#include "codings/reedsolomon.h"
#include "codings/viterbi/viterbi27.h"

#include "ccsds/vcdu.h"
#include "ccsds/demuxer.h"
#include "msumr/lrpt_msumr_reader.h"
#include "msumr/simple_image.h"

/*
 * Full Meteor-M2 LRPT decoder, ported from SatDump's meteor_support plugin.
 *
 * Pipeline (matches SatDump's non-m2x QPSK path exactly):
 *   soft QPSK symbols (int8 I/Q)
 *     -> Correlator (find CCSDS ASM, resolve QPSK phase ambiguity)
 *     -> rotate_soft (phase correction)
 *     -> Viterbi r=1/2 k=7 (CCSDS_R2_K7_POLYS)  -> 1024-byte frame
 *     -> NRZ-M differential decode (optional)
 *     -> CCSDS derandomization
 *     -> Reed-Solomon (255,223) interleave 4
 *     -> CADU (1024 bytes)
 *   CADU
 *     -> VCDU parse, keep vcid == 5 (MSU-MR)
 *     -> CCSDS AOS demuxer (M-PDU size 882, with insert zone)
 *     -> MSU-MR LRPT reader  (APID 64..69 = channels 0..5)
 *     -> per-channel grayscale image + RGB composite
 *
 * Fed soft symbols incrementally (push model); can be queried at any time.
 * Thread-safe. Non-copyable sub-objects live behind unique_ptr and are
 * recreated on reset()/diff-mode change.
 */
class LRPTDecoder {
public:
    static constexpr int FRAME_SIZE = 1024;
    static constexpr int ENCODED_FRAME_SIZE = 1024 * 8 * 2; // 16384 soft symbols

    explicit LRPTDecoder(bool diff_decode = false) : d_diff_decode(diff_decode) {
        build();
        symbuf.reserve(ENCODED_FRAME_SIZE * 2);
    }

    // Feed soft QPSK symbols (interleaved I,Q int8 soft values).
    void pushSymbols(const int8_t* data, int count) {
        std::lock_guard<std::mutex> lck(mtx);
        symbuf.insert(symbuf.end(), data, data + count);
        processBuffered();
    }

    // Stats for the UI.
    bool     isLocked()   { std::lock_guard<std::mutex> l(mtx); return locked; }
    float    getBER()     { std::lock_guard<std::mutex> l(mtx); return viterbi->ber(); }
    int      getRSAvg()   { std::lock_guard<std::mutex> l(mtx); return (errors[0] + errors[1] + errors[2] + errors[3]) / 4; }
    uint64_t getCADUs()   { std::lock_guard<std::mutex> l(mtx); return caduCount; }
    uint64_t getPackets() { std::lock_guard<std::mutex> l(mtx); return packetCount; }

    void reset() {
        std::lock_guard<std::mutex> l(mtx);
        symbuf.clear();
        locked = false;
        caduCount = packetCount = 0;
        for (int i = 0; i < 4; i++) errors[i] = 0;
        build();
    }

    void setDiffDecode(bool v) {
        std::lock_guard<std::mutex> l(mtx);
        if (v == d_diff_decode) return;
        d_diff_decode = v;
        correlator = std::make_unique<Correlator>(QPSK, v ? 0xfc4ef4fd0cc2df89 : 0xfca2b63db00d9794);
    }
    bool getDiffDecode() { std::lock_guard<std::mutex> l(mtx); return d_diff_decode; }

    std::vector<int> activeChannels() {
        std::lock_guard<std::mutex> l(mtx);
        std::vector<int> out;
        for (int c = 0; c < 6; c++)
            if (reader->getChannel(c).size() > 0) out.push_back(c);
        return out;
    }

    meteorimg::SimpleImage getChannelImage(int channel) {
        std::lock_guard<std::mutex> l(mtx);
        return reader->getChannel(channel);
    }

    bool getComposite(int rCh, int gCh, int bCh, std::vector<uint8_t>& rgb, int& width, int& height) {
        std::lock_guard<std::mutex> l(mtx);
        meteorimg::SimpleImage r = reader->getChannel(rCh);
        meteorimg::SimpleImage g = reader->getChannel(gCh);
        meteorimg::SimpleImage b = reader->getChannel(bCh);
        size_t w = 0, h = 0;
        for (auto* im : {&r, &g, &b}) {
            if (im->width() > w) w = im->width();
            if (im->height() > h) h = im->height();
        }
        if (w == 0 || h == 0) return false;
        width = (int)w; height = (int)h;
        rgb.assign(w * h * 3, 0);
        auto sample = [](meteorimg::SimpleImage& im, size_t idx) -> uint8_t {
            return (idx < im.size()) ? im.get(idx) : 0;
        };
        for (size_t i = 0; i < w * h; i++) {
            rgb[i * 3 + 0] = sample(r, i);
            rgb[i * 3 + 1] = sample(g, i);
            rgb[i * 3 + 2] = sample(b, i);
        }
        return true;
    }

private:
    void build() {
        correlator = std::make_unique<Correlator>(QPSK, d_diff_decode ? 0xfc4ef4fd0cc2df89 : 0xfca2b63db00d9794);
        rs         = std::make_unique<reedsolomon::ReedSolomon>(reedsolomon::RS223);
        viterbi    = std::make_unique<viterbi::Viterbi27>(ENCODED_FRAME_SIZE / 2, viterbi::CCSDS_R2_K7_POLYS);
        demuxer    = std::make_unique<ccsds::ccsds_aos::Demuxer>(882, true);
        reader     = std::make_unique<meteor::msumr::lrpt::MSUMRReader>(false);
        nrzm       = diff::NRZMDiff();
    }

    // Mirror of SatDump's process() non-m2x loop, in push form.
    void processBuffered() {
        while ((int)symbuf.size() >= ENCODED_FRAME_SIZE) {
            int8_t* buffer = symbuf.data();

            phase_t phase; bool swap; int cor = 0;
            int pos = correlator->correlate(buffer, phase, swap, cor, ENCODED_FRAME_SIZE);

            locked = (pos == 0);

            if (pos != 0) {
                int drop = (pos < ENCODED_FRAME_SIZE) ? pos : ENCODED_FRAME_SIZE;
                symbuf.erase(symbuf.begin(), symbuf.begin() + drop);
                continue;
            }

            rotate_soft(buffer, ENCODED_FRAME_SIZE, phase, swap);
            viterbi->work(buffer, frameBuffer);

            if (d_diff_decode)
                nrzm.decode(frameBuffer, FRAME_SIZE);

            derand_ccsds(&frameBuffer[4], FRAME_SIZE - 4);

            if (frameBuffer[9] == 0xFF)
                for (int i = 0; i < FRAME_SIZE; i++)
                    frameBuffer[i] ^= 0xFF;

            rs->decode_interlaved(&frameBuffer[4], false, 4, errors);

            if (errors[0] >= 0 && errors[1] >= 0 && errors[2] >= 0 && errors[3] >= 0) {
                cadu[0] = 0x1d; cadu[1] = 0xcf; cadu[2] = 0xfc; cadu[3] = 0x1d;
                std::memcpy(&cadu[4], &frameBuffer[4], FRAME_SIZE - 4);
                caduCount++;
                handleCADU(cadu);
            }

            symbuf.erase(symbuf.begin(), symbuf.begin() + ENCODED_FRAME_SIZE);
        }
    }

    void handleCADU(uint8_t* c) {
        ccsds::ccsds_aos::VCDU vcdu = ccsds::ccsds_aos::parseVCDU(c);
        if (vcdu.vcid != 5) return; // MSU-MR imagery only
        std::vector<ccsds::CCSDSPacket> pkts = demuxer->work(c);
        for (ccsds::CCSDSPacket& pkt : pkts) {
            packetCount++;
            reader->work(pkt);
        }
    }

    bool d_diff_decode;
    std::mutex mtx;

    std::unique_ptr<Correlator> correlator;
    std::unique_ptr<reedsolomon::ReedSolomon> rs;
    std::unique_ptr<viterbi::Viterbi27> viterbi;
    std::unique_ptr<ccsds::ccsds_aos::Demuxer> demuxer;
    std::unique_ptr<meteor::msumr::lrpt::MSUMRReader> reader;
    diff::NRZMDiff nrzm;

    std::vector<int8_t> symbuf;
    uint8_t frameBuffer[FRAME_SIZE];
    uint8_t cadu[1024];

    bool locked = false;
    int errors[4] = {0, 0, 0, 0};
    uint64_t caduCount = 0;
    uint64_t packetCount = 0;
};
