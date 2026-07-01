// Minimal host-side mock of Arduino.h — just enough to compile the
// GoogleSchedular library (and the real ArduinoJson / FastTimer it depends on)
// natively for unit tests. Not for use on a device.
//
// The mock is built around three Arduino types the library and ArduinoJson
// need: String, Stream and Print/Printable. It also provides the flash-memory
// (PROGMEM) shims, which collapse to plain RAM accesses on a host.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <string>

typedef uint8_t byte;

// --- Flash-access shims ---------------------------------------------------
// On a host there is no separate program memory, so every "flash" access is a
// plain RAM access. ArduinoJson keys built with F()/FPSTR go through these.
#define PROGMEM
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*reinterpret_cast<const uint8_t*>(addr))
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef memcmp_P
#define memcmp_P memcmp
#endif
#ifndef strlen_P
#define strlen_P strlen
#endif
#ifndef strcmp_P
#define strcmp_P strcmp
#endif
#ifndef strncmp_P
#define strncmp_P strncmp
#endif

// Flash-string marker type, as on a real core. F("x") / FPSTR(p) yield a
// const __FlashStringHelper*; ArduinoJson treats it as a PROGMEM C string.
// It must be a *complete* type: ArduinoJson takes sizeof() of the pointee.
class __FlashStringHelper {};
#define FPSTR(p) (reinterpret_cast<const __FlashStringHelper*>(p))
#define F(str)   FPSTR(str)


// --- String ---------------------------------------------------------------
// A small but faithful subset of Arduino's String, backed by std::string.
// It exposes exactly the members the library and ArduinoJson use:
//   c_str()/length()  -> mark it as an "Arduino String" to ArduinoJson
//   concat()/operator=(const char*) -> the serializeJson(...) write path
//   equals/toInt/isEmpty/remove + operator+/+= -> the library logic
class String {
public:
    String() : _s() {}
    String(const char* p) : _s(p ? p : "") {}
    // Arduino's String(const __FlashStringHelper*): lets the library pass an
    // F("...")/FPSTR(...) literal wherever a String is expected (e.g. the
    // request paths handed to _postJsonRequest / _getRequest).
    String(const __FlashStringHelper* p)
        : _s(p ? reinterpret_cast<const char*>(p) : "") {}
    String(const String& o) : _s(o._s) {}
    String(const std::string& o) : _s(o) {}
    // Arduino's String(int) renders the number in base 10 (used for the
    // polling interval that GoogleSchedular stashes in _calendarId).
    explicit String(int v) : _s(std::to_string(v)) {}
    explicit String(unsigned int v) : _s(std::to_string(v)) {}
    explicit String(long v) : _s(std::to_string(v)) {}
    explicit String(unsigned long v) : _s(std::to_string(v)) {}

    // ArduinoJson assigns a decoded value with `dst = str.c_str()`, and its
    // String Writer clears the buffer with `str = (const char*)0`.
    String& operator=(const char* p) { _s = p ? p : ""; return *this; }
    String& operator=(const String& o) { _s = o._s; return *this; }

    const char* c_str() const { return _s.c_str(); }
    // Unsigned length() is what ArduinoJson's string_traits keys on to route
    // this type through its Arduino-String adapter.
    unsigned int length() const { return static_cast<unsigned int>(_s.size()); }
    bool isEmpty() const { return _s.empty(); }

    bool equals(const String& o) const { return _s == o._s; }
    bool equals(const char* p) const { return _s == (p ? p : ""); }

    long toInt() const { return std::strtol(_s.c_str(), nullptr, 10); }

    // Arduino's remove(index): drops everything from `index` to the end.
    void remove(unsigned int index) {
        if (index < _s.size()) _s.erase(index);
    }

    // ArduinoJson's String Writer appends through concat(); returns non-zero on
    // success (here always, since std::string does not fail to allocate here).
    unsigned int concat(const char* p) {
        if (p) _s += p;
        return 1;
    }
    unsigned int concat(const String& o) { _s += o._s; return 1; }

    String& operator+=(const char* p) { if (p) _s += p; return *this; }
    String& operator+=(const String& o) { _s += o._s; return *this; }
    // FPSTR("Bearer ") + _accessToken: the left side is a flash string.
    String& operator+=(const __FlashStringHelper* p) {
        if (p) _s += reinterpret_cast<const char*>(p);
        return *this;
    }

    friend String operator+(const String& a, const String& b) {
        String r(a); r += b; return r;
    }
    friend String operator+(const String& a, const char* b) {
        String r(a); r += b; return r;
    }
    friend String operator+(const char* a, const String& b) {
        String r(a); r += b; return r;
    }
    // FPSTR(...) + String, as used by GoogleApiCalendar::_getRequest.
    friend String operator+(const __FlashStringHelper* a, const String& b) {
        String r; r += a; r += b; return r;
    }

    bool operator==(const String& o) const { return _s == o._s; }
    bool operator!=(const String& o) const { return _s != o._s; }

private:
    std::string _s;
};


// --- Print / Printable ----------------------------------------------------
// ArduinoJson references both under ARDUINOJSON_ENABLE_ARDUINO_STREAM (its
// StringBuilderPrint derives from Print). The tests never serialize through
// Print, so a minimal, do-nothing surface is enough to satisfy the compiler.
class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) { return 1; }
    virtual size_t write(const uint8_t* buffer, size_t size) {
        for (size_t i = 0; i < size; ++i) write(buffer[i]);
        return size;
    }
};

class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print&) const { return 0; }
};


// --- Stream ---------------------------------------------------------------
// ArduinoJson deserializes straight from an Arduino Stream via readBytes()
// (see ArduinoStreamReader). Concrete stream mocks (WiFiClientSecure) derive
// from this and feed a canned body; see test/mock/WiFiClientSecure.h.
class Stream {
public:
    virtual ~Stream() {}
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    // ArduinoJson only relies on readBytes(); the default composes it from
    // read()/available() so a subclass can override just those.
    virtual size_t readBytes(char* buffer, size_t length) {
        size_t n = 0;
        while (n < length && available() > 0) {
            int c = read();
            if (c < 0) break;
            buffer[n++] = static_cast<char>(c);
        }
        return n;
    }
};


// --- Fake clock -----------------------------------------------------------
// Driven by the test: set g_fakeMillis, then call the code under test.
extern unsigned long g_fakeMillis;
inline unsigned long millis() { return g_fakeMillis; }
