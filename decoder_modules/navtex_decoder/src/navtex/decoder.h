#pragma once
#include "../decoder.h"
#include "modem.h"
#include <imgui.h>
#include <config.h>
#include <gui/style.h>
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/sink/handler_sink.h>
#include <string>
#include <mutex>
#include <vector>
#include <ctype.h>

// dsp::complex_t is {float re; float im;} so a complex_t buffer can be read
// by the modem as interleaved floats.
#include <dsp/types.h>

#define NAVTEX_CONCAT(a, b) ((std::string(a) + b).c_str())

extern ConfigManager config;

// NAVTEX is fixed-format: SITOR-B (CCIR 476 FEC), 100 baud, 170 Hz shift. The
// preset table keeps the same shape as the RTTY module's so the shared "Mode"
// combo renders identically; here it holds the single standard configuration.
struct NavtexPreset { const char* name; double baud; double shift; };
static const NavtexPreset NAVTEX_PRESETS[] = {
    { "SITOR-B  100 baud / 170 Hz", 100.0, 170.0 },
};
static const int NAVTEX_PRESET_COUNT = sizeof(NAVTEX_PRESETS) / sizeof(NAVTEX_PRESETS[0]);
static const char* NAVTEX_PRESET_LIST = "SITOR-B  100 baud / 170 Hz\0";

// NAVTEX message subject indicator (header character B2).
static inline const char* navtexSubject(char b2) {
    switch (toupper((unsigned char)b2)) {
        case 'A': return "Navigational warning";
        case 'B': return "Meteorological warning";
        case 'C': return "Ice report";
        case 'D': return "Search & rescue, piracy";
        case 'E': return "Meteorological forecast";
        case 'F': return "Pilot service message";
        case 'G': return "AIS message";
        case 'H': return "LORAN message";
        case 'I': return "Not used";
        case 'J': return "SATNAV message";
        case 'K': return "Other navaid message";
        case 'L': return "Navigational warning (extra)";
        case 'T': return "Test transmission";
        case 'V': return "Notice to fishermen";
        case 'W': return "Environmental";
        case 'X': return "Special service";
        case 'Y': return "Special service";
        case 'Z': return "No message on hand";
        default:  return "Unknown subject";
    }
}

class NavtexDecoder : public Decoder {
public:
    NavtexDecoder(const std::string& name, VFOManager::VFO* vfo) {
        this->name = name;
        this->vfo  = vfo;

        config.acquire();
        if (config.conf[name].contains("reverse")) { reverse = config.conf[name]["reverse"]; }
        if (config.conf[name].contains("modeIdx")) { modeIdx = config.conf[name]["modeIdx"]; }
        config.release(false);
        if (modeIdx < 0 || modeIdx >= NAVTEX_PRESET_COUNT) { modeIdx = 0; }

        modem.onChar = [this](char c) { this->onChar(c); };
        applyModeConfig();
    }

    ~NavtexDecoder() { stop(); }

    void setVFO(VFOManager::VFO* v) override { vfo = v; }

    void start() override {
        if (running || !vfo) { return; }
        squelch.init(vfo->output, squelchEnabled ? squelchLevel : -150.0);
        sink.init(&squelch.out, handler, this);
        squelch.start();
        sink.start();
        running = true;
    }

    void stop() override {
        if (!running) { return; }
        sink.stop();
        squelch.stop();
        running = false;
    }

    // --- shared control surface ---
    void setVFOMode(int mode) override { vfoMode = mode; modem.setSideband(mode); }
    void setMode(int idx) override {
        if (idx < 0 || idx >= NAVTEX_PRESET_COUNT) { return; }
        modeIdx = idx;
        applyModeConfig();
    }
    void setAFFreq(double f) override { afFreq = f; modem.setAFFreq(f); }
    float getAFFreq() const override { return (float)modem.getAFFreq(); }
    void setSquelchEnabled(bool en) override {
        squelchEnabled = en;
        if (running) { squelch.setLevel(en ? squelchLevel : -150.0); }
    }
    void setSquelchLevel(float dB) override {
        squelchLevel = dB;
        if (running && squelchEnabled) { squelch.setLevel(dB); }
    }
    void setAFCEnabled(bool en) override { afc = en; modem.setAFCEnabled(en); }
    double getTrackedAFFreq() const override { return modem.getTrackedAFFreq(); }

    int getBandSpectrum(float* out, int n) const override { return modem.getBandSpectrum(out, n); }
    double getBandFlo() const override { return modem.getBandFlo(); }
    double getBandFhi() const override { return modem.getBandFhi(); }
    double getSignalBandwidth() const override { return modem.getSignalBandwidth(); }

