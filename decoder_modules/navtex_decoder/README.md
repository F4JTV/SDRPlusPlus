# NAVTEX Decoder (SITOR-B / CCIR 476) for SDR++

An SDR++ decoder module for **NAVTEX** maritime safety broadcasts. NAVTEX is the
international system used to distribute navigational warnings, meteorological
warnings and forecasts, and urgent marine safety information to ships, primarily
on **518 kHz** (international/English), **490 kHz** (national languages), and
**4209.5 kHz** (tropical/HF).

NAVTEX transmissions use **SITOR collective B-mode** (a.k.a. AMTOR FEC mode):
narrow-shift FSK with a forward-error-correction scheme built on the **CCIR 476**
7-bit constant-ratio alphabet. This module demodulates the FSK, recovers the FEC
time-diversity stream, and prints the decoded message text, including the
`ZCZC B1B2B3B4` header (station + subject + serial) and the `NNNN` end marker.

---

## Features

- **SITOR-B / CCIR 476 FEC decoding** — full constant-ratio (4-of-7) error
  detection with time-diversity recovery: it reads the DX (alpha) copy of each
  character and falls back to the RX (rep) copy, then to soft-decision
  averaging, when the primary copy fails its mark/space ratio check.
- **Automatic phasing lock** — finds the alpha/rep character alignment from the
  idle phasing sequence with no manual intervention, and re-acquires if sync is
  lost.
- **Synchronous DPLL bit recovery** — a transition-locked timing-error-detector
  PLL samples one soft bit per symbol at mid-bit, giving solid performance down
  into noise (validated with added Gaussian noise; see *Validation* below).
- **Sideband selection** — USB (default), LSB, or NFM front ends.
- **Reverse** — one click to swap mark/space when the signal is inverted.
- **AFC** — optional automatic frequency correction that locks onto the centre of
  the balanced mark/space tone pair, with a live readout of the tracked AF
  offset.
- **Interactive band-view widget** — shows the two tone markers (AF ± 85 Hz) and
  a faint centre line; drag to set the audio-frequency placement directly.
- **Live message header parsing** — decodes the `ZCZC B1 B2 NN` header out of the
  text stream and shows the originating station character, the human-readable
  **subject category** (from the B2 subject-indicator table), and the message
  serial number.
- **Scrolling decoded-text panel** with a **Clear** button, plus a collapsible
  in-UI legend of the B2 subject categories.
- Configuration (sideband, AF frequency, AFC, squelch, reverse) is persisted to
  `navtex_decoder_config.json`.

---

## Requirements

- A working **SDR++** build tree (this module is built in-tree alongside the
  other decoder modules).
- Build toolchain for SDR++ on **Ubuntu 24.04** (CMake ≥ 3.13, a C++17
  compiler, and the usual SDR++ build dependencies).

---

## Build (in-tree, Ubuntu 24.04)

1. **Copy the module** into the SDR++ source tree, next to the other decoders:

   ```bash
   cp -r navtex_decoder /path/to/SDRPlusPlus/decoder_modules/
   ```

2. **Register it in the root `CMakeLists.txt`.** Add a build option next to the
   other decoder options:

   ```cmake
   option(OPT_BUILD_NAVTEX_DECODER "Build NAVTEX decoder" ON)
   ```

   and add the subdirectory in the section where the other decoder modules are
   added:

   ```cmake
   if (OPT_BUILD_NAVTEX_DECODER)
       add_subdirectory("decoder_modules/navtex_decoder")
   endif()
   ```

3. **Configure and build:**

   ```bash
   cd /path/to/SDRPlusPlus
   mkdir -p build && cd build
   cmake .. -DOPT_BUILD_NAVTEX_DECODER=ON
   make -j$(nproc) navtex_decoder
   ```

   The compiled module (`navtex_decoder.so`) will be placed with the other
   built modules. Install/copy it where your SDR++ instance loads modules from,
   then enable it from **Module Manager** in the SDR++ UI.

---

## Usage

1. Add a **NAVTEX Decoder** instance from the Module Manager.
2. Set **Sideband** to **USB** (the normal choice for NAVTEX).
3. Tune the radio to a NAVTEX channel — **518 kHz** is the main international
   channel; **490 kHz** and **4209.5 kHz** are also used.
4. Place the audio passband over the signal: the widget shows the two tone
   markers at **AF ± 85 Hz** (170 Hz shift). Drag the band-view, or use the
   **AF frequency** slider, so the two markers sit on the two FSK tones.
5. Enable **AFC** to let the decoder track and centre the tone pair
   automatically; the tracked offset is shown next to the control.
6. If the signal is inverted (mark/space swapped), tick **Reverse**.
7. Use **Squelch** to suppress decoding on noise between transmissions.

When sync is achieved the status shows **FEC synced**, the header line resolves
the station/subject/serial, and decoded text scrolls in the panel. Each NAVTEX
message is framed `ZCZC B1B2B3B4 … NNNN`, where `B1` is the station,
`B2` the subject category, `B3B4` the message serial.

---

## Module structure

```
navtex_decoder/
├── CMakeLists.txt           # standard in-tree SDR++ module definition
├── README.md
└── src/
    ├── main.cpp             # ModuleManager instance: VFO, UI, config, sideband
    ├── decoder.h            # abstract decoder control interface
    └── navtex/
        ├── ccir476.h        # CCIR 476 alphabet tables + 4-of-7 check / decode
        ├── sitorb.h         # SITOR-B FEC: phasing lock, DX/RX diversity, shift
        ├── modem.h          # FSK front end (correlators, spectrum, AFC) + DPLL
        └── decoder.h        # NAVTEX decoder: squelch → modem → text + header UI
```

The FSK front end (mark/space sliding correlators, sideband handling, spectrum
DFT bank, and AFC) follows the same architecture as the RTTY decoder; the NAVTEX
path replaces the asynchronous UART sampler with a **synchronous,
transition-locked DPLL** feeding one soft bit per symbol into the SITOR-B FEC
state machine.

---

## Validation

The decoder was validated against synthetic SITOR-B signals generated with full
phasing, DX/RX time-diversity interleaving, shift management, and FSK modulation
to complex baseband (fs = 24 kHz, AF = 1 kHz, 170 Hz shift). The test message
(`ZCZC FA08 … NNNN`, a gale-warning bulletin) was recovered correctly,
including the `ZCZC` header and `NNNN` terminator, in every case:

| Case               | Result |
|--------------------|:------:|
| USB, clean         | PASS   |
| USB + noise (0.30) | PASS   |
| USB + noise (0.60) | PASS   |
| Reverse (inverted) | PASS   |

---

## Notes

- NAVTEX is a fixed-format mode: **SITOR-B, 100 baud, 170 Hz shift**. There are
  no user-adjustable modem parameters — the only signal controls are sideband,
  AF placement, AFC, and reverse.
- The B2 subject table follows the standard NAVTEX subject indicator
  characters; the in-UI legend lists them for quick reference.
