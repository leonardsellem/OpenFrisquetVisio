#pragma once
#include <stdint.h>

// Volatile qualification only: NVS values and wall-clock changes are NOT renewals.
// Producer sends all three scalar commands, then session:sequence every 60 s.
// A session changes on every MQTT connection; sequence must strictly increase.
class ControlLease {
public:
    static constexpr uint32_t HEARTBEAT_MS = 60000;
    static constexpr uint32_t LEASE_MS = 1800000;
    enum Field { AMBIENT, TARGET, MODE };

    void reset() { _fields = 0; _armed = false; }
    void mark(Field field, uint32_t now) { _received[field] = now; _fields |= 1U << field; }
    bool renew(uint64_t sequence, uint32_t now) {
        if ((_seenSequence && sequence <= _sequence) || _fields != 7) return false;
        for (uint8_t i = 0; i < 3; ++i)
            if (uint32_t(now - _received[i]) > HEARTBEAT_MS) return false;
        _sequence = sequence;
        _seenSequence = true;
        _fields = 0;
        _renewed = now;
        _armed = true;
        return true;
    }
    bool current(uint32_t now) {
        if (_armed && uint32_t(now - _renewed) >= LEASE_MS) _armed = false;
        return _armed;
    }
private:
    uint32_t _received[3] = {};
    uint32_t _renewed = 0;
    uint64_t _sequence = 0;
    uint8_t _fields = 0;
    bool _armed = false;
    bool _seenSequence = false;
};