    // --- NAVTEX-specific UI (status + decoded text + reverse) ---
    void showMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (ImGui::Checkbox(NAVTEX_CONCAT("Reverse##navtex_rev_", name), &reverse)) {
            applyModeConfig();
            config.acquire(); config.conf[name]["reverse"] = reverse; config.release(true);
        }
        ImGui::SameLine();
        if (modem.getSynced()) {
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "FEC synced");
        } else {
            ImGui::TextDisabled("searching...");
        }

        // Current message header (origin / subject / serial), when known.
        {
            std::lock_guard<std::mutex> lck(textMtx);
            if (haveHeader) {
                ImGui::TextDisabled("Msg %c%c%02d  %s",
                                    hdrB1, hdrB2, hdrNum, navtexSubject(hdrB2));
            } else {
                ImGui::TextDisabled("Msg --");
            }
        }

        // Decoded text area.
        ImGui::BeginChild(NAVTEX_CONCAT("##navtex_text_", name),
                          ImVec2(menuWidth, 180.0f * style::uiScale), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lck(textMtx);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(decodedText.c_str());
            ImGui::PopTextWrapPos();
        }
        if (scrollDown) { ImGui::SetScrollHereY(1.0f); scrollDown = false; }
        ImGui::EndChild();

        if (ImGui::Button(NAVTEX_CONCAT("Clear##navtex_clr_", name), ImVec2(menuWidth, 0))) {
            std::lock_guard<std::mutex> lck(textMtx);
            decodedText.clear();
            haveHeader = false;
        }

        if (ImGui::CollapsingHeader(NAVTEX_CONCAT("Message subjects (B2)##navtex_leg_", name))) {
            ImGui::TextDisabled(
                "A nav warning   B met warning   C ice report\n"
                "D SAR/piracy    E met forecast  F pilot service\n"
                "G AIS           H LORAN         J SATNAV\n"
                "K other navaid  L nav warning   T test\n"
                "V to fishermen  W environmental Z no message");
        }
    }

private:
    void applyModeConfig() {
        const NavtexPreset& p = NAVTEX_PRESETS[modeIdx];
        modem.configure(sampleRate, p.baud, p.shift, reverse);
        modem.setSideband(vfoMode);
        modem.setAFFreq(afFreq);
        modem.setAFCEnabled(afc);
    }

    // Track the "ZCZC B1 B2 NN" message header out of the decoded stream.
    void updateHeader(char c) {
        // Rolling 4-char window to spot "ZCZC".
        win[0] = win[1]; win[1] = win[2]; win[2] = win[3]; win[3] = c;
        if (!capturing && win[0] == 'Z' && win[1] == 'C' && win[2] == 'Z' && win[3] == 'C') {
            capturing = true; capCount = 0; return;
        }
        if (capturing) {
            if (capCount == 0 && (c == ' ' || c == '\r' || c == '\n')) { return; } // skip gap
            if (isalnum((unsigned char)c)) {
                capBuf[capCount++] = c;
                if (capCount >= 4) {
                    hdrB1  = toupper((unsigned char)capBuf[0]);
                    hdrB2  = toupper((unsigned char)capBuf[1]);
                    hdrNum = (isdigit((unsigned char)capBuf[2]) ? (capBuf[2]-'0') : 0) * 10
                           + (isdigit((unsigned char)capBuf[3]) ? (capBuf[3]-'0') : 0);
                    haveHeader = true;
                    capturing = false;
                }
            } else {
                capturing = false; // header malformed, give up
            }
        }
    }

    void onChar(char c) {
        std::lock_guard<std::mutex> lck(textMtx);
        decodedText.push_back(c);
        if (decodedText.size() > 20000) { decodedText.erase(0, decodedText.size() - 20000); }
        updateHeader(c);
        scrollDown = true;
    }

    static void handler(dsp::complex_t* data, int count, void* ctx) {
        NavtexDecoder* _this = (NavtexDecoder*)ctx;
        _this->modem.process((const float*)data, count);
    }

    std::string name;
    VFOManager::VFO* vfo = NULL;
    bool running = false;

    dsp::noise_reduction::PowerSquelch squelch;
    dsp::sink::Handler<dsp::complex_t> sink;
    navtex::Modem modem;

    double sampleRate   = 24000.0;
    double afFreq       = 1000.0;
    int    vfoMode      = 0;       // USB
    int    modeIdx      = 0;
    bool   reverse      = false;
    bool   afc          = false;
    bool   squelchEnabled = false;
    float  squelchLevel = -50.0f;

    std::mutex  textMtx;
    std::string decodedText;
    bool scrollDown = false;

    // header tracking
    char win[4]    = {0, 0, 0, 0};
    bool capturing = false;
    int  capCount  = 0;
    char capBuf[4] = {0, 0, 0, 0};
    bool haveHeader = false;
    char hdrB1 = '?', hdrB2 = '?';
    int  hdrNum = 0;
};
