#pragma once

#include <atomic>
#include <thread>
#include <chrono>

#include "spsc_ring.h"

/* Dream headers */
#include "sound/soundinterface.h"
#include "util/Vector.h"

/*
 * DRMSoundIn bridges the SDR++ VFO (producer) to the Dream receiver
 * (consumer). Dream pulls baseband I/Q through CSoundInInterface::Read; we
 * hand it interleaved 16-bit samples taken from the lock-free ring that the
 * DSP thread keeps filling.
 *
 * The receiver is configured with SetInChanSel(CS_IQ_POS), so Read() must
 * return interleaved stereo where the left channel carries I and the right
 * channel carries Q.
 */
class DRMSoundIn : public CSoundInInterface {
public:
    DRMSoundIn(SpscRing<short>* ring, std::atomic<bool>* running)
        : pRing(ring), pRunning(running) {}

    virtual ~DRMSoundIn() {}

    bool Init(int iNewSampleRate, int iNewBufferSize, bool bNewBlocking) override {
        iSampleRate = iNewSampleRate;
        iBufferSize = iNewBufferSize;
        bBlocking   = bNewBlocking;
        return true;
    }

    /*
     * Returns the "bad" flag expected by Dream: false means a good read,
     * true means the input failed / no data. psData is pre-sized by Dream to
     * the number of shorts it wants (2 * output block for I/Q input).
     */
    bool Read(CVector<short>& psData, CParameter& /*Parameters*/) override {
        const int want = psData.Size();
        if (want <= 0) return false;

        int got = 0;
        bool starvedThisCall = false;
        /* Block until enough samples are available or the module stops. */
        while (got < want) {
            if (!pRunning->load(std::memory_order_acquire)) {
                /* Pad the remainder with silence and report a bad read so the
                   receiver does not lock onto garbage while shutting down. */
                for (int i = got; i < want; i++) psData[i] = 0;
                bExhausted = true;
                return true;
            }
            size_t n = pRing->read(&psData[got], (size_t)(want - got));
            got += (int)n;
            if (got < want) {
                /* The ring is momentarily empty. With a real-time source this
                   is normal once per block, so we only flag ONE underrun per
                   Read() call (not per retry) to keep the diagnostic meaningful
                   - a steadily climbing count then really does mean the input
                   is being starved. Short 500 us poll keeps added latency low. */
                starvedThisCall = true;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        if (starvedThisCall) {
            nUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    void Close() override {}

    std::string GetVersion() override { return "SDR++ VFO I/Q bridge"; }
    CSoundInInterface* GetItem() override { return this; }

    /* CSelectionInterface (we expose a single virtual device) */
    void Enumerate(std::vector<std::string>& names,
                   std::vector<std::string>& descriptions,
                   std::string& defaultDevice) override {
        names.clear();
        descriptions.clear();
        names.push_back("sdrpp-vfo");
        descriptions.push_back("SDR++ VFO baseband I/Q");
        defaultDevice = "sdrpp-vfo";
    }
    std::string GetItemName() override { return "sdrpp-vfo"; }
    void SetItem(std::string) override {}

    /* True once a read could not be fully satisfied because the producer
       stopped (used by the offline test harness; harmless in normal use). */
    bool exhausted() const { return bExhausted; }

    /* Number of times Read() found the ring starved. A steadily climbing value
       at runtime means the I/Q stream has gaps - fatal for OFDM lock. */
    unsigned long underruns() const { return nUnderruns.load(std::memory_order_relaxed); }

private:
    SpscRing<short>*    pRing;
    std::atomic<bool>*  pRunning;
    int  iSampleRate = 48000;
    int  iBufferSize = 0;
    bool bBlocking   = true;
    bool bExhausted  = false;
    std::atomic<unsigned long> nUnderruns{0};
};
