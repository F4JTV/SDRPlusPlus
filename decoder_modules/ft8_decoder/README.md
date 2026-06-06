# FT8 / FT4 / WSPR decoder for SDR++

A decoder module for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)
that receives and decodes the three most common weak-signal amateur digital
modes:

- **FT8** — 15-second slots, the de-facto standard for HF DXing.
- **FT4** — 7.5-second slots, a faster contest-oriented variant.
- **WSPR** — 2-minute slots, the Weak Signal Propagation Reporter beacon mode.

Decoded traffic is shown in a **detached, non-modal window**. The window is an
ordinary floating panel: dragging it never dims the SDR++ interface and never
moves the VFO. The VFO snap interval is also set explicitly so tuning is not
pulled onto a coarse grid.

## Features

- FT8, FT4 and WSPR in a single module, switchable at runtime.
- Single USB audio chain at 12 kHz feeding the appropriate decoder.
- Automatic UTC slot alignment from the system clock (15 s / 7.5 s / 120 s).
- Background decode on a worker thread; the DSP path stays light.
- Detached results window with sortable, scrolling table:
  - **FT8/FT4**: UTC, dB, DT, audio frequency (Hz), message.
  - **WSPR**: UTC, dB, DT, on-air frequency (MHz), drift (Hz), message.
- Auto-scroll, one-click clear, and TSV snapshot export.
- Optional real-time logging to a TSV file (inline path field — no modal
  dialog, so it can never disturb the VFO).
- Configurable VFO snap interval: 1 Hz, 10 Hz, 100 Hz, 1 kHz, 2.5 kHz.
- Multiple instances supported.
- Per-instance persistent settings (mode, snap interval, logging).

## Time synchronisation

All three modes are slot-based and rely on the PC clock being accurate. **Keep
the system clock synchronised with NTP** (within ~1 second). Without it the
decoder will capture across slot boundaries and decode rates drop sharply. The
*DT* column shows the measured time offset of each decode and is a good health
check: well-synced stations cluster near 0.

## Building

### Ubuntu 24.04 / 24.10

Install the SDR++ build dependencies:

```bash
sudo apt install --no-install-recommends \
    build-essential cmake pkg-config \
    libfftw3-dev libglfw3-dev libvolk-dev libzstd-dev
```

Clone SDR++ and drop this module into `decoder_modules/`:

```bash
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus
cp -r /path/to/ft8_decoder decoder_modules/

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_FT8_DECODER=ON
make -j$(nproc)
sudo make install
```

The compiled module is at `build/decoder_modules/ft8_decoder/ft8_decoder.so`.

### Patching the SDR++ root `CMakeLists.txt`

Two lines must be added to the SDR++ root `CMakeLists.txt`.

1. In the options section (near the other `OPT_BUILD_*_DECODER` options):

```cmake
option(OPT_BUILD_FT8_DECODER "Build the FT8/FT4/WSPR decoder module (Dependencies: fftw3)" OFF)
```

2. In the `add_subdirectory` section (near the other decoder modules):

```cmake
if (OPT_BUILD_FT8_DECODER)
add_subdirectory("decoder_modules/ft8_decoder")
endif (OPT_BUILD_FT8_DECODER)
```

### Windows 11

Prerequisites (see the SDR++ README for the authoritative list):

- [CMake](https://cmake.org) (>= 3.13)
- [vcpkg](https://vcpkg.io) with `fftw3:x64-windows`, `glfw3:x64-windows`,
  `zstd:x64-windows`
- [PothosSDR](https://github.com/pothosware/PothosSDR) installed in
  `C:\Program Files\PothosSDR` (used by SDR++ for VOLK)
- Visual Studio 2019 or later with the C++ Desktop workload

From a Developer PowerShell:

```powershell
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus
Copy-Item -Recurse path\to\ft8_decoder decoder_modules\

mkdir build
cd build
cmake .. "-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake" `
         -DOPT_BUILD_FT8_DECODER=ON `
         -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## Usage

1. In SDR++, open the **Module Manager**, create an instance of
   `ft8_decoder`, give it a name, and click **+**.
2. Tune the radio so the band's dial frequency lines up with the **lower edge**
   of the module's VFO (USB convention). Typical dial frequencies:
   - FT8: 14.074, 7.074, 10.136, 3.573 MHz, etc.
   - FT4: 14.080, 7.0475, 10.140 MHz, etc.
   - WSPR: 14.0956, 7.0386, 10.1387 MHz, etc. (USB dial).
3. In the module menu, pick the **Mode** (FT8 / FT4 / WSPR).
4. Click **Show Decodes** to open the detached results window.
5. (Optional) Tick **Log to file** and enter a full path to a `.tsv` file.

The window can be dragged, resized and docked freely without affecting the
VFO or the rest of the interface.

## Notes on accuracy

- The **FT8/FT4** path is built on [ft8_lib](https://github.com/kgoba/ft8_lib)
  and has been validated end-to-end against generated reference signals
  (correct frequency, time offset and message text). The reported SNR is an
  approximation derived from the sync score and is not identical to WSJT-X's
  SNR scale.
- The **WSPR** path uses the proven K9AN/K1JT `wsprd` decode core. Its file
  I/O front-end was replaced with an in-memory interface, and a 12 kHz → 375 Hz
  complex down-conversion stage was added ahead of it. This integration is
  best validated on-air against a known-good reference such as WSJT-X or
  WSPR-X. Absolute frequencies assume the VFO lower edge equals the USB dial
  frequency.

## Credits

- **ft8_lib** — FT8/FT4 protocol implementation by Karlis Goba (YL3JG):
  <https://github.com/kgoba/ft8_lib>
- **wsprd** — WSPR decoder core by Steven Franke (K9AN) and Joe Taylor (K1JT),
  part of the WSJT-X project.
- **kissfft** — Mark Borgerding's FFT library (bundled with ft8_lib).
- **SDR++** — the host SDR application by Alexandre Rouma.

## License

GPL-3.0, consistent with SDR++, ft8_lib and wsprd. See [LICENSE](LICENSE).
