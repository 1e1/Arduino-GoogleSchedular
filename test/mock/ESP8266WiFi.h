// Host-side stand-in for ESP8266WiFi.h.
//
// GoogleSchedular includes it for the Wi-Fi radio, but the OAuth/calendar logic
// under test never touches the radio (it only uses the HTTP/TLS clients), so an
// empty shim is enough to satisfy the include.
#pragma once

#include "Arduino.h"
