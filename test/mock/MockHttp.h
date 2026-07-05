// Shared, programmable HTTP response queue for the native tests.
//
// A test scripts a whole flow (device code -> poll pending -> poll OK ->
// calendar list -> events) by pushing (code, body) pairs with mockHttpPush().
// Each HTTPClient::POST()/GET() pops the next pair in FIFO order: the int code
// becomes the returned HTTP status, and the body is what the paired
// WiFiClientSecure streams to ArduinoJson's deserializeJson(). The mock
// HTTPClient and WiFiClientSecure are otherwise decoupled, exactly like the
// real ones, so the library code runs unmodified.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// One scripted HTTP exchange.
struct MockHttpResponse {
    int code;
    std::string body;
};

// FIFO of scripted responses, consumed by POST()/GET().
inline std::vector<MockHttpResponse>& mockHttpQueue() {
    static std::vector<MockHttpResponse> q;
    return q;
}

// Cursor into mockHttpQueue(): index of the next response to hand out.
inline size_t& mockHttpCursor() {
    static size_t cursor = 0;
    return cursor;
}

// The body the *current* WiFiClientSecure must stream. Set by the HTTPClient
// when POST()/GET() consumes a response, read by the WiFiClientSecure the
// library then passes to deserializeJson().
inline std::string& mockHttpCurrentBody() {
    static std::string body;
    return body;
}

// Records every URI passed to HTTPClient::begin(), newest last, so a test can
// assert how the request (e.g. the events time window) was built.
inline std::vector<std::string>& mockHttpUris() {
    static std::vector<std::string> uris;
    return uris;
}

// Queue a scripted response. Call once per expected POST()/GET(), in order.
inline void mockHttpPush(int code, const char* body) {
    mockHttpQueue().push_back(MockHttpResponse{code, body ? body : ""});
}

// Reset all mock state between tests.
inline void mockHttpReset() {
    mockHttpQueue().clear();
    mockHttpCursor() = 0;
    mockHttpCurrentBody().clear();
    mockHttpUris().clear();
}

// Pop the next scripted response, publish its body for the WiFiClientSecure,
// and return its HTTP code. If the queue is exhausted (a test under-scripted a
// flow) it returns 0 and an empty body, which the library treats as a failure.
inline int mockHttpConsume() {
    std::vector<MockHttpResponse>& q = mockHttpQueue();
    size_t& cursor = mockHttpCursor();
    if (cursor >= q.size()) {
        mockHttpCurrentBody().clear();
        return 0;
    }
    const MockHttpResponse& r = q[cursor++];
    mockHttpCurrentBody() = r.body;
    return r.code;
}
