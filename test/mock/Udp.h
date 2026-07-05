// Minimal host-side mock of Udp.h for native unit tests.
// Required transitively by TimestampNtp.hpp; the tests drive time through a
// fake Ntp instead of real UDP, so nothing more is needed here.
#pragma once
#include "Arduino.h"
