// Host-side mock of ESP8266HTTPClient's HTTPClient.
//
// It implements exactly the members GoogleOAuth2/GoogleApiCalendar call and
// drives the shared response queue (see MockHttp.h): POST()/GET() pop the next
// scripted (code, body) pair, publishing the body for the paired
// WiFiClientSecure and returning the code. begin() records the request URI so a
// test can assert how it was assembled.
#pragma once

#include "Arduino.h"
#include "MockHttp.h"
#include "WiFiClientSecure.h"

// HTTP status codes the library compares against (names/values match the real
// ESP8266HTTPClient enum).
#ifndef HTTP_CODE_OK
#define HTTP_CODE_OK 200
#endif
#ifndef HTTP_CODE_PRECONDITION_REQUIRED
#define HTTP_CODE_PRECONDITION_REQUIRED 428
#endif

class HTTPClient {
public:
    HTTPClient() {}

    // begin(client, host, port, path, https): the library always passes the
    // full request path here; record it so tests can inspect the built URI.
    bool begin(WiFiClientSecure& /*client*/, const String& /*host*/,
               uint16_t /*port*/, const String& path, bool /*https*/) {
        mockHttpUris().push_back(path.c_str());
        return true;
    }
    // Overload accepting flash-string host/path, matching how the library may
    // pass F("...") literals.
    bool begin(WiFiClientSecure& /*client*/, const __FlashStringHelper* /*host*/,
               uint16_t /*port*/, const String& path, bool /*https*/) {
        mockHttpUris().push_back(path.c_str());
        return true;
    }

    void addHeader(const String& /*name*/, const String& /*value*/) {}
    void useHTTP10(bool /*use*/) {}

    // POST/GET consume the next scripted response (publishing its body for the
    // WiFiClientSecure to stream) and return its HTTP status code.
    int POST(const String& /*payload*/) { return mockHttpConsume(); }
    int GET() { return mockHttpConsume(); }

    void end() {}
};
