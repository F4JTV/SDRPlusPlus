# THOR decoder

An SDR++ module that decodes the **THOR** family of digital text modes from the
FLDIGI lineage. THOR is an IFK+ (incremental frequency keying) MFSK mode derived
from DominoEX, hardened for HF with a rate-1/2 convolutional FEC, a diagonal
bit interleaver and the IZ8BLY/THOR varicode. The module decodes all fourteen
standard variants directly from the receiver's I/Q stream.

## Supported modes

| # | Mode | Native rate | Tone spacing | Speed | FEC (K) |
|---|------|-------------|--------------|-------|---------|
| 0 | THOR Micro | 8000 Hz | ~2 Hz | slowest | 7 |
| 1 | THOR 4 | 8000 Hz | ~3.9 Hz | very slow | 7 |
| 2 | THOR 5 | 11025 Hz | ~10.8 Hz | very slow | 7 |
| 3 | THOR 8 | 8000 Hz | ~7.8 Hz | slow | 7 |
| 4 | THOR 11 | 11025 Hz | ~10.8 Hz | slow | 7 |
| 5 | THOR 16 | 8000 Hz | ~15.6 Hz | medium | 7 |
| 6 | THOR 22 | 11025 Hz | ~21.5 Hz | medium | 7 |
| 7 | THOR 32 | 8000 Hz | ~31.3 Hz | fast | 7 |
| 8 | THOR 44 | 11025 Hz | ~43.1 Hz | fast | 7 |
| 9 | THOR 56 | 16000 Hz | ~55.2 Hz | fast | 7 |
| 10 | THOR 25x4 | 8000 Hz | ~25 Hz (x4) | fast | 15 |
| 11 | THOR 50x1 | 8000 Hz | ~50 Hz | fast | 15 |
| 12 | THOR 50x2 | 8000 Hz | ~50 Hz (x2) | fast | 15 |
| 13 | THOR 100 | 8000 Hz | ~100 Hz | fastest | 15 |

The slower modes (Micro/4/5/8/11) are intended for weak-signal and NVIS work; the
faster modes trade robustness for throughput. The `25x4`, `50x1`, `50x2` and
`100` variants use the long-constraint (K=15) convolutional code.

## Usage

1. Tune the SDR so the THOR signal sits inside the VFO passband.
2. Open the **THOR decoder** entry in the module list.
3. Pick the **Mode** that matches the transmission.
4. Choose the **VFO mode**:
   * **USB** — normal HF amateur usage (default).
   * **LSB** — for inverted/lower-sideband setups.
   * **NFM** — for THOR carried over an FM channel (VHF/UHF).
5. Set the **AF freq** to the centre of the signal. The easiest way is to click
   (and drag) directly on the band-view strip: the blue shaded region shows the
   tone band and the yellow line marks the current centre. THOR is differential
   and tolerates a few hundred hertz of mistuning, so exact centring is not
   critical.
6. Decoded text appears in the **Decoded text** panel. Use **Clear** to reset it.

The **Squelch** gate suppresses decoding on noise-only segments; lower the
threshold to open it on weak signals. The small scope shows the dominant-tone
vector and **S/N** gives a rough signal-to-noise estimate.

## Build

The module follows the standard SDR++ out-of-tree decoder layout and is wired
into the top-level build with the `OPT_BUILD_THOR_DECODER` option (ON by
default):

```sh
cd SDRPlusPlus
mkdir -p build && cd build
cmake .. -DOPT_BUILD_THOR_DECODER=ON
make thor_decoder -j$(nproc)
```

The resulting `thor_decoder` plugin is loaded by SDR++ like any other decoder
module.

## Design notes

* **Faithful port.** The demodulator (front-end mixing, sliding-FFT tone bank,
  IFK+ differential decode, FEC, interleaver and varicode) is a direct port of
  the FLDIGI THOR implementation, so on-air behaviour matches FLDIGI.

* **Sample-rate handling.** SDR++ delivers audio at the 48 kHz VFO rate. Each
  mode runs internally at a rate that is an exact integer division of 48 kHz
  (8000 = /6, 12000 = /4 for the 11025-native modes, 16000 = /3). The tone
  spacing and baud are preserved in hertz, so the demodulator is numerically
  equivalent to FLDIGI running at the mode's native rate while keeping the
  sliding-FFT history buffers compact. A 128-tap decimating low-pass FIR
  (≈3.4 kHz cut-off) bridges 48 kHz to the internal rate.

* **No AFC.** Like FLDIGI, THOR relies on its wide tone-capture range and manual
  tuning rather than an automatic frequency control; the differential IFK+
  scheme makes a closed-loop AFC unnecessary (and unreliable, because the tone
  occupancy of an IFK+ stream is not uniform). Tune with the band view instead.

* **Text mode.** THOR is a keyboard/chat mode, so the module outputs decoded
  text only and does not push to the SDR map.

## Validation

The decoder was checked two ways:

1. **Loopback.** A faithful THOR transmitter (also ported from FLDIGI, used only
   for testing) generates each of the fourteen modes; the signal is passed
   through additive white Gaussian noise and decoded back. All fourteen modes
   recover the test message byte-exactly from clean conditions down to about
   −9 dB SNR.

2. **Real off-air recordings.** Publicly available THOR recordings (THOR 5, 11,
   16, 22 and 100) decode the expected text cleanly, confirming correct
   behaviour across both the K=7 and K=15 codes and the 8000/11025-native tone
   plans against real signals rather than synthetic data alone.

The test harness lives under `test/` (`test_thor.cpp` for loopback,
`decode_wav.cpp` for decoding a 48 kHz mono WAV) and builds standalone, without
the SDR++ runtime.
