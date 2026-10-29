#pragma once
#include <stddef.h>
#include <stdint.h>

// 868.96 MHz: 0.1% => 3.6 s/hour, no LBT/AFA claimed.
// 62 minute buckets cover the sliding hour AND a packet crossing a bucket edge.
// Persist the full uncertain attempt before TX; reduce only on verified TX_DONE.
class RadioTxPolicy {
public:
    static constexpr int16_t INHIBITED = -1200;
    static constexpr int16_t AIRTIME_LIMIT = -1201;
    static constexpr uint32_t HOUR_MS = 3600000;
    static constexpr uint32_t BUDGET_US = 3600000;
    static constexpr uint32_t SAFETY_RESERVE_US = 500000; // WITHIN, not above, 3.6 s.
    static constexpr int8_t CHIP_POWER_DBM = -9; // SX1262 input, NOT antenna ERP.
    // Primary docs give gain at 25 C, not a full installed-chain tolerance bound.
    // No runtime override; see README for the precise remaining predicate.
    static constexpr bool HELTEC_V43_ENVELOPE_QUALIFIED = false;

    void reset(uint32_t now, uint32_t recoveredUs = 0) {
        for (auto& bucket : _buckets) bucket = 0;
        _buckets[0] = recoveredUs <= BUDGET_US ? recoveredUs : BUDGET_US;
        _lastTick = now; _uptime = 0; _reservedUs = 0;
    }
    static uint32_t chargeUs(size_t length) {
        // 25 kbit/s, 16-bit preamble, 32-bit sync, length and 16-bit CRC.
        // Reserve RadioLib 7.8.1's whole timeout: 5 ms + 5 * nominal ToA,
        // plus 1 ms rounding. This overcounts even failed TX/IRQ timeouts.
        return 6000U + 5U * uint32_t((length + 9U) * 320U);
    }
    static bool isNoHeat(const uint8_t* data, size_t length) {
        // Exact Z1 INIT: only the admitted 30 C ambient / 5 C target / raw 0x10.
        // Association/message identifiers vary; no public priority/budget bypass.
        static const uint8_t fixed[] = {1,0x17,0xA0,0x29,0,0x15,0xA0,0x2F,0,4,8,1,44,0,50,0,0x10,0,0};
        if (!data || length != 23 || data[0] != 0x80 || data[1] != 8 || data[2] == 0xFF) return false;
        for (size_t i = 0; i < sizeof(fixed); ++i) if (data[i + 4] != fixed[i]) return false;
        return true;
    }
    uint32_t usedUs() const {
        uint32_t used = 0;
        for (uint32_t bucket : _buckets) used += bucket;
        return used;
    }
    bool reserve(uint32_t now, size_t length, bool safety = false) {
        const uint64_t previousMinute = _uptime / 60000U;
        _uptime += uint32_t(now - _lastTick);
        _lastTick = now;
        const uint64_t minute = _uptime / 60000U;
        const uint64_t elapsed = minute - previousMinute;
        for (uint64_t n = 0; n < elapsed && n < 62; ++n)
            _buckets[(previousMinute + n + 1) % 62] = 0;
        if (length == 0 || length > 63) return false;
        const uint32_t charge = chargeUs(length);
        const uint32_t ceiling = safety ? BUDGET_US : BUDGET_US - SAFETY_RESERVE_US;
        const uint32_t used = usedUs();
        if (used > ceiling || charge > ceiling - used) return false;
        _reservedBucket = minute % 62;
        _reservedUs = charge;
        _buckets[_reservedBucket] += charge;
        return true;
    }
    void confirm(uint32_t actualUs) {
        if (actualUs <= _reservedUs) _buckets[_reservedBucket] -= _reservedUs - actualUs;
        _reservedUs = 0;
    }
private:
    uint32_t _buckets[62] = {};
    uint32_t _reservedUs = 0;
    uint8_t _reservedBucket = 0;
    uint32_t _lastTick = 0;
    uint64_t _uptime = 0;
};
