#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <gui/widgets/constellation_diagram.h>
#include <utils/optionlist.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include "decoder.h"
#include "mfsk/decoder.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "mfsk_decoder",
    /* Description:     */ "MFSK decoder (MFSK4..128 + L variants, FLDIGI family)",
    /* Author:          */ "F4JTV",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define VFO_SAMPLE_RATE  (mfsk::AUDIO_SR)
#define VFO_BANDWIDTH    3000.0

class MFSKDecoderModule : public ModuleManager::Instance {
public:
    MFSKDecoderModule(std::string name) {
        this->name = name;

        // Snap interval options (Hz) for tuning on the waterfall.
        snapIntervals.define(1,    "1 Hz",    1.0);
        snapIntervals.define(10,   "10 Hz",   10.0);
        snapIntervals.define(100,  "100 Hz",  100.0);
        snapIntervals.define(250,  "250 Hz",  250.0);
        snapIntervals.define(500,  "500 Hz",  500.0);
        snapIntervals.define(1000, "1 kHz",   1000.0);

        // Load config (defensively: a corrupt config left by an earlier crash
        // must never throw an uncaught exception out of the constructor).
        config.acquire();
        try {
            auto& c = config.conf[name];
            if (c.contains("vfoMode")        && c["vfoMode"].is_number())        { vfoMode        = c["vfoMode"]; }
            if (c.contains("modeIdx")        && c["modeIdx"].is_number())        { modeIdx        = c["modeIdx"]; }
            if (c.contains("snapId")         && c["snapId"].is_number())         { snapId         = c["snapId"]; }
            if (c.contains("afFreq")         && c["afFreq"].is_number())         { afFreq         = c["afFreq"]; }
            if (c.contains("squelchEnabled") && c["squelchEnabled"].is_boolean()){ squelchEnabled = c["squelchEnabled"]; }
            if (c.contains("afcEnabled")     && c["afcEnabled"].is_boolean())    { afcEnabled     = c["afcEnabled"]; }
            if (c.contains("squelchLevel")   && c["squelchLevel"].is_number())   { squelchLevel   = c["squelchLevel"]; }
        }
        catch (...) { /* ignore bad config, fall back to defaults */ }
        config.release();

        vfoMode = std::clamp(vfoMode, 0, 2);
        modeIdx = std::clamp(modeIdx, 0, mfsk::MFSK_MODE_COUNT - 1);
        snapId  = std::clamp(snapId, 0, snapIntervals.size() - 1);

        // Create VFO (proven-safe parameters: 48 kHz audio VFO, 3 kHz channel)
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 15000.0, false);
        if (!vfo) { return; }
        vfo->setSnapInterval(snapIntervals.value(snapId));

        // Create decoder and apply settings
        decoder = new mfsk::MFSKDecoder(vfo->output);
        decoder->onChar = [this](char c) { onChar(c); };
        decoder->setVFOMode((mfsk::VfoMode)vfoMode);
        applyVfoGeometry();
        decoder->setMode(modeIdx);
        decoder->setAFFreq(afFreq);
        decoder->setSquelchEnabled(squelchEnabled);
        decoder->setAFCEnabled(afcEnabled);
        decoder->setSquelchLevel(squelchLevel);
        decoder->start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~MFSKDecoderModule() {
        gui::menu.removeEntry(name);
        if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0,
                                            VFO_BANDWIDTH, VFO_SAMPLE_RATE, 200.0, 15000.0, false);
        if (!vfo) { return; }
        vfo->setSnapInterval(snapIntervals.value(snapId));
        applyVfoGeometry();
        if (decoder) {
            decoder->setInput(vfo->output);
            decoder->start();
        }
        enabled = true;
    }

    void disable() {
        if (decoder) { decoder->stop(); }
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
        enabled = false;
    }

    bool isEnabled() { return enabled; }

    // Set the VFO reference + bandwidth so a single sideband is selected, exactly
    // like the PSK module: USB -> marker on the lower edge (passband above),
    // LSB -> marker on the upper edge (passband below), NFM -> centered.
    void applyVfoGeometry() {
        if (!vfo) { return; }
        if (vfoMode == mfsk::VFO_MODE_USB) {
            vfo->setBandwidthLimits(200.0, 15000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_LOWER);
            vfo->setBandwidth(mfsk::MFSK_SSB_BW);
        }
        else if (vfoMode == mfsk::VFO_MODE_LSB) {
            vfo->setBandwidthLimits(200.0, 15000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_UPPER);
            vfo->setBandwidth(mfsk::MFSK_SSB_BW);
        }
        else { // NFM
            vfo->setBandwidthLimits(5000.0, 25000.0, false);
            vfo->setReference(ImGui::WaterfallVFO::REF_CENTER);
            vfo->setBandwidth(mfsk::MFSK_NFM_BW);
        }
    }

