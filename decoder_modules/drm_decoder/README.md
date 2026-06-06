# SDR++ DRM Decoder

A [Digital Radio Mondiale](https://www.drm.org/) (DRM30) decoder module for
[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus).

The module captures baseband I/Q from an SDR++ VFO, feeds it to the
[Dream](https://github.com/rafael2k/dream) DRM receiver core (GPL-2) through a
lock-free ring buffer, and routes the decoded audio back into the SDR++ sink
manager. Dream handles the complete DRM chain — OFDM synchronisation, channel
estimation, MLC/Viterbi decoding, energy dispersal, FAC/SDC parsing, MSC
de-multiplexing and AAC / xHE-AAC / Opus audio decoding — and the module
exposes it through a clean SDR++ UI with a tunable VFO, service list, audio
routing and live signal-quality indicators.

## Features

- Selectable channel bandwidth (4.5 / 5 / 9 / 10 / 18 / 20 kHz)
- Real-time acquisition indicator (**SIGNAL LOCKED**) and SNR meter
- Robustness mode display (A / B / C / D / E)
- Spectrum occupancy readout
- Service list with codec (AAC / xHE-AAC / Opus / MPEG-AAC); click to select an
  audio service
- Audio routed through the standard SDR++ sink (soundcard, network sink, …)
- Station label and DRM text message rendering
- Decoded audio sample-rate readout

## Project layout

```
drm_decoder/
├── CMakeLists.txt        # Builds the module + the vendored Dream receiver core
├── README.md
├── dream/                # ← to be cloned (Dream DRM receiver, GPL-2)
│   └── src/              # full DRM decoder (compiled without the Qt GUI)
└── src/
    ├── main.cpp          # SDR++ module: VFO, UI, audio routing, receiver thread
    ├── spsc_ring.h       # lock-free single-producer/single-consumer ring buffer
    ├── drm_input.h       # CSoundInInterface bridge: VFO I/Q → Dream
    └── drm_output.h      # CSoundOutInterface bridge: Dream audio → SDR++ sink
```

## Dependencies

| Component    | Role                                                  |
|--------------|-------------------------------------------------------|
| `libfaad2`   | classic AAC / HE-AAC DRM audio decoder (`NeAACDecInitDRM`) |
| `libfdk-aac` | xHE-AAC (USAC) audio decoder, used by modern DRM stations |
| `libopus`    | Opus audio decoder                                    |
| `libfftw3`   | FFT for OFDM demodulation                             |
| `libsndfile` | audio file I/O used by the Dream core                 |
| `libspeexdsp`| resampling used by the Dream core                     |
| `zlib`       | data services / Journaline                            |
| `libpcap`    | RSCI/MDI support in the Dream core                    |

> **faad2 / DRM note (Debian/Ubuntu):** the `libfaad-dev` package installs two
> runtime libraries — `libfaad.so` (plain AAC) and `libfaad_drm.so`. Only the
> latter exports `NeAACDecInitDRM`, which the DRM audio path needs. The build
> links `libfaad_drm` automatically when present (and warns if it has to fall
> back to plain `libfaad`).

> **xHE-AAC note:** modern DRM stations (e.g. the TDF/Issoudun tests) use
> xHE-AAC (USAC), which `libfaad2` cannot decode — the receiver would lock and
> show the station label but stay silent. This module builds against
> `libfdk-aac` (option `OPT_DRM_USE_FDK_AAC`, ON by default) to decode xHE-AAC.
> Classic AAC/HE-AAC is still handled by `libfaad2`, because the stock
> `libfdk-aac` advertises DRM-AAC support but fails on real DRM AAC streams; the
> bundled `dream_patches/fdk_aac_codec.cpp` restricts FDK to xHE-AAC only. The
> menu's **Audio out** indicator shows whether decoded audio is reaching the
> sink: if it climbs but you hear nothing, the decode is fine and the problem is
> audio routing (assign an output device to this stream in the Sinks menu); if
> it stays at 0, the audio codec is the issue.

## Building on Ubuntu 24.04

```bash
# 1. System dependencies
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libfaad-dev libopus-dev libfdk-aac-dev libfftw3-dev libsndfile1-dev \
    libspeexdsp-dev zlib1g-dev libpcap-dev \
    libglfw3-dev libvolk-dev libzstd-dev

# 2. Clone SDR++ if needed
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# 3. Drop in this module
tar xzf /path/to/drm_decoder.tar.gz -C decoder_modules/

# 4. Clone the Dream receiver core INSIDE the module directory
cd decoder_modules/drm_decoder
git clone https://github.com/rafael2k/dream.git
cd ../..

# 5. Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_DRM_DECODER=ON
make -j$(nproc)

# 6. Install
sudo make install
```

If `OPT_BUILD_DRM_DECODER` is not present in SDR++'s root `CMakeLists.txt`, add
it manually. Near the other decoder options:

```cmake
option(OPT_BUILD_DRM_DECODER "Build the DRM (Digital Radio Mondiale) decoder module" OFF)
```

and near the other `add_subdirectory` decoder entries:

```cmake
if (OPT_BUILD_DRM_DECODER)
add_subdirectory("decoder_modules/drm_decoder")
endif (OPT_BUILD_DRM_DECODER)
```

## Building on Windows 11

Prerequisites: Visual Studio 2022 (Desktop C++ workload), CMake ≥ 3.20,
[vcpkg](https://github.com/microsoft/vcpkg), Git for Windows.

```powershell
# 1. Set up vcpkg if needed
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# 2. Install DRM dependencies (plus SDR++ standard ones)
.\vcpkg install faad2:x64-windows libsndfile:x64-windows fftw3:x64-windows `
                opus:x64-windows speexdsp:x64-windows zlib:x64-windows fdk-aac:x64-windows `
                volk:x64-windows glfw3:x64-windows zstd:x64-windows

# 3. Clone SDR++
cd C:\
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# 4. Extract the module and clone the Dream core
tar -xzf C:\Downloads\drm_decoder.tar.gz -C decoder_modules\
cd decoder_modules\drm_decoder
git clone https://github.com/rafael2k/dream.git
cd ..\..

# 5. Configure + build (Developer PowerShell for VS 2022)
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
    -DOPT_BUILD_DRM_DECODER=ON `
    -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --target drm_decoder -- /m
```

The output `drm_decoder.dll` lands in
`build\decoder_modules\drm_decoder\Release\`. Copy it to the `modules\` folder
next to `sdrpp.exe`, together with the runtime DLLs vcpkg places in
`installed\x64-windows\bin\` (`libfaad.dll`, `opus.dll`, `fftw3.dll`,
`fftw3f.dll`, `sndfile.dll`, …).

## Usage

1. Launch SDR++
2. In the **Module Manager**, create an instance of `drm_decoder`
3. Tune the VFO to a DRM signal (DRM30 lives on the long-, medium- and
   short-wave bands)
4. Pick the channel bandwidth matching the broadcast (10 kHz is the most
   common)
5. Wait for acquisition — the **SIGNAL LOCKED** indicator turns green and the
   service list fills in
6. Click an audio service to start playback

## Technical notes

### Why 48 kHz?

A DRM30 channel occupies at most 20 kHz. The module runs the VFO at a 48 kHz
baseband sample rate and feeds Dream complex I/Q (configured with
`CS_IQ_POS`, i.e. I on the left channel and Q on the right). 48 kHz comfortably
covers every DRM30 spectrum-occupancy class with margin for the analysis
filters.

### Threading

```
┌──────────────────┐  push   ┌───────────────┐  pull  ┌──────────────────┐
│ SDR++ DSP thread │ ──────► │ SPSC ring      │ ─────► │ Dream receiver   │
│ (iqHandler)      │         │ (lock-free)    │        │ thread (process) │
└──────────────────┘         └───────────────┘        └────────┬─────────┘
                                                               │ Write()
                                                               ▼
                                                      ┌──────────────────┐
                                                      │ stereo audio     │
                                                      │ stream           │
                                                      └────────┬─────────┘
                                                               ▼
                                                      ┌──────────────────┐
                                                      │ SDR++ SinkManager│
                                                      │ → audio output   │
                                                      └──────────────────┘
```

The lock-free ring keeps the I/Q hot path mutex-free. Because DRM is an OFDM
waveform, the receiver locks by tracking the timing and frequency of the OFDM
symbols, and *any* discontinuity in the sample stream destroys that
synchronisation — a single dropped block and the receiver will not lock again,
no matter how strong the signal is. The ring therefore uses a **lossless
backpressure** policy: if the decoder briefly falls behind, the producer waits
for it to catch up rather than overwriting unread samples, so the stream stays
continuous. The ring holds several seconds of I/Q, so this only ever happens on
a transient overrun.

The menu shows an **I/Q stream** indicator: it reads "continuous" in normal
operation and turns red with an underrun count if the decoder is being starved
of samples — the first thing to check if a strong signal refuses to lock.

### faad2 linkage

By default Dream loads its AAC decoder at runtime via `dlopen` of a separately
built `libfaad_drm`. This module instead compiles the Dream core with
`USE_FAAD2_LIBRARY` and links the system `libfaad2` directly — the stock library
already exports the `NeAACDecInitDRM` entry point required for DRM, which keeps
the build self-contained.

## Licensing

This module is glue code around the Dream receiver core, which is licensed
under the **GNU GPL v2**. The combined work is therefore distributed under the
GPL-2. See the Dream repository for its copyright notices and third-party IP
considerations.
