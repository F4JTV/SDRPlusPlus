#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <signal_path/vfo_manager.h>
#include <dsp/sink/handler_sink.h>
#include "dsp.h"
#include "adsb/adsb.h"
#include "adsb/frame_detector.h"

// 2 MHz sampling => exactly 2 samples per 1 us Mode S bit (classic dump1090).
#define ADSB_SAMPLERATE 2000000.0
#define ADSB_BANDWIDTH  2000000.0

class ADSBDecoder {
public:
    // Called (on the DSP thread) for every CRC-valid frame, with its parsed
    // message (kind == KIND_UNKNOWN if the DF is not an ADS-B message we parse).
    std::function<void(const adsb::Message&, const adsb::RawFrame&)> onMessage;

    ADSBDecoder(const std::string& name, VFOManager::VFO* vfo) {
        this->name = name;
        this->vfo = vfo;

        // Configure the VFO for a wide 2 MHz channel centred on the carrier.
        vfo->setBandwidthLimits(ADSB_BANDWIDTH, ADSB_BANDWIDTH, true);
        vfo->setSampleRate(ADSB_SAMPLERATE, ADSB_BANDWIDTH);

        // DSP chain: complex baseband -> magnitude -> handler -> detector.
        dsp.init(vfo->output);
        dataHandler.init(&dsp.out, _dataHandler, this);

        detector.onFrame = [this](const adsb::RawFrame& f) { this->handleFrame(f); };
    }

    ~ADSBDecoder() { stop(); }

    void setVFO(VFOManager::VFO* vfo) {
        this->vfo = vfo;
        vfo->setBandwidthLimits(ADSB_BANDWIDTH, ADSB_BANDWIDTH, true);
        vfo->setSampleRate(ADSB_SAMPLERATE, ADSB_BANDWIDTH);
        dsp.setInput(vfo->output);
    }

    void start() {
        if (running) { return; }
        detector.reset();
        dsp.start();
        dataHandler.start();
        running = true;
    }

    void stop() {
        if (!running) { return; }
        dsp.stop();
        dataHandler.stop();
        running = false;
    }

    // Stats for the GUI.
    uint64_t getFrameCount() const { return frameCount.load(); }

private:
    static void _dataHandler(float* data, int count, void* ctx) {
        ADSBDecoder* _this = (ADSBDecoder*)ctx;
        _this->detector.process(data, count);
    }

    void handleFrame(const adsb::RawFrame& f) {
        frameCount.fetch_add(1);
        adsb::Message m;
        adsb::parseAdsb(f.bytes, m); // m.kind stays UNKNOWN if not an ADS-B msg
        if (onMessage) { onMessage(m, f); }
    }

    std::string name;
    VFOManager::VFO* vfo = nullptr;
    bool running = false;

    MagnitudeDSP dsp;
    dsp::sink::Handler<float> dataHandler;
    adsb::FrameDetector detector;

    std::atomic<uint64_t> frameCount{0};
};
