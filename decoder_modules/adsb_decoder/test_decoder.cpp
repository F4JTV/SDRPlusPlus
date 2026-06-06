#include "src/adsb/adsb.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// Parse a hex string like "8D4840D6..." into bytes.
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

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  [OK]   %s\n", msg); } else { printf("  [FAIL] %s\n", msg); fails++; } } while(0)

int main() {
    printf("=== ADS-B decoder validation ===\n\n");

    // ---- 1. CRC must be 0 for valid DF17 frames ----
    printf("[1] CRC-24 syndrome (must be 0 for valid frames)\n");
    {
        auto f = fromHex("8D4840D6202CC371C32CE0576098"); // ident KLM1023
        printf("  ident frame  syndrome = 0x%06X\n", adsb::crc(f.bytes, f.len));
        CHECK(adsb::crc(f.bytes, f.len) == 0, "ident frame CRC == 0");
    }
    {
        auto f = fromHex("8D40621D58C382D690C8AC2863A7"); // position even
        printf("  pos-even     syndrome = 0x%06X\n", adsb::crc(f.bytes, f.len));
        CHECK(adsb::crc(f.bytes, f.len) == 0, "pos-even frame CRC == 0");
    }
    {
        auto f = fromHex("8D485020994409940838175B284F"); // velocity
        printf("  velocity     syndrome = 0x%06X\n", adsb::crc(f.bytes, f.len));
        CHECK(adsb::crc(f.bytes, f.len) == 0, "velocity frame CRC == 0");
    }
    {
        // Corrupt one byte: must NOT be 0.
        auto f = fromHex("8D4840D6202CC371C32CE0576098");
        f.bytes[5] ^= 0x01;
        CHECK(adsb::crc(f.bytes, f.len) != 0, "corrupted frame CRC != 0");
    }

    // ---- 2. ICAO + callsign ----
    printf("\n[2] Identification (ICAO + callsign)\n");
    {
        auto f = fromHex("8D4840D6202CC371C32CE0576098");
        adsb::Message m;
        bool ok = adsb::parseAdsb(f.bytes, m);
        printf("  ICAO=%06X  callsign='%s'  cat=%d\n", m.icao, m.callsign.c_str(), m.category);
        CHECK(ok && m.kind == adsb::KIND_IDENT, "parsed as identification");
        CHECK(m.icao == 0x4840D6, "ICAO == 4840D6");
        CHECK(m.callsign == "KLM1023", "callsign == 'KLM1023'");
    }

    // ---- 3. Global CPR position (even + odd) ----
    printf("\n[3] Airborne position (global CPR decode)\n");
    {
        auto fe = fromHex("8D40621D58C382D690C8AC2863A7"); // even, t=0
        auto fo = fromHex("8D40621D58C386435CC412692AD6"); // odd,  t=10s
        adsb::Message me, mo;
        adsb::parseAdsb(fe.bytes, me);
        adsb::parseAdsb(fo.bytes, mo);
        printf("  even F=%d altFt=%d  odd F=%d\n", me.cprOddFrame, me.altitudeFt, mo.cprOddFrame);
        CHECK(me.cprOddFrame == false, "even frame F bit == 0");
        CHECK(mo.cprOddFrame == true,  "odd frame F bit == 1");
        CHECK(me.icao == 0x40621D,     "ICAO == 40621D");

        double lat, lon;
        // The canonical vector uses the EVEN frame as the most recent.
        bool ok = adsb::cprDecodeGlobal(me.cprLat, me.cprLon, mo.cprLat, mo.cprLon,
                                        false, false, lat, lon);
        printf("  decoded lat=%.6f lon=%.6f  (expected ~52.2572, ~3.91937)\n", lat, lon);
        CHECK(ok, "global CPR decode succeeded");
        CHECK(std::fabs(lat - 52.2572) < 0.01, "latitude ~= 52.2572");
        CHECK(std::fabs(lon - 3.91937) < 0.01, "longitude ~= 3.91937");

        printf("  altitude (from even frame) = %d ft (expected 38000)\n", me.altitudeFt);
        CHECK(me.hasAltitude && std::abs(me.altitudeFt - 38000) <= 25, "altitude ~= 38000 ft");
    }

    // ---- 4. Velocity ----
    printf("\n[4] Airborne velocity\n");
    {
        auto f = fromHex("8D485020994409940838175B284F");
        adsb::Message m;
        bool ok = adsb::parseAdsb(f.bytes, m);
        printf("  speed=%.1f kt  heading=%.2f deg  vrate=%d fpm\n",
               m.speedKt, m.headingDeg, m.verticalRateFpm);
        CHECK(ok && m.kind == adsb::KIND_VELOCITY, "parsed as velocity");
        CHECK(std::fabs(m.speedKt - 159.0) < 1.0, "ground speed ~= 159 kt");
        CHECK(std::fabs(m.headingDeg - 182.88) < 0.5, "heading ~= 182.88 deg");
        CHECK(std::abs(m.verticalRateFpm - (-832)) <= 64, "vertical rate ~= -832 fpm");
    }

    printf("\n=== %s (%d failure%s) ===\n", fails ? "FAILURES" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
