#pragma once
#include <stddef.h>
#include <stdint.h>

// 868.96 MHz: 0.1% => 3.6 s/hour, no LBT/AFA claimed.
// 61 minute buckets conservatively cover EVERY sliding hour (including edges).
// Charge before attempting TX, including failures/retries; never refund.
class RadioTxPolicy {
public:
    static constexpr int16_t INHIBITED = -1200;
    static constexpr int16_t AIRTIME_LIMIT = -1201;
    static constexpr uint32_t HOUR_MS = 3600000;
    static constexpr uint32_t BUDGET_US = 3600000;
    static constexpr int8_t CHIP_POWER_DBM = -9; // SX1262 input, NOT antenna ERP.

    void reset(uint32_t now) { _lastTick = now; _uptime = 0; }
    static uint32_t chargeUs(size_t length) {
        // 25 kbit/s, 16-bit preamble, 32-bit sync, length and 16-bit CRC.
        // Reserve RadioLib 7.8.1's whole timeout: 5 ms + 5 * nominal ToA,
        // plus 1 ms rounding. This overcounts even failed TX/IRQ timeouts.
        return 6000U + 5U * uint32_t((length + 9U) * 320U);
    }
    bool reserve(uint32_t now, size_t length) {
        const uint64_t previousMinute = _uptime / 60000U;
        _uptime += uint32_t(now - _lastTick);
        _lastTick = now;
        const uint64_t minute = _uptime / 60000U;
        const uint64_t elapsed = minute - previousMinute;
        for (uint64_t n = 0; n < elapsed && n < 61; ++n)
            _buckets[(previousMinute + n + 1) % 61] = 0;
        // Reboot cannot reset an on-air budget: wait out the unknown old hour.
        if (_uptime < HOUR_MS || length == 0 || length > 63) return false;
        const uint32_t charge = chargeUs(length);
        uint32_t used = 0;
        for (uint32_t bucket : _buckets) used += bucket;
        if (charge > BUDGET_US - used) return false;
        _buckets[minute % 61] += charge;
        return true;
    }
private:
    uint32_t _buckets[61] = {};
    uint32_t _lastTick = 0;
    uint64_t _uptime = 0;
};
