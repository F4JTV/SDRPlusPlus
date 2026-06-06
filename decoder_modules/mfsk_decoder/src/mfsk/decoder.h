#pragma once
#include "../decoder.h"
#include "modem.h"
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/demod/ssb.h>
#include <dsp/demod/fm.h>
#include <dsp/sink/handler_sink.h>
#include <algorithm>

// MFSK decoder DSP chain (same structure as the working psk_decoder):
//
//   VFO IQ -> PowerSquelch -> SSB(USB/LSB) or FM(NFM) -> Handler sink -> MFSKModem
//
// CANONICAL SDR++ PATTERN (identical to m17 / radio / psk_decoder):
//   * every DSP block is a MEMBER, init()'d EXACTLY ONCE in buildDSP() (called
//     from the constructor). init() registers a block's input/output streams;
//     calling it twice double-registers them and CRASHES SDR++.
//   * start()/stop() only ever call start()/stop()/setInput()/setMode() - NEVER
//     init(). Both demodulators are wired to the squelch output but only one is
//     ever started at a time (the stopped one's worker never reads the stream).

namespace mfsk {

    static constexpr double MFSK_SSB_BW = 3000.0;   // SSB channel bandwidth (Hz)
    static constexpr double MFSK_NFM_BW = 12500.0;  // NFM bandwidth (Hz)

    class MFSKDecoder : public Decoder {
    public:
        MFSKDecoder(dsp::stream<dsp::complex_t>* in) {
            vfoInput = in;
            modem.configure(modeIdx, afFreq);
            buildDSP();
        }

        ~MFSKDecoder() { stop(); }

        // --- one-time DSP construction (init each block exactly once) ---------
        void buildDSP() {
            squelch.init(vfoInput, currentSquelchLevel());

            // Both demodulators consume the squelch output; only one is started.
            ssb.init(&squelch.out, dsp::demod::SSB<float>::USB, MFSK_SSB_BW, AUDIO_SR,
                     100.0 / AUDIO_SR, 5.0 / AUDIO_SR);
            nfm.init(&squelch.out, AUDIO_SR, MFSK_NFM_BW, true);

            // Sink starts wired to the SSB output (USB is the default mode).
            sink.init(&ssb.out, sinkHandler, this);
        }

        void setInput(dsp::stream<dsp::complex_t>* in) override {
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            vfoInput = in;
            squelch.setInput(vfoInput);
            if (wasRunning) { start(); }
        }

        void setVFOMode(VfoMode mode) override {
            if (mode == vfoMode) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            vfoMode = mode;
            if (wasRunning) { start(); }
        }

        void setMode(int idx) override {
            idx = std::clamp(idx, 0, MFSK_MODE_COUNT - 1);
            if (idx == modeIdx) { return; }
            bool wasRunning = running;
            if (wasRunning) { stop(); }
            modeIdx = idx;
            modem.configure(modeIdx, afFreq);
            if (wasRunning) { start(); }
        }

        void setAFFreq(double freq) override {
            afFreq = freq;
            modem.setAFFreq(freq);
        }

        void setAFCEnabled(bool en) override { afcEnabled = en; modem.setAFC(en); }
        bool getAFCEnabled() const override { return afcEnabled; }
        double getTrackedAFFreq() const override { return modem.getTrackedAF(); }

        int    getBandSpectrum(float* out, int n) const override { return modem.getBandSpectrum(out, n); }
        double getBandFlo() const override { return modem.getBandFlo(); }
        double getBandFhi() const override { return modem.getBandFhi(); }
        double getEdgeFreq() const override { return modem.getEdgeFreq(); }
        double getToneSpan() const override { return modem.getToneSpan(); }

        void setSquelchEnabled(bool en) override {
            squelchEnabled = en;
            squelch.setLevel(currentSquelchLevel());
        }

        void setSquelchLevel(float dB) override {
            squelchLevel = std::clamp(dB, MFSK_SQUELCH_MIN, MFSK_SQUELCH_MAX);
            squelch.setLevel(currentSquelchLevel());
        }

        bool  getSquelchEnabled() const override { return squelchEnabled; }
        float getSquelchLevel()   const override { return squelchLevel; }

        int   getTone() const override { return modem.getTone(); }
        float getSNR()  const override { return modem.getSNR(); }

        // Fill a constellation buffer with the recent dominant-tone scope points.
        void getConstellation(dsp::complex_t* buf, int n) {
            static std::complex<float> tmp[1024];
            int c = (n < 1024) ? n : 1024;
            modem.getScope(tmp, c);
            for (int i = 0; i < c; i++) { buf[i].re = tmp[i].real(); buf[i].im = tmp[i].imag(); }
        }

        // --- start/stop: NO init() here, only start/stop/setInput/setMode -----
        void start() override {
            if (running) { return; }

            modem.configure(modeIdx, afFreq);

            squelch.setInput(vfoInput);
            squelch.setLevel(currentSquelchLevel());
            squelch.start();

            if (vfoMode == VFO_MODE_NFM) {
                nfm.setInput(&squelch.out);
                sink.setInput(&nfm.out);
                nfm.start();
            }
            else {
                ssb.setMode((vfoMode == VFO_MODE_LSB) ? dsp::demod::SSB<float>::LSB
                                                      : dsp::demod::SSB<float>::USB);
                ssb.setInput(&squelch.out);
                sink.setInput(&ssb.out);
                ssb.start();
            }

            sink.start();
            running = true;
        }

        void stop() override {
            if (!running) { return; }
            sink.stop();
            ssb.stop();      // stop() on a non-started block is a safe no-op
            nfm.stop();
            squelch.stop();
            running = false;
        }

    private:
        static void sinkHandler(float* data, int count, void* ctx) {
            auto* _this = (MFSKDecoder*)ctx;
            _this->modem.process(data, count, [=](char c) {
                if (_this->onChar) { _this->onChar(c); }
            });
        }

        float currentSquelchLevel() const {
            return squelchEnabled ? squelchLevel : MFSK_SQUELCH_MIN;
        }

        dsp::stream<dsp::complex_t>* vfoInput = nullptr;

        // DSP blocks (members, init'd once)
        dsp::noise_reduction::PowerSquelch squelch;
        dsp::demod::SSB<float>             ssb;
        dsp::demod::FM<float>              nfm;
        dsp::sink::Handler<float>          sink;

        MFSKModem modem;

        bool    running        = false;
        VfoMode vfoMode        = VFO_MODE_USB;
        int     modeIdx        = 3;       // default MFSK16 (index in MFSK_MODES)
        double  afFreq         = 1500.0;
        bool    squelchEnabled = true;
        bool    afcEnabled     = true;     // automatic AF acquisition + tracking
        float   squelchLevel   = -50.0f;
    };

}
