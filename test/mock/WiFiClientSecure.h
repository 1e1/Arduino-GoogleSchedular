// Host-side mock of the TLS client used by GoogleOAuth2.
//
// It behaves as an Arduino Stream so ArduinoJson can deserialize a canned JSON
// body straight from it (deserializeJson(doc, _wifiClient)). The body is taken
// from mockHttpCurrentBody(), which the mock HTTPClient publishes when it
// consumes the next scripted response. A fresh read window opens on stop(),
// which the library calls once per request.
#pragma once

#include "Arduino.h"
#include "MockHttp.h"

class WiFiClientSecure : public Stream {
public:
    WiFiClientSecure() : _pos(0), _bodyReadable(false) {}

    // No-op TLS knobs the library calls; kept for API parity with the real one.
    void setInsecure() {}

    // Streaming surface consumed by ArduinoJson's Reader<Stream>.
    // The body only becomes readable once an HTTPClient POST()/GET() has run,
    // i.e. once mockHttpCurrentBody() holds this request's reply.
    int available() override {
        _sync();
        const std::string& b = mockHttpCurrentBody();
        return _pos < b.size() ? static_cast<int>(b.size() - _pos) : 0;
    }

    int read() override {
        _sync();
        const std::string& b = mockHttpCurrentBody();
        if (_pos >= b.size()) return -1;
        return static_cast<unsigned char>(b[_pos++]);
    }

    int peek() override {
        _sync();
        const std::string& b = mockHttpCurrentBody();
        if (_pos >= b.size()) return -1;
        return static_cast<unsigned char>(b[_pos]);
    }

    // The library calls stop() at the end of every request. Arm the next read
    // so a subsequent request streams the freshly published body from its start.
    void stop() {
        _pos = 0;
        _bodyReadable = false;
    }

private:
    // Bind this client to the current scripted body the first time it is read
    // after an HTTPClient consumed a response.
    void _sync() {
        if (!_bodyReadable) {
            _bodyReadable = true;
            _pos = 0;
        }
    }

    size_t _pos;
    bool _bodyReadable;
};
