#pragma once
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace NumericPayload {
// Decimal only, no whitespace, embedded NUL, hex, partial parse or saturation.
inline bool parse(const char* text, size_t length, float minimum, float maximum, float& result) {
    if (!length) return false;
    size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    bool digit = false;
    while (i < length && text[i] >= '0' && text[i] <= '9') { ++i; digit = true; }
    if (i < length && text[i] == '.') {
        ++i;
        while (i < length && text[i] >= '0' && text[i] <= '9') { ++i; digit = true; }
    }
    if (!digit) return false;
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < length && (text[i] == '-' || text[i] == '+')) ++i;
        const size_t exponent = i;
        while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == exponent) return false;
    }
    if (i != length) return false;
    char* end = nullptr;
    errno = 0;
    const float value = strtof(text, &end);
    if (errno == ERANGE || end != text + length || !isfinite(value)
        || value < minimum || value > maximum) return false;
    result = value;
    return true;
}

inline bool unsignedInteger(const char* text, size_t length, uint64_t& result) {
    if (!length) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint8_t digit = text[i] - '0';
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    result = value;
    return true;
}
}
