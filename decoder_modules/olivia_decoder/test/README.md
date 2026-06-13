# Olivia decoder - test tools

Standalone command-line tools that exercise the same Jalocha MFSK engine the
module uses, for validating the decoder without a live radio. They depend only
on the vendored engine under `../src/olivia/jalocha`, so no SDR++ build is
needed.

Build all three:

```
g++ -O2 -std=c++17 -I ../src/olivia olivia_gen.cpp        -o olivia_gen
g++ -O2 -std=c++17 -I ../src/olivia olivia_roundtrip.cpp  -o olivia_roundtrip
g++ -O2 -std=c++17 -I ../src/olivia olivia_decode_wav.cpp -o olivia_decode_wav
```

## olivia_gen - signal generator

Encodes text into an Olivia WAV (8 kHz, mono, 16-bit) with an optional noise
level. This is the generator to use when no on-air signal is available.

```
./olivia_gen --mode 16-500 "CQ CQ DE TEST 73"
./olivia_gen --mode 8-250 --snr 0 --center 1200 "weak signal test"
./olivia_gen --mode 32-1000 --out test.wav "hello world"
```

Options: `--mode tones-bw` (e.g. 4-125, 16-500, 64-2000), `--center Hz`
(default 1500), `--snr dB` (in-band SNR of added white noise; omit for clean),
`--lead sec` (lead-in idle for sync, default 1.0), `--out file`, `--seed N`.

## olivia_roundtrip - automated self-test

Encodes a known message, optionally adds noise, decodes it back and reports
pass/fail across several submodes and SNRs. No arguments:

```
./olivia_roundtrip
```

## olivia_decode_wav - decode a WAV file

Decodes a mono 8 kHz 16-bit WAV, scanning candidate centre frequencies and
printing the best result. Useful for real recordings (convert first, e.g.
`ffmpeg -i sample.mp3 -ac 1 -ar 8000 sample.wav`).

```
./olivia_decode_wav sample.wav <tones> <bandwidth>
./olivia_decode_wav olivia_16-500.wav 16 500
```

Note: the centre must land within the synchroniser's tolerance
(about +/- SyncMargin x toneSpacing). The coarse scan here can occasionally pick
a centre just outside tolerance and print structured garbage; in the SDR++
module you place the centre interactively with the band view and the f/o
read-out, which avoids that.
