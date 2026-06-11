#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <filesystem>
#include "meteor_demod.h"
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <meteor_demodulator_interface.h>
#include <gui/widgets/folder_select.h>
#include <gui/widgets/constellation_diagram.h>
#include <utils/opengl_include_code.h>

#include <fstream>
#include <thread>
#include <atomic>
#include <vector>

#include "lrpt/lrpt_decoder.h"
#include "lrpt/stb_image_write.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "meteor_demodulator",
    /* Description:     */ "Meteor demodulator + LRPT image decoder for SDR++",
    /* Author:          */ "Ryzerth;F4JTV (LRPT decode, ported from SatDump)",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

ConfigManager config;

std::string genFileName(std::string prefix, std::string suffix) {
    time_t now = time(0);
    tm* ltm = localtime(&now);
    char buf[1024];
    sprintf(buf, "%s_%02d-%02d-%02d_%02d-%02d-%02d%s", prefix.c_str(), ltm->tm_hour, ltm->tm_min, ltm->tm_sec, ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900, suffix.c_str());
    return buf;
}

#define INPUT_SAMPLE_RATE 150000

// A growable RGBA OpenGL texture, lazily created in the render thread (where the
// GL context is active) and recreated when the image dimensions change. This is
// the directly-managed-texture pattern (avoids ImGui::ImageDisplay's fixed size).
class GrowableTexture {
public:
    ~GrowableTexture() {
        if (textureId) glDeleteTextures(1, &textureId);
    }

    // Upload an RGBA8 buffer (width*height*4). Call from the render thread.
    void update(const uint8_t* rgba, int width, int height) {
        if (!rgba || width <= 0 || height <= 0) return;
        if (!created) {
            glGenTextures(1, &textureId);
            created = true;
        }
        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        w = width; h = height;
        valid = true;
    }

    bool isValid() const { return valid; }
    GLuint id() const { return textureId; }
    int width() const { return w; }
    int height() const { return h; }

private:
    GLuint textureId = 0;
    bool created = false, valid = false;
    int w = 0, h = 0;
};

class MeteorDemodulatorModule : public ModuleManager::Instance {
public:
    MeteorDemodulatorModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;

