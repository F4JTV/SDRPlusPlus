#pragma once
#include <dsp/processor.h>
#include <dsp/types.h>
#include <cmath>

// Converts the complex baseband stream into a magnitude (envelope) stream.
// ADS-B / Mode S is pulse-position modulated, so only the signal envelope
// matters. Output is linear amplitude sqrt(re^2 + im^2).
class MagnitudeDSP : public dsp::Processor<dsp::complex_t, float> {
    using base_type = dsp::Processor<dsp::complex_t, float>;
public:
    MagnitudeDSP() {}
    MagnitudeDSP(dsp::stream<dsp::complex_t>* in) { init(in); }

    int process(int count, const dsp::complex_t* in, float* out) {
        for (int i = 0; i < count; i++) {
            out[i] = sqrtf(in[i].re * in[i].re + in[i].im * in[i].im);
        }
        return count;
    }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }

        process(count, base_type::_in->readBuf, base_type::out.writeBuf);

        base_type::_in->flush();
        if (!base_type::out.swap(count)) { return -1; }
        return count;
    }
};
