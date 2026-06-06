#pragma once

#include <functional>

/* Dream headers */
#include "sound/soundinterface.h"
#include "util/Vector.h"

/*
 * DRMSoundOut captures the decoded PCM produced by the Dream receiver and
 * forwards it to the SDR++ sink manager. Dream writes interleaved stereo
 * 16-bit samples at the current audio sample rate (typically 48 kHz for
 * AAC/xHE-AAC, sometimes 24 kHz); we convert to float and push into the
 * module's stereo stream.
 *
 * The two callbacks are owned by the module:
 *   - onSamples(short* interleavedStereo, int frames)
 *   - onSampleRate(int newRate)   -> lets the module retune the sink stream
 */
class DRMSoundOut : public CSoundOutInterface {
public:
    using SamplesCb    = std::function<void(const short*, int)>;
    using SampleRateCb = std::function<void(int)>;

    DRMSoundOut(SamplesCb samplesCb, SampleRateCb sampleRateCb)
        : cbSamples(std::move(samplesCb)), cbSampleRate(std::move(sampleRateCb)) {}

    virtual ~DRMSoundOut() {}

    bool Init(int iNewSampleRate, int iNewBufferSize, bool bNewBlocking) override {
        iBufferSize = iNewBufferSize;
        bBlocking   = bNewBlocking;
        if (iNewSampleRate != iSampleRate && iNewSampleRate > 0) {
            iSampleRate = iNewSampleRate;
            if (cbSampleRate) cbSampleRate(iSampleRate);
        }
        return true;
    }

    /* psData holds interleaved stereo shorts: [L0,R0,L1,R1,...]. */
    bool Write(CVector<short>& psData) override {
        const int total = psData.Size();
        if (total <= 0) return false;
        const int frames = total / 2;
        if (frames > 0 && cbSamples) {
            cbSamples(&psData[0], frames);
        }
        return false; /* false == good write, matching Dream's convention */
    }

    void Close() override {}

    std::string GetVersion() override { return "SDR++ sink bridge"; }
    CSoundOutInterface* GetItem() override { return this; }

    void Enumerate(std::vector<std::string>& names,
                   std::vector<std::string>& descriptions,
                   std::string& defaultDevice) override {
        names.clear();
        descriptions.clear();
        names.push_back("sdrpp-sink");
        descriptions.push_back("SDR++ audio sink");
        defaultDevice = "sdrpp-sink";
    }
    std::string GetItemName() override { return "sdrpp-sink"; }
    void SetItem(std::string) override {}

    int GetSampleRate() const { return iSampleRate; }

private:
    SamplesCb    cbSamples;
    SampleRateCb cbSampleRate;
    int  iSampleRate = 48000;
    int  iBufferSize = 0;
    bool bBlocking   = true;
};