        writeBuffer = new int8_t[STREAM_BUFFER_SIZE];

        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name] = json({});
        }
        if (config.conf[name].contains("recPath")) {
            folderSelect.setPath(config.conf[name]["recPath"]);
        }
        if (config.conf[name].contains("brokenModulation")) {
            brokenModulation = config.conf[name]["brokenModulation"];
        }
        if (config.conf[name].contains("oqpsk")) {
            oqpsk = config.conf[name]["oqpsk"];
        }
        if (config.conf[name].contains("lrptDiff")) {
            lrptDiff = config.conf[name]["lrptDiff"];
        }
        if (config.conf[name].contains("lrptModeM2x")) {
            lrptModeM2x = config.conf[name]["lrptModeM2x"];
        }
        config.release();

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, true);
        demod.init(vfo->output, 72000.0f, INPUT_SAMPLE_RATE, 33, 0.6f, 0.1f, 0.005f, brokenModulation, oqpsk, 1e-6, 0.01);
        split.init(&demod.out);
        split.bindStream(&symSinkStream);
        split.bindStream(&sinkStream);
        reshape.init(&symSinkStream, 1024, (72000 / 30) - 1024);
        symSink.init(&reshape.out, symSinkHandler, this);
        sink.init(&sinkStream, sinkHandler, this);

        decoder = std::make_unique<LRPTDecoder>(lrptModeM2x ? LRPTDecoder::MODE_M2X : LRPTDecoder::MODE_LEGACY, lrptDiff);

        demod.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();

        gui::menu.registerEntry(name, menuHandler, this, this);
        core::modComManager.registerInterface("meteor_demodulator", name, moduleInterfaceHandler, this);
    }

    ~MeteorDemodulatorModule() {
        if (recording) {
            std::lock_guard<std::mutex> lck(recMtx);
            recording = false;
            recFile.close();
        }
        stopFileDecode();
        demod.stop();
        split.stop();
        reshape.stop();
        symSink.stop();
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        gui::menu.removeEntry(name);
        delete[] writeBuffer;
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0), 150000, INPUT_SAMPLE_RATE, 150000, 150000, true);

        demod.setBrokenModulation(brokenModulation);
        demod.setInput(vfo->output);

        demod.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();

        enabled = true;
    }

    void disable() {
        demod.stop();
        split.stop();
        reshape.stop();
        symSink.stop();
        sink.stop();

        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        _this->constDiagram.draw();

        if (_this->folderSelect.render("##meteor_rec" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }

        if (ImGui::Checkbox(CONCAT("Broken modulation##meteor_rec", _this->name), &_this->brokenModulation)) {
            _this->demod.setBrokenModulation(_this->brokenModulation);
            config.acquire();
            config.conf[_this->name]["brokenModulation"] = _this->brokenModulation;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("OQPSK##oqpsk", _this->name), &_this->oqpsk)) {
            _this->demod.setOQPSK(_this->oqpsk);
            config.acquire();
            config.conf[_this->name]["oqpsk"] = _this->oqpsk;
            config.release(true);
        }

        if (!_this->folderSelect.pathIsValid() && _this->enabled) { style::beginDisabled(); }

        if (_this->recording) {
            if (ImGui::Button(CONCAT("Stop##meteor_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stopRecording();
            }
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Recording %.2fMB", (float)_this->dataWritten / 1000000.0f);
        }
        else {
            if (ImGui::Button(CONCAT("Record##meteor_rec_", _this->name), ImVec2(menuWidth, 0))) {
                _this->startRecording();
            }
            ImGui::TextUnformatted("Idle --.--MB");
        }

        if (!_this->folderSelect.pathIsValid() && _this->enabled) { style::endDisabled(); }

        if (!_this->enabled) { style::endDisabled(); }

        // ---------------- LRPT image decoder ----------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("LRPT image decoder");
        ImGui::Spacing();

        if (ImGui::Checkbox(CONCAT("Decode LRPT (live)##lrpt_live", _this->name), &_this->liveDecode)) {
            // Nothing else needed; sinkHandler checks the flag.
        }

        // Satellite / coding mode
        ImGui::TextUnformatted("Satellite:");
        ImGui::SetNextItemWidth(menuWidth);
        int modeIdx = _this->lrptModeM2x ? 0 : 1;
        const char* modeItems = "Meteor-M2-3 / M2-4 (M2-x)\0Meteor-M2 (ancien, 72k QPSK)\0";
        if (ImGui::Combo(CONCAT("##lrpt_mode", _this->name), &modeIdx, modeItems)) {
            _this->lrptModeM2x = (modeIdx == 0);
            if (_this->decoder)
                _this->decoder->setMode(_this->lrptModeM2x ? LRPTDecoder::MODE_M2X : LRPTDecoder::MODE_LEGACY);
            config.acquire();
            config.conf[_this->name]["lrptModeM2x"] = _this->lrptModeM2x;
            // M2-3 / M2-4 transmit OQPSK: enable it automatically on the demod.
            if (_this->lrptModeM2x && !_this->oqpsk) {
                _this->oqpsk = true;
                _this->demod.setOQPSK(true);
                config.conf[_this->name]["oqpsk"] = true;
            }
            config.release(true);
        }
        if (_this->lrptModeM2x)
            ImGui::TextDisabled("M2-3 = 137.9 MHz, M2-4 = 137.1 MHz (cocher OQPSK)");

        if (ImGui::Checkbox(CONCAT("Differential decode##lrpt_diff", _this->name), &_this->lrptDiff)) {
            if (_this->decoder) _this->decoder->setDiffDecode(_this->lrptDiff);
            config.acquire();
            config.conf[_this->name]["lrptDiff"] = _this->lrptDiff;
            config.release(true);
        }

        // Status line
        {
            bool locked = _this->decoder ? _this->decoder->isLocked() : false;
            float ber = _this->decoder ? _this->decoder->getBER() : 0.0f;
            uint64_t cadus = _this->decoder ? _this->decoder->getCADUs() : 0;
            uint64_t pkts = _this->decoder ? _this->decoder->getPackets() : 0;

            ImGui::TextUnformatted("Sync:");
            ImGui::SameLine();
            if (locked) ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "LOCKED");
            else ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "no sync");
            ImGui::SameLine();
            ImGui::Text("| BER %.3f", ber);
            ImGui::Text("CADUs: %llu   MSU-MR packets: %llu",
                        (unsigned long long)cadus, (unsigned long long)pkts);
        }

        if (_this->fileDecoding) {
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Decoding file... %d%%", (int)(_this->fileProgress * 100.0f));
        }

        // View selector
        ImGui::SetNextItemWidth(menuWidth);
        const char* viewItems = "Composite RGB\0Channel 1 (APID 64)\0Channel 2 (APID 65)\0Channel 3 (APID 66)\0Channel 4 (APID 67)\0Channel 5 (APID 68)\0Channel 6 (APID 69)\0";
        ImGui::Combo(CONCAT("##lrpt_view", _this->name), &_this->viewMode, viewItems);

        if (_this->viewMode == 0) {
            ImGui::Text("R/G/B channels:");
            ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
            ImGui::InputInt(CONCAT("##lrpt_r", _this->name), &_this->compR, 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
            ImGui::InputInt(CONCAT("##lrpt_g", _this->name), &_this->compG, 0, 0);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth / 3.0f - 4);
            ImGui::InputInt(CONCAT("##lrpt_b", _this->name), &_this->compB, 0, 0);
            _this->compR = std::clamp(_this->compR, 0, 5);
            _this->compG = std::clamp(_this->compG, 0, 5);
            _this->compB = std::clamp(_this->compB, 0, 5);
        }

        if (ImGui::Button(CONCAT("Refresh image##lrpt_refresh", _this->name), ImVec2(menuWidth, 0))) {
            _this->rebuildImage();
        }

        // Auto-refresh roughly every 2 s while live decoding
        if (_this->liveDecode) {
            double now = ImGui::GetTime();
            if (now - _this->lastRefresh > 2.0) {
                _this->lastRefresh = now;
                _this->rebuildImage();
            }
        }

        // Image display
        if (!_this->displayRGBA.empty() && _this->dispW > 0 && _this->dispH > 0) {
            if (_this->texDirty) {
                _this->texture.update(_this->displayRGBA.data(), _this->dispW, _this->dispH);
                _this->texDirty = false;
            }
            if (_this->texture.isValid()) {
                float aspect = (float)_this->dispH / (float)_this->dispW;
                float dw = menuWidth;
                float dh = dw * aspect;
                ImGui::Image((ImTextureID)(intptr_t)_this->texture.id(), ImVec2(dw, dh));
                ImGui::Text("Image: %d x %d", _this->dispW, _this->dispH);
            }
        }
        else {
            ImGui::TextDisabled("No image yet");
        }

        if (ImGui::Button(CONCAT("Save PNG##lrpt_save", _this->name), ImVec2(menuWidth, 0))) {
            _this->savePNG();
        }

        if (ImGui::Button(CONCAT("Reset decoder##lrpt_reset", _this->name), ImVec2(menuWidth, 0))) {
            if (_this->decoder) _this->decoder->reset();
            _this->displayRGBA.clear();
            _this->dispW = _this->dispH = 0;
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Offline decode (.s soft symbols):");
        ImGui::SetNextItemWidth(menuWidth);
        ImGui::InputText(CONCAT("##lrpt_file", _this->name), _this->filePath, sizeof(_this->filePath));
        if (_this->fileDecoding) { style::beginDisabled(); }
        if (ImGui::Button(CONCAT("Decode file##lrpt_decfile", _this->name), ImVec2(menuWidth, 0))) {
            _this->startFileDecode(_this->filePath);
        }
        if (_this->fileDecoding) { style::endDisabled(); }
    }

    static void symSinkHandler(dsp::complex_t* data, int count, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        dsp::complex_t* buf = _this->constDiagram.acquireBuffer();
        memcpy(buf, data, 1024 * sizeof(dsp::complex_t));
        _this->constDiagram.releaseBuffer();
    }

    static void sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;

        // Convert soft QPSK symbols to int8 I/Q (same scaling as the .s recorder)
        if (_this->recording || _this->liveDecode) {
            for (int i = 0; i < count; i++) {
                _this->writeBuffer[(2 * i)] = std::clamp<int>(data[i].re * 84.0f, -127, 127);
                _this->writeBuffer[(2 * i) + 1] = std::clamp<int>(data[i].im * 84.0f, -127, 127);
            }
        }

        if (_this->liveDecode && _this->decoder) {
            _this->decoder->pushSymbols(_this->writeBuffer, count * 2);
        }

        {
            std::lock_guard<std::mutex> lck(_this->recMtx);
            if (!_this->recording) { return; }
            _this->recFile.write((char*)_this->writeBuffer, count * 2);
            _this->dataWritten += count * 2;
        }
    }

    void rebuildImage() {
        if (!decoder) return;
        if (viewMode == 0) {
            std::vector<uint8_t> rgb;
            int w = 0, h = 0;
            if (decoder->getComposite(compR, compG, compB, rgb, w, h)) {
                rgbaFromRGB(rgb, w, h);
            }
        }
        else {
            int ch = viewMode - 1;
            meteorimg::SimpleImage img = decoder->getChannelImage(ch);
            if (img.size() > 0) {
                rgbaFromGray(img);
            }
        }
    }

    void rgbaFromGray(meteorimg::SimpleImage& img) {
        int w = (int)img.width(), h = (int)img.height();
        if (w <= 0 || h <= 0) return;
        displayRGBA.assign((size_t)w * h * 4, 255);
        const uint8_t* src = img.data();
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t v = src[i];
            displayRGBA[i * 4 + 0] = v;
            displayRGBA[i * 4 + 1] = v;
            displayRGBA[i * 4 + 2] = v;
            displayRGBA[i * 4 + 3] = 255;
        }
        dispW = w; dispH = h; texDirty = true;
    }

    void rgbaFromRGB(std::vector<uint8_t>& rgb, int w, int h) {
        if (w <= 0 || h <= 0) return;
        displayRGBA.assign((size_t)w * h * 4, 255);
        for (size_t i = 0; i < (size_t)w * h; i++) {
            displayRGBA[i * 4 + 0] = rgb[i * 3 + 0];
            displayRGBA[i * 4 + 1] = rgb[i * 3 + 1];
            displayRGBA[i * 4 + 2] = rgb[i * 3 + 2];
            displayRGBA[i * 4 + 3] = 255;
        }
        dispW = w; dispH = h; texDirty = true;
    }

    void savePNG() {
        rebuildImage();
        if (displayRGBA.empty() || dispW <= 0 || dispH <= 0) {
            flog::warn("LRPT: nothing to save yet");
            return;
        }
        std::string base = folderSelect.pathIsValid() ? folderSelect.expandString(folderSelect.path) : ((std::string)core::args["root"] + "/recordings");
        std::string suffix = (viewMode == 0) ? "_LRPT_RGB" : ("_LRPT_ch" + std::to_string(viewMode));
        std::string filename = genFileName(base + "/meteor", suffix + ".png");
        // Write RGB (drop alpha) for a compact image
        std::vector<uint8_t> rgb((size_t)dispW * dispH * 3);
        for (size_t i = 0; i < (size_t)dispW * dispH; i++) {
            rgb[i * 3 + 0] = displayRGBA[i * 4 + 0];
            rgb[i * 3 + 1] = displayRGBA[i * 4 + 1];
            rgb[i * 3 + 2] = displayRGBA[i * 4 + 2];
        }
        if (stbi_write_png(filename.c_str(), dispW, dispH, 3, rgb.data(), dispW * 3)) {
            flog::info("LRPT: saved '{0}'", filename);
        }
        else {
            flog::error("LRPT: failed to save PNG");
        }
    }

    void startFileDecode(const std::string& path) {
        if (fileDecoding) return;
        if (path.empty() || !std::filesystem::exists(path)) {
            flog::error("LRPT: file does not exist: '{0}'", path);
            return;
        }
        stopFileDecode();
        if (decoder) decoder->reset();
        fileProgress = 0.0f;
        fileDecoding = true;
        fileDecodeStop = false;
        fileThread = std::thread(&MeteorDemodulatorModule::fileDecodeWorker, this, path);
    }

    void stopFileDecode() {
        fileDecodeStop = true;
        if (fileThread.joinable()) fileThread.join();
        fileDecoding = false;
    }

    void fileDecodeWorker(std::string path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) { fileDecoding = false; return; }
        std::streamsize total = f.tellg();
        f.seekg(0, std::ios::beg);
        const size_t CHUNK = 65536; // int8 samples per read
        std::vector<int8_t> buf(CHUNK);
        std::streamsize done = 0;
        while (!fileDecodeStop && f.good()) {
            f.read((char*)buf.data(), CHUNK);
            std::streamsize got = f.gcount();
            if (got <= 0) break;
            if (decoder) decoder->pushSymbols(buf.data(), (int)got);
            done += got;
            fileProgress = total > 0 ? (float)((double)done / (double)total) : 0.0f;
        }
        fileProgress = 1.0f;
        // Build the final image (on the worker; texture upload happens in render thread)
        rebuildImage();
        fileDecoding = false;
    }

    void startRecording() {
        std::lock_guard<std::mutex> lck(recMtx);
        dataWritten = 0;
        std::string filename = genFileName(folderSelect.expandString(folderSelect.path) + "/meteor", ".s");
        recFile = std::ofstream(filename, std::ios::binary);
        if (recFile.is_open()) {
            flog::info("Recording to '{0}'", filename);
            recording = true;
        }
        else {
            flog::error("Could not open file for recording!");
        }
    }

    void stopRecording() {
        std::lock_guard<std::mutex> lck(recMtx);
        recording = false;
        recFile.close();
        dataWritten = 0;
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        MeteorDemodulatorModule* _this = (MeteorDemodulatorModule*)ctx;
        if (code == METEOR_DEMODULATOR_IFACE_CMD_START) {
            if (!_this->recording) { _this->startRecording(); }
        }
        else if (code == METEOR_DEMODULATOR_IFACE_CMD_STOP) {
            if (_this->recording) { _this->stopRecording(); }
        }
    }

    std::string name;
    bool enabled = true;

    // DSP Chain
    VFOManager::VFO* vfo;
    dsp::demod::Meteor demod;
    dsp::routing::Splitter<dsp::complex_t> split;

    dsp::stream<dsp::complex_t> symSinkStream;
    dsp::stream<dsp::complex_t> sinkStream;
    dsp::buffer::Reshaper<dsp::complex_t> reshape;
    dsp::sink::Handler<dsp::complex_t> symSink;
    dsp::sink::Handler<dsp::complex_t> sink;

    ImGui::ConstellationDiagram constDiagram;

    FolderSelect folderSelect;

    std::mutex recMtx;
    bool recording = false;
    uint64_t dataWritten = 0;
    std::ofstream recFile;
    bool brokenModulation = false;
    bool oqpsk = false;
    int8_t* writeBuffer;

    // LRPT decode
    std::unique_ptr<LRPTDecoder> decoder;
    bool liveDecode = false;
    bool lrptDiff = true;
    bool lrptModeM2x = true;

    int viewMode = 0; // 0 = composite, 1..6 = channels
    int compR = 0, compG = 1, compB = 2;

    std::vector<uint8_t> displayRGBA;
    int dispW = 0, dispH = 0;
    bool texDirty = false;
    GrowableTexture texture;
    double lastRefresh = 0.0;

    char filePath[1024] = "";
    std::thread fileThread;
    std::atomic<bool> fileDecoding{false};
    std::atomic<bool> fileDecodeStop{false};
    std::atomic<float> fileProgress{0.0f};
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        flog::warn("Recordings directory does not exist, creating it");
        if (!std::filesystem::create_directory(root + "/recordings")) {
            flog::error("Could not create recordings directory");
        }
    }
    json def = json({});
    config.setPath(root + "/meteor_demodulator_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new MeteorDemodulatorModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (MeteorDemodulatorModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
