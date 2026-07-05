// Minimal host-side mock of IPAddress.h for native unit tests.
// Required transitively by TimestampNtp.hpp.
#pragma once
#include "Arduino.h"

class IPAddress {
public:
    IPAddress() {}
    IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
};
