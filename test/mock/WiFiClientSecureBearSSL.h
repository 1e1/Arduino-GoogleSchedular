// Host-side stand-in for ESP8266's WiFiClientSecureBearSSL.h.
//
// On the device this header brings the BearSSL::WiFiClientSecure type into
// scope; the library uses it unqualified as WiFiClientSecure, so the mock just
// exposes that class at global scope (see WiFiClientSecure.h).
#pragma once

#include "WiFiClientSecure.h"