private:
    void onChar(char c) {
        std::lock_guard<std::mutex> lck(textMtx);
        if (c == '\r') { return; }
        decodedText += c;
        // Cap the buffer so the GUI stays responsive.
        if (decodedText.size() > 16384) {
            decodedText.erase(0, decodedText.size() - 16384);
        }
    }

    static void menuHandler(void* ctx) {
        MFSKDecoderModule* _this = (MFSKDecoderModule*)ctx;
        if (!_this->decoder) { return; }
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // --- Mode -------------------------------------------------------------
        ImGui::LeftLabel("Mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mfsk_mode_", _this->name), &_this->modeIdx, modesStr())) {
            _this->modeIdx = std::clamp(_this->modeIdx, 0, mfsk::MFSK_MODE_COUNT - 1);
            _this->decoder->setMode(_this->modeIdx);
            _this->saveConfig();
        }

        // --- VFO mode (sideband) ---------------------------------------------
        ImGui::LeftLabel("VFO mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mfsk_vfomode_", _this->name), &_this->vfoMode, "USB\0LSB\0NFM\0")) {
            _this->vfoMode = std::clamp(_this->vfoMode, 0, 2);
            _this->decoder->setVFOMode((mfsk::VfoMode)_this->vfoMode);
            _this->applyVfoGeometry();
            _this->saveConfig();
        }

        // --- Snap interval ----------------------------------------------------
        ImGui::LeftLabel("Snap");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(CONCAT("##mfsk_snap_", _this->name), &_this->snapId, _this->snapIntervals.txt)) {
            if (_this->vfo) { _this->vfo->setSnapInterval(_this->snapIntervals.value(_this->snapId)); }
            _this->saveConfig();
        }

        // --- Band view: spectrum of the audio passband with a marker showing
        //     where the tones sit (left edge = AF). Click/drag to set the AF. ---
        {
            static float spec[256];
            int nb = _this->decoder->getBandSpectrum(spec, 256);
            double flo = _this->decoder->getBandFlo();
            double fhi = _this->decoder->getBandFhi();
            double edge = _this->afcEnabled ? _this->decoder->getEdgeFreq() : (double)_this->afFreq;
            double span = _this->decoder->getToneSpan();

            float w = menuWidth;
            float h = 56.0f;
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 cBg   = IM_COL32(20, 22, 28, 255);
            ImU32 cBar  = IM_COL32(90, 160, 230, 255);
            ImU32 cBand = IM_COL32(90, 160, 230, 60);
            ImU32 cEdge = IM_COL32(250, 200, 80, 255);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), cBg, 3.0f);

            // peak for normalisation
            float mx = 1e-6f; for (int i = 0; i < nb; i++) mx = std::max(mx, spec[i]);
            // spectrum bars
            for (int i = 0; i < nb; i++) {
                float x = p0.x + w * (i / (float)(nb - 1));
                float bh = (spec[i] / mx) * (h - 4);
                dl->AddLine(ImVec2(x, p0.y + h - 2), ImVec2(x, p0.y + h - 2 - bh), cBar, 1.0f);
            }
            // tone band (edge .. edge+span) shaded, plus the left-edge marker line
            auto f2x = [&](double f) { return p0.x + w * (float)((f - flo) / (fhi - flo)); };
            if (span > 0) {
                float xa = f2x(edge), xb = f2x(edge + span);
                dl->AddRectFilled(ImVec2(xa, p0.y), ImVec2(xb, p0.y + h), cBand);
            }
            float xe = f2x(edge);
            dl->AddLine(ImVec2(xe, p0.y), ImVec2(xe, p0.y + h), cEdge, 2.0f);

            // interactive: click/drag sets AF (= left edge / tone 0)
            ImGui::InvisibleButton(CONCAT("##mfsk_band_", _this->name), ImVec2(w, h));
            if (ImGui::IsItemActive() && !_this->afcEnabled) {
                float mxs = ImGui::GetIO().MousePos.x;
                double f = flo + (double)((mxs - p0.x) / w) * (fhi - flo);
                f = std::round(f);
                if (f < 300) f = 300; if (f > 2600) f = 2600;
                _this->afFreq = (float)f;
                _this->decoder->setAFFreq(_this->afFreq);
            }
            if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(0)))
                _this->saveConfig();
        }

        // --- AFC (automatic AF acquisition + tracking) ------------------------
        if (ImGui::Checkbox(CONCAT("AFC##mfsk_afc_", _this->name), &_this->afcEnabled)) {
            _this->decoder->setAFCEnabled(_this->afcEnabled);
            _this->saveConfig();
        }
        if (_this->afcEnabled) {
            ImGui::SameLine();
            ImGui::TextDisabled("(auto: %.0f Hz)", _this->decoder->getTrackedAFFreq());
        }

        // --- AF frequency -----------------------------------------------------
        // When AFC is on the modem owns the AF, so the manual slider is shown
        // disabled. For manual modes (MFSK4/8) use the band view above or this.
        ImGui::LeftLabel("AF freq");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::BeginDisabled(_this->afcEnabled);
        if (ImGui::SliderFloat(CONCAT("##mfsk_af_", _this->name), &_this->afFreq, 300.0f, 2600.0f, "%.0f Hz")) {
            _this->decoder->setAFFreq(_this->afFreq);
            _this->saveConfig();
        }
        ImGui::EndDisabled();

        // --- Squelch ----------------------------------------------------------
        if (ImGui::Checkbox(CONCAT("Squelch##mfsk_sqen_", _this->name), &_this->squelchEnabled)) {
            _this->decoder->setSquelchEnabled(_this->squelchEnabled);
            _this->saveConfig();
        }
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::SliderFloat(CONCAT("##mfsk_sql_", _this->name), &_this->squelchLevel,
                               mfsk::MFSK_SQUELCH_MIN, mfsk::MFSK_SQUELCH_MAX, "%.1f dB")) {
            _this->decoder->setSquelchLevel(_this->squelchLevel);
            _this->saveConfig();
        }

        // --- Scope (dominant-tone constellation) ------------------------------
        // Full menu width, square, and responsive: menuWidth is recomputed every
        // frame from the available content region, so it follows the menu size.
        {
            float scopeW = ImGui::GetContentRegionAvail().x;
            dsp::complex_t* cbuf = _this->diag.acquireBuffer();
            _this->decoder->getConstellation(cbuf, 1024);
            _this->diag.releaseBuffer();
            _this->diag.draw(ImVec2(scopeW, scopeW));
        }

        // --- Decoded text -----------------------------------------------------
        ImGui::TextUnformatted("Decoded text:");
        {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            ImGui::BeginChild(CONCAT("##mfsk_txt_", _this->name), ImVec2(menuWidth, 200.0f), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(_this->decodedText.c_str(),
                                   _this->decodedText.c_str() + _this->decodedText.size());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        if (ImGui::Button(CONCAT("Clear##mfsk_clr_", _this->name), ImVec2(menuWidth, 0))) {
            std::lock_guard<std::mutex> lck(_this->textMtx);
            _this->decodedText.clear();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    static const char* modesStr() {
        // Build a null-separated combo string once.
        static std::string s;
        if (s.empty()) {
            for (int i = 0; i < mfsk::MFSK_MODE_COUNT; i++) {
                s += mfsk::MFSK_MODES[i].name;
                s += '\0';
            }
            s += '\0';
        }
        return s.c_str();
    }

    void saveConfig() {
        config.acquire();
        config.conf[name]["vfoMode"]        = vfoMode;
        config.conf[name]["modeIdx"]        = modeIdx;
        config.conf[name]["snapId"]         = snapId;
        config.conf[name]["afFreq"]         = afFreq;
        config.conf[name]["squelchEnabled"] = squelchEnabled;
        config.conf[name]["afcEnabled"]     = afcEnabled;
        config.conf[name]["squelchLevel"]   = squelchLevel;
        config.release(true);
    }

    std::string name;
    bool enabled = true;

    VFOManager::VFO*    vfo     = nullptr;
    mfsk::MFSKDecoder*  decoder = nullptr;

    // GUI widgets
    ImGui::ConstellationDiagram   diag;
    OptionList<int, double>       snapIntervals;

    // Settings
    int    vfoMode        = mfsk::VFO_MODE_USB;
    int    modeIdx        = 3;       // MFSK16 (index in MFSK_MODES)
    int    snapId         = 0;
    float  afFreq         = 1500.0f;
    bool   squelchEnabled = true;
    bool   afcEnabled     = true;
    float  squelchLevel   = -50.0f;

    // Decoded text
    std::mutex  textMtx;
    std::string decodedText;
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    json def = json({});
    config.setPath(root + "/mfsk_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new MFSKDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (MFSKDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
