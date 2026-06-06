# SDR++ Cospas-Sarsat 406 MHz Beacon Decoder

A real-time decoder module for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)
that receives and decodes Cospas-Sarsat 406 MHz emergency distress beacons
(ELT, EPIRB, PLB, SSAS).

Developed by **F4JTV** for **ADRASEC 06**, based on the original `dec406`
decoder by **F4EHY**.

---

## Features

- Real-time decoding of 406 MHz emergency beacons
- Beacon types: **ELT** (aviation), **EPIRB** (maritime),
  **PLB** (personal), **SSAS** (ship security)
- Decoded information: 15-character Hex ID, country (MID code), beacon type,
  protocol, MMSI, GPS position, BCH error-correction status
- Quick frequency buttons for all Cospas-Sarsat channels
- Logging to timestamped file
- OpenStreetMap integration (click to view beacon position)

### Supported frequencies

| Frequency   | Usage             |
|-------------|-------------------|
| 406.025 MHz | Primary channel   |
| 406.028 MHz | Secondary channel |
| 406.037 MHz | Additional        |
| 406.040 MHz | Additional        |

---

## Build & Install (Ubuntu 24.04)

This module is built **in-tree**: its sources are placed inside the SDR++
`decoder_modules/` folder and compiled together with SDR++ using the
standard `OPT_BUILD_*` mechanism.

### 1. Install dependencies

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libfftw3-dev libglfw3-dev libglew-dev libvolk-dev libzstd-dev \
    libsoapysdr-dev libairspy-dev librtlsdr-dev libhackrf-dev \
    libairspyhf-dev libiio-dev libad9361-dev libbladerf-dev \
    libcodec2-dev portaudio19-dev
```

### 2. Get the SDR++ source and add the module

```bash
cd ~
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# Place the module sources inside decoder_modules/
git clone https://github.com/F4JTV/sdrpp_cospas_sarsat.git \
    decoder_modules/cospas_sarsat_decoder
```

### 3. Register the module in the root CMakeLists.txt

Two small edits in `~/SDRPlusPlus/CMakeLists.txt`.

**a) Add the build option** next to the other decoder options
(near the other `OPT_BUILD_*_DECODER` lines):

```cmake
option(OPT_BUILD_COSPAS_SARSAT_DECODER "Build the Cospas-Sarsat 406 MHz decoder module (no dependencies required)" OFF)
```

**b) Add the subdirectory** in the `# Decoders` section
(near the other `add_subdirectory("decoder_modules/...")` lines):

```cmake
if (OPT_BUILD_COSPAS_SARSAT_DECODER)
add_subdirectory("decoder_modules/cospas_sarsat_decoder")
endif (OPT_BUILD_COSPAS_SARSAT_DECODER)
```

### 4. Build

```bash
cd ~/SDRPlusPlus
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_COSPAS_SARSAT_DECODER=ON
make -j$(nproc)
```

The compiled plugin is produced at
`build/decoder_modules/cospas_sarsat_decoder/cospas_sarsat_decoder.so`.

### 5. Install

```bash
sudo make install
```

This installs the whole SDR++ build, including the module, to
`/usr/lib/sdrpp/plugins/`. To install only the module without reinstalling
everything:

```bash
sudo cp decoder_modules/cospas_sarsat_decoder/cospas_sarsat_decoder.so \
    /usr/lib/sdrpp/plugins/
```

---

## Enabling the module in SDR++

1. Launch SDR++
2. Open the **Module Manager** (left-side menu)
3. In the dropdown at the bottom, select `cospas_sarsat_decoder`
4. Enter an instance name (e.g. `SARSAT`) and click **+**
5. Tune to a Cospas-Sarsat frequency (406.025 MHz is a good start)

---

## Technical details

**Signal processing chain**

1. FM demodulation — 6 kHz bandwidth, ±3.5 kHz deviation
2. AGC for level normalization
3. Biphase-L decoding at 400 baud
4. Frame sync: 15-bit preamble (all 1s) + 9-bit sync word
5. BCH error correction: BCH(82,61) t=3 and BCH(38,26) t=2

**Frame structure (144 bits)**

```
| Preamble | Sync   | PDF-1   | BCH-1   | PDF-2   | BCH-2   |
| 15 bits  | 9 bits | 61 bits | 21 bits | 26 bits | 12 bits |
```

**References**

- Cospas-Sarsat T.001 Issue 3 Rev.14
- ITU-R M.633 — EPIRB transmission characteristics
- ITU MID Table — Country codes

---

## License

BSD 3-Clause License — see [LICENSE](LICENSE).

## Credits

- **F4EHY** — original `dec406` decoder (http://jgsenlis.free.fr/)
- **Alexandre Rouma** — SDR++ software
- **F4JTV / ADRASEC 06** — SDR++ module port
