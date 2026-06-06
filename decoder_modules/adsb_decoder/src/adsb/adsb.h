#pragma once
#include <stdint.h>
#include <string>
#include <cstddef>

// ----------------------------------------------------------------------------
// Pure ADS-B (Mode S Extended Squitter, 1090 MHz) decoding logic.
//
// This file has NO dependency on SDR++ so it can be unit-tested standalone.
// It operates on raw decoded frames (already demodulated/sliced into bytes).
//
//  - Mode S CRC-24 verification (DF11/DF17/DF18 syndrome must be 0).
//  - Downlink Format / ICAO address extraction.
//  - DF17/DF18 ADS-B message parsing:
//      * Aircraft identification (TC 1-4)  -> callsign + category
//      * Airborne position    (TC 9-18,20-22) -> CPR lat/lon + altitude
//      * Surface position     (TC 5-8)        -> CPR lat/lon + ground speed
//      * Airborne velocity    (TC 19)         -> speed, heading, vertical rate
//  - CPR global decode (even+odd pair) and local decode (with reference).
// ----------------------------------------------------------------------------

namespace adsb {

    // A raw Mode S frame: 7 bytes (short, 56 bits) or 14 bytes (long, 112 bits).
    struct RawFrame {
        uint8_t bytes[14];
        int len; // 7 or 14
    };

    // Compute the Mode S 24-bit CRC syndrome over a frame.
    // For a valid DF17/DF18 frame (parity overlaid with 0), this returns 0.
    uint32_t crc(const uint8_t* msg, int lenBytes);

    // Downlink Format (top 5 bits).
    inline int downlinkFormat(const uint8_t* msg) { return (msg[0] >> 3) & 0x1F; }

    // ICAO 24-bit address. For DF17/18 it is bits 9..32 (bytes 1..3).
    inline uint32_t icaoAddress(const uint8_t* msg) {
        return ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];
    }

    // ADS-B Type Code (first 5 bits of the ME field; ME starts at byte 4).
    inline int typeCode(const uint8_t* msg) { return (msg[4] >> 3) & 0x1F; }

    // Decoded message kinds.
    enum MsgKind {
        KIND_UNKNOWN = 0,
        KIND_IDENT,            // callsign
        KIND_AIRBORNE_POS,     // lat/lon (CPR) + altitude
        KIND_SURFACE_POS,      // lat/lon (CPR) + ground speed
        KIND_VELOCITY          // speed/heading/vertical rate
    };

    // Result of decoding a single DF17/DF18 ADS-B message.
    struct Message {
        MsgKind kind = KIND_UNKNOWN;
        uint32_t icao = 0;
        int typeCode = 0;

        // Identification
        std::string callsign;      // trimmed
        int category = 0;

        // Position (raw CPR halves; need global/local decode to get lat/lon)
        bool cprOddFrame = false;  // F bit: false=even, true=odd
        uint32_t cprLat = 0;       // 17-bit
        uint32_t cprLon = 0;       // 17-bit
        bool hasAltitude = false;
        int altitudeFt = 0;        // barometric, feet

        // Velocity (TC 19)
        bool hasGroundSpeed = false;
        double speedKt = 0.0;      // knots (ground speed or airspeed)
        bool airspeed = false;     // true if IAS/TAS instead of ground speed
        bool hasHeading = false;
        double headingDeg = 0.0;
        bool hasVerticalRate = false;
        int verticalRateFpm = 0;   // feet per minute, signed
    };

    // Parse a DF17/DF18 long frame (14 bytes) into a Message.
    // Returns true if the message kind was recognised.
    bool parseAdsb(const uint8_t* msg, Message& out);

    // CPR: number of longitude zones for a given latitude (NL function).
    int cprNL(double lat);

    // Global CPR decode from an even and an odd frame.
    //  cprLatE/cprLonE : 17-bit CPR halves from the even frame
    //  cprLatO/cprLonO : 17-bit CPR halves from the odd frame
    //  mostRecentOdd   : true if the odd frame is the most recent of the pair
    //  surface         : true for surface position (TC 5-8)
    // Returns true on success and fills lat/lon (degrees).
    bool cprDecodeGlobal(uint32_t cprLatE, uint32_t cprLonE,
                         uint32_t cprLatO, uint32_t cprLonO,
                         bool mostRecentOdd, bool surface,
                         double& lat, double& lon);

    // Local CPR decode using a known reference position (faster, single frame).
    bool cprDecodeLocal(uint32_t cprLat, uint32_t cprLon, bool oddFrame, bool surface,
                        double refLat, double refLon,
                        double& lat, double& lon);

} // namespace adsb
