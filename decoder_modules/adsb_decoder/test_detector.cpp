#include "src/adsb/frame_detector.h"
#include "src/adsb/adsb.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdlib>

static adsb::RawFrame fromHex(const char* hex) {
    adsb::RawFrame f{};
    int n = (int)strlen(hex) / 2;
    f.len = n;
    for (int i = 0; i < n; i++) {
        char h[3] = { hex[i*2], hex[i*2+1], 0 };
        f.bytes[i] = (uint8_t)strtol(h, nullptr, 16);
    }
    return f;
}

// Build a synthetic 2 MHz magnitude stream for a frame (preamble + PPM bits).
static void appendFrame(std::vector<float>& s, const adsb::RawFrame& f, float high, float low) {
    // Preamble: pulses (high) at samples 0,2,7,9; rest low.
    float pre[16];
    for (int i = 0; i < 16; i++) pre[i] = low;
    pre[0] = pre[2] = pre[7] = pre[9] = high;
    for (int i = 0; i < 16; i++) s.push_back(pre[i]);
    // Data: bit=1 -> [high,low], bit=0 -> [low,high].
    int bits = f.len * 8;
    for (int i = 0; i < bits; i++) {
        int byte = i >> 3, bit = i & 7;
        int b = (f.bytes[byte] >> (7 - bit)) & 1;
        if (b) { s.push_back(high); s.push_back(low); }
        else   { s.push_back(low);  s.push_back(high); }
    }
}

static void appendNoise(std::vector<float>& s, int n, float level) {
    for (int i = 0; i < n; i++) s.push_back(level * ((float)rand() / RAND_MAX));
}

int main() {
    printf("=== ADS-B frame detector round-trip ===\n\n");
    srand(12345);

    std::vector<float> stream;
    appendNoise(stream, 137, 1.0f);                                  // quiet lead-in
    appendFrame(stream, fromHex("8D4840D6202CC371C32CE0576098"), 100, 2); // ident
    appendNoise(stream, 53, 1.0f);
    appendFrame(stream, fromHex("8D40621D58C382D690C8AC2863A7"), 100, 2); // pos even
    appendNoise(stream, 89, 1.0f);
    appendFrame(stream, fromHex("8D485020994409940838175B284F"), 100, 2); // velocity
    appendNoise(stream, 240, 1.0f);                                  // tail

    int found = 0;
    std::vector<adsb::Message> msgs;
    adsb::FrameDetector det;
    det.onFrame = [&](const adsb::RawFrame& f) {
        found++;
        adsb::Message m;
        if (adsb::parseAdsb(f.bytes, m)) msgs.push_back(m);
        printf("  frame len=%d  ICAO=%06X  DF=%d  TC=%d\n",
               f.len, adsb::icaoAddress(f.bytes),
               adsb::downlinkFormat(f.bytes), adsb::typeCode(f.bytes));
    };

    // Feed the stream in small irregular chunks to exercise boundary handling.
    int pos = 0, n = (int)stream.size();
    int chunkSizes[] = {64, 100, 33, 250, 17, 400, 7};
    int ci = 0;
    while (pos < n) {
        int c = chunkSizes[ci++ % 7];
        if (pos + c > n) c = n - pos;
        det.process(stream.data() + pos, c);
        pos += c;
    }

    printf("\n  frames detected: %d (expected 3)\n", found);

    int fails = 0;
    if (found != 3) { printf("  [FAIL] expected 3 frames\n"); fails++; }
    else            { printf("  [OK]   3 frames detected\n"); }

    bool gotIdent = false, gotPos = false, gotVel = false;
    for (auto& m : msgs) {
        if (m.kind == adsb::KIND_IDENT && m.callsign == "KLM1023") gotIdent = true;
        if (m.kind == adsb::KIND_AIRBORNE_POS && m.icao == 0x40621D) gotPos = true;
        if (m.kind == adsb::KIND_VELOCITY && std::abs((int)m.speedKt - 159) <= 1) gotVel = true;
    }
    if (gotIdent) printf("  [OK]   identification recovered (KLM1023)\n"); else { printf("  [FAIL] identification\n"); fails++; }
    if (gotPos)   printf("  [OK]   position frame recovered (40621D)\n");   else { printf("  [FAIL] position\n");       fails++; }
    if (gotVel)   printf("  [OK]   velocity recovered (159 kt)\n");         else { printf("  [FAIL] velocity\n");       fails++; }

    printf("\n=== %s ===\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}
