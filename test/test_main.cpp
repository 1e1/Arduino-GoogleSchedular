// Native (host) unit tests for the GoogleSchedular library.
//
// The library is written for the ESP8266/ESP32, but its non-trivial logic (the
// OAuth2 device flow state machine, calendar resolution and the event query) is
// plain C++. These tests compile the real, unmodified library against the mocks
// in test/mock/ so that logic can be exercised on the host — see test/run.sh.
//
// The network layer is mocked so a test can script a whole flow with
// mockHttpPush(code, body): each HTTPClient POST()/GET() pops the next scripted
// response in FIFO order and the paired WiFiClientSecure streams its body into
// ArduinoJson, exactly as a real reply would. Time is provided by a settable
// fake Ntp so token/poll deadlines are fully deterministic.
//
// Covered:
//   1. State predicates       (hasFailed / isInitialized / isAuthenticated / isLinked)
//   2. startRegistration      (parsing + interval stashed in _calendarId)
//   3. handleRegistration     (PENDING re-arms, OK -> AUTHENTICATED, error -> ERROR)
//   4. maintain() dispatch     (refresh / poll / bootstrap-from-refresh_token)
//   5. setCalendar            (match -> LINKED, no match -> AUTHENTICATED)
//   6. syncAt                 (event list + time window, const char* and String)
//   7. malformed body on 200  (demoted to a failure -> ERROR)

#define ESP8266 1  // select the ESP8266 include branch of GoogleSchedular.hpp

#include <cstdio>
#include <cstring>
#include <list>

#include "GoogleSchedular.hpp"

// Backing storage for the mocked millis() (declared extern in the Arduino mock).
// Time under test comes from FakeNtp below; this only satisfies FastTimer code.
unsigned long g_fakeMillis = 0;

// --- test harness ---------------------------------------------------------

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        ++g_failures; \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_STR(got, want) do { \
    if (std::strcmp((got), (want)) != 0) { \
        ++g_failures; \
        std::printf("  FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
    } \
} while (0)


// --- fake clock -----------------------------------------------------------

// Injected time source: GoogleSchedular stores an Ntp* and only ever calls
// ->time(), so a settable subclass gives full control over token/poll timing.
class FakeNtp : public Ntp {
public:
    unsigned long time(void) const override { return _t; }
    void set(unsigned long t) { _t = t; }
private:
    unsigned long _t = 1000;
};


// A GoogleSchedular subclass that exposes the protected internals a couple of
// tests need to observe directly (the state and the reused _calendarId /
// _expirationTimestamp members). This does not change any behavior; it only
// reads private fields so the assertions can be precise.
class TestSchedular : public GoogleSchedular {
public:
    TestSchedular(const String& id, const String& secret, Ntp* ntp)
        : GoogleSchedular(id, secret, ntp) {}
    State state() const { return _state; }
    const String& calendarIdRaw() const { return _calendarId; }
    unsigned long expiration() const { return _expirationTimestamp; }
};


// --- shared flow helpers --------------------------------------------------

// Drive a scheduler from VOID to AUTHENTICATED: one device-code reply, then one
// token reply. `expiresIn` sets the access_token lifetime so a later test can
// make it expire on demand. Leaves the fake clock at `now`.
static void driveToAuthenticated(TestSchedular& sched, FakeNtp& ntp,
                                 unsigned long now, unsigned int expiresIn) {
    mockHttpReset();
    mockHttpPush(200, "{\"verification_url\":\"https://www.google.com/device\","
                      "\"user_code\":\"WXYZ-1234\",\"interval\":5,"
                      "\"device_code\":\"DEVICE_CODE_ABC\"}");
    String url, code;
    ntp.set(1000);
    sched.startRegistration(url, code);

    char body[128];
    std::snprintf(body, sizeof(body),
                  "{\"access_token\":\"ACCESS_TOKEN\",\"refresh_token\":\"REFRESH_TOKEN\","
                  "\"expires_in\":%u}", expiresIn);
    mockHttpReset();
    mockHttpPush(200, body);
    ntp.set(now);              // past the poll deadline (1000 + interval)
    sched.handleRegistration();
}


// --- 1. state predicates --------------------------------------------------

// A tiny probe that lets us set an arbitrary State and read the four predicates.
// It documents the CADE bitmask: isAuthenticated() is the single bit 0b0100, so
// both AUTHENTICATED and LINKED are "authenticated".
class PredicateProbe : public GoogleSchedular {
public:
    PredicateProbe(Ntp* ntp) : GoogleSchedular(String("i"), String("s"), ntp) {}
    void force(State s) { _state = s; }
};

static void test_state_predicates() {
    std::printf("state predicates\n");
    FakeNtp ntp;
    PredicateProbe p(&ntp);

    struct Row {
        GoogleSchedular::State s;
        bool failed, initialized, authenticated, linked;
    } rows[] = {
        // state,                              failed init  auth   linked
        { GoogleSchedular::VOID,          false, false, false, false },
        { GoogleSchedular::INIT,          false, true,  false, false },
        { GoogleSchedular::AUTHENTICATED, false, false, true,  false },
        { GoogleSchedular::LINKED,        false, false, true,  true  },
        { GoogleSchedular::ERROR,         true,  false, false, false },
    };

    for (auto& r : rows) {
        p.force(r.s);
        CHECK(p.hasFailed()       == r.failed);
        CHECK(p.isInitialized()   == r.initialized);
        // isAuthenticated() is the bit test _state & 0b0100.
        CHECK(p.isAuthenticated() == r.authenticated);
        CHECK(p.isLinked()        == r.linked);
    }
}


// --- 2. startRegistration -------------------------------------------------

static void test_start_registration() {
    std::printf("startRegistration\n");
    FakeNtp ntp; ntp.set(1000);
    TestSchedular sched(String("client-id"), String("client-secret"), &ntp);

    mockHttpReset();
    mockHttpPush(200, "{\"verification_url\":\"https://www.google.com/device\","
                      "\"user_code\":\"ABCD-EFGH\",\"interval\":7,"
                      "\"device_code\":\"DEVICE_CODE_1\"}");
    String url, code;
    sched.startRegistration(url, code);

    CHECK(sched.isInitialized());
    CHECK(sched.state() == GoogleSchedular::INIT);
    CHECK_STR(url.c_str(),  "https://www.google.com/device");
    CHECK_STR(code.c_str(), "ABCD-EFGH");
    // The polling interval is stashed in _calendarId as a String (no calendar is
    // known yet). Assert it directly, and that the deadline is now + interval.
    CHECK_STR(sched.calendarIdRaw().c_str(), "7");
    CHECK(sched.expiration() == 1000 + 7);

    // A non-OK device-code reply must leave the state at VOID (unchanged).
    FakeNtp ntp2; ntp2.set(1000);
    TestSchedular sched2(String("client-id"), String("client-secret"), &ntp2);
    mockHttpReset();
    mockHttpPush(500, "{}");
    String u2, c2;
    sched2.startRegistration(u2, c2);
    CHECK(sched2.state() == GoogleSchedular::VOID);
}


// --- 3. handleRegistration / pollAuthorization ----------------------------

static void test_handle_registration() {
    std::printf("handleRegistration / pollAuthorization\n");

    // 3a. PENDING (428) keeps INIT and re-arms the poll using the stashed
    //     interval, observed through the timing of the next poll.
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        mockHttpReset();
        mockHttpPush(200, "{\"verification_url\":\"u\",\"user_code\":\"c\","
                          "\"interval\":5,\"device_code\":\"D\"}");
        String u, c; sched.startRegistration(u, c);      // deadline = 1005

        // Before the deadline: no poll (queue untouched).
        mockHttpReset();
        ntp.set(1004);
        sched.handleRegistration();
        CHECK(mockHttpCursor() == 0);

        // Past the deadline: polls, gets PENDING, stays INIT, re-arms to 1006+5.
        mockHttpReset();
        mockHttpPush(HTTP_CODE_PRECONDITION_REQUIRED, "{\"error\":\"authorization_pending\"}");
        ntp.set(1006);
        sched.handleRegistration();
        CHECK(mockHttpCursor() == 1);
        CHECK(sched.isInitialized());

        // Re-armed deadline is 1011: before it, no poll (proves the interval was
        // re-stashed from _calendarId).
        mockHttpReset();
        ntp.set(1009);
        sched.handleRegistration();
        CHECK(mockHttpCursor() == 0);
        CHECK(sched.expiration() == 1006 + 5);
    }

    // 3b. OK (200) with tokens -> AUTHENTICATED, deadline uses the safety margin.
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        mockHttpReset();
        mockHttpPush(200, "{\"verification_url\":\"u\",\"user_code\":\"c\","
                          "\"interval\":5,\"device_code\":\"D\"}");
        String u, c; sched.startRegistration(u, c);

        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"AT\",\"refresh_token\":\"RT\","
                          "\"expires_in\":3600}");
        ntp.set(2000);                                   // past the deadline
        sched.handleRegistration();
        CHECK(sched.isAuthenticated());
        CHECK(sched.state() == GoogleSchedular::AUTHENTICATED);
        CHECK(!sched.isInitialized());
        // expiry = now + expires_in - EXPIRATION_TIME_MARGIN
        CHECK(sched.expiration() ==
              2000 + 3600 - GoogleSchedular::EXPIRATION_TIME_MARGIN);
    }

    // 3c. Any other HTTP code -> ERROR.
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        mockHttpReset();
        mockHttpPush(200, "{\"verification_url\":\"u\",\"user_code\":\"c\","
                          "\"interval\":5,\"device_code\":\"D\"}");
        String u, c; sched.startRegistration(u, c);

        mockHttpReset();
        mockHttpPush(403, "{\"error\":\"access_denied\"}");
        ntp.set(2000);
        sched.handleRegistration();
        CHECK(sched.hasFailed());
        CHECK(sched.state() == GoogleSchedular::ERROR);
    }
}


// --- 4. maintain() dispatch -----------------------------------------------

static void test_maintain() {
    std::printf("maintain dispatch\n");

    // 4a. AUTHENTICATED + expired -> refresh, stays AUTHENTICATED.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/100);
        CHECK(sched.isAuthenticated());

        ntp.set(1000000);                                // token now expired
        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"AT_NEW\",\"expires_in\":3600}");
        sched.maintain();
        CHECK(sched.isAuthenticated());
        CHECK(!sched.hasFailed());
        CHECK(mockHttpCursor() == 1);                    // a refresh happened
        CHECK(sched.expiration() ==
              1000000 + 3600 - GoogleSchedular::EXPIRATION_TIME_MARGIN);
    }

    // 4b. AUTHENTICATED + expired, refresh fails -> ERROR.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/100);

        ntp.set(1000000);
        mockHttpReset();
        mockHttpPush(401, "{\"error\":\"invalid_token\"}");
        sched.maintain();
        CHECK(sched.hasFailed());
    }

    // 4c. AUTHENTICATED but not expired -> maintain() does nothing.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        const unsigned long before = sched.expiration();

        ntp.set(2100);                                   // still well before expiry
        mockHttpReset();
        sched.maintain();
        CHECK(mockHttpCursor() == 0);                    // no request issued
        CHECK(sched.isAuthenticated());
        CHECK(sched.expiration() == before);
    }

    // 4d. INIT -> maintain() polls (delegates to handleRegistration).
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        mockHttpReset();
        mockHttpPush(200, "{\"verification_url\":\"u\",\"user_code\":\"c\","
                          "\"interval\":5,\"device_code\":\"D\"}");
        String u, c; sched.startRegistration(u, c);

        ntp.set(2000);
        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"AT\",\"refresh_token\":\"RT\","
                          "\"expires_in\":3600}");
        sched.maintain();
        CHECK(sched.isAuthenticated());                  // poll completed via maintain
    }

    // 4e. Bootstrap: only a refresh_token preset, no access_token yet.
    //     maintain() forces a refresh and promotes to AUTHENTICATED.
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        sched.setRefreshToken(String("A_PRESET_REFRESH_TOKEN"));   // length > 8
        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"BOOTSTRAPPED\",\"expires_in\":3600}");
        sched.maintain();
        CHECK(sched.isAuthenticated());
        CHECK(!sched.hasFailed());
    }

    // 4f. Bootstrap with a rejected refresh_token (400 invalid_grant) -> ERROR.
    //     The token is KEPT and isAuthInvalid() is set; since the credential is
    //     dead, maintain() does NOT keep retrying it (sticky) -- only a re-pair
    //     with a fresh token recovers.
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        sched.setRefreshToken(String("A_BAD_REFRESH_TOKEN"));
        mockHttpReset();
        mockHttpPush(400, "{\"error\":\"invalid_grant\"}");
        sched.maintain();
        CHECK(sched.hasFailed());
        CHECK(sched.isAuthInvalid());
        CHECK_STR(sched.getRefreshToken().c_str(), "A_BAD_REFRESH_TOKEN");   // kept

        // A later maintain() issues NO request: the dead token is not retried.
        mockHttpReset();
        sched.maintain();
        CHECK(mockHttpCursor() == 0);
        CHECK(sched.isAuthInvalid());
    }
}


// --- 5. setCalendar -------------------------------------------------------

static void test_set_calendar() {
    std::printf("setCalendar\n");

    // 5a. A matching summary -> LINKED with the matching id chosen.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);

        mockHttpReset();
        mockHttpPush(200, "{\"items\":["
                          "{\"id\":\"home@group\",\"summary\":\"Home\"},"
                          "{\"id\":\"work@group\",\"summary\":\"Work\"},"
                          "{\"id\":\"gym@group\",\"summary\":\"Gym\"}]}");
        sched.setCalendar(String("Work"));
        CHECK(sched.isLinked());
        CHECK(sched.state() == GoogleSchedular::LINKED);
        CHECK_STR(sched.calendarIdRaw().c_str(), "work@group");
    }

    // 5b. No matching summary -> stays AUTHENTICATED (not LINKED).
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);

        mockHttpReset();
        mockHttpPush(200, "{\"items\":["
                          "{\"id\":\"home@group\",\"summary\":\"Home\"},"
                          "{\"id\":\"gym@group\",\"summary\":\"Gym\"}]}");
        sched.setCalendar(String("Office"));
        CHECK(!sched.isLinked());
        CHECK(sched.isAuthenticated());
        CHECK(sched.state() == GoogleSchedular::AUTHENTICATED);
    }

    // 5c. A network/parse failure while listing calendars -> ERROR
    //     (distinct from "calendar not found", which stays AUTHENTICATED).
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);

        mockHttpReset();
        mockHttpPush(503, "{\"items\":[]}");
        sched.setCalendar(String("Whatever"));
        CHECK(sched.hasFailed());
        CHECK(sched.state() == GoogleSchedular::ERROR);
    }
}


// --- 6. syncAt ------------------------------------------------------------

static void test_sync_at() {
    std::printf("syncAt (event list + time window)\n");

    // 6a. const char* overload: two events, order preserved; the request URI
    //     widens the seconds-units digit to build the surrounding 10 s bucket.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        // Link to a known calendar id so the URI is predictable.
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"primary-cal\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));
        CHECK(sched.isLinked());

        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"summary\":\"P1\"},{\"summary\":\"P2\"}]}");
        CHECK(sched.syncAt("2024-11-04T07:30:15Z"));   // returns true on success

        std::list<String> events = sched.getEventList();
        CHECK(events.size() == 2);
        std::list<String>::const_iterator it = events.begin();
        CHECK_STR(it->c_str(), "P1"); ++it;
        CHECK_STR(it->c_str(), "P2");

        // Inspect the URI the mock HTTPClient received. Index 18 of the RFC3339
        // timestamp is the seconds-units digit: timeMin forces it to '0' and
        // timeMax to '9', so the window is [..:10Z, ..:19Z].
        const String& uri = mockHttpUris().back();
        const char* u = uri.c_str();
        CHECK(std::strstr(u, "/calendar/v3/calendars/primary-cal/events") != nullptr);
        CHECK(std::strstr(u, "fields=items(summary)") != nullptr);
        CHECK(std::strstr(u, "singleEvents=true") != nullptr);
        CHECK(std::strstr(u, "timeMin=2024-11-04T07:30:10Z") != nullptr);
        CHECK(std::strstr(u, "timeMax=2024-11-04T07:30:19Z") != nullptr);
    }

    // 6b. const String& overload delegates to the same logic.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));

        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"summary\":\"OnlyEvent\"}]}");
        String ts("2024-11-04T07:30:15Z");
        sched.syncAt(ts);

        std::list<String> events = sched.getEventList();
        CHECK(events.size() == 1);
        CHECK_STR(events.front().c_str(), "OnlyEvent");
        // Same widened window from the String overload.
        const char* u = mockHttpUris().back().c_str();
        CHECK(std::strstr(u, "timeMin=2024-11-04T07:30:10Z") != nullptr);
        CHECK(std::strstr(u, "timeMax=2024-11-04T07:30:19Z") != nullptr);
    }

    // 6c. An empty items array yields an empty (cleared) event list, no error.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));

        mockHttpReset();
        mockHttpPush(200, "{\"items\":[]}");
        sched.syncAt("2024-11-04T07:30:15Z");
        CHECK(sched.getEventList().empty());
        CHECK(!sched.hasFailed());
    }

    // 6d. HTTP failure on the events request -> ERROR.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));

        mockHttpReset();
        mockHttpPush(503, "{\"items\":[]}");
        CHECK(!sched.syncAt("2024-11-04T07:30:15Z"));   // returns false on failure
        CHECK(sched.hasFailed());
    }

    // 6e. Preconditions return false, leave state untouched, issue no request.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);

        // AUTHENTICATED but not LINKED yet -> no-op, no HTTP call.
        mockHttpReset();
        CHECK(!sched.syncAt("2024-11-04T07:30:15Z"));
        CHECK(sched.state() == GoogleSchedular::AUTHENTICATED);
        CHECK(mockHttpCursor() == 0);

        // Linked, but a null timestamp -> still false, state preserved, no call.
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));
        CHECK(sched.isLinked());
        mockHttpReset();
        CHECK(!sched.syncAt(static_cast<const char*>(nullptr)));
        CHECK(sched.state() == GoogleSchedular::LINKED);
        CHECK(mockHttpCursor() == 0);
    }
}


// --- 7. malformed body on 200 --------------------------------------------

static void test_malformed_body() {
    std::printf("malformed body on 200 -> ERROR\n");

    // A 200 whose body is invalid JSON is demoted to a failure in
    // _getRequest/_postJsonRequest (httpCode forced to 0), which syncAt surfaces
    // as ERROR.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));

        mockHttpReset();
        mockHttpPush(200, "{ this is not valid json ]]");
        sched.syncAt("2024-11-04T07:30:15Z");
        CHECK(sched.hasFailed());
    }

    // Same demotion on the OAuth POST path: a 200 device-code reply with a
    // broken body must NOT reach INIT (requestDeviceAndUserCode returns ERROR,
    // so startRegistration leaves the state at VOID).
    {
        FakeNtp ntp; ntp.set(1000);
        TestSchedular sched(String("i"), String("s"), &ntp);
        mockHttpReset();
        mockHttpPush(200, "not json at all");
        String u, c;
        sched.startRegistration(u, c);
        CHECK(sched.state() == GoogleSchedular::VOID);
        CHECK(!sched.isInitialized());
    }
}


// --- 8. isValidTimestamp --------------------------------------------------

static void test_is_valid_timestamp() {
    std::printf("isValidTimestamp\n");

    // Canonical, well-formed instants.
    CHECK(GoogleSchedular::isValidTimestamp("2024-11-04T07:30:15Z"));
    CHECK(GoogleSchedular::isValidTimestamp("2099-12-31T23:59:59Z"));

    // Null / empty / too short.
    CHECK(!GoogleSchedular::isValidTimestamp(static_cast<const char*>(nullptr)));
    CHECK(!GoogleSchedular::isValidTimestamp(""));
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04T07:30:1"));   // 18 chars

    // Longer than 20 (trailing junk).
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04T07:30:15Z "));
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04T07:30:15ZZ"));

    // Missing trailing 'Z'.
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04T07:30:150"));

    // Wrong separators.
    CHECK(!GoogleSchedular::isValidTimestamp("2024/11/04T07:30:15Z"));
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04 07:30:15Z"));  // space, not 'T'

    // Non-digit in a digit slot.
    CHECK(!GoogleSchedular::isValidTimestamp("20X4-11-04T07:30:15Z"));
    CHECK(!GoogleSchedular::isValidTimestamp("2024-11-04T07:3X:15Z"));
}


// --- 9. auth failure cause (isAuthInvalid, token kept) --------------------

static void test_auth_failure_cause() {
    std::printf("auth failure cause (isAuthInvalid, token kept)\n");

    const char* TOKEN = "A_LONG_REFRESH_TOKEN_1234567890";

    // 9a. HTTP 400 invalid_grant on bootstrap -> ERROR, isAuthInvalid, token kept.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        sched.setRefreshToken(String(TOKEN));   // access_token stays empty
        mockHttpReset();
        mockHttpPush(400, "{\"error\":\"invalid_grant\"}");
        sched.maintain();
        CHECK(sched.hasFailed());
        CHECK(sched.isAuthInvalid());
        CHECK(sched.lastAuthHttpCode() == 400);
        CHECK_STR(sched.getRefreshToken().c_str(), TOKEN);   // NOT wiped
    }

    // 9b. Transient failure (503) -> ERROR but NOT invalid, token kept.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        sched.setRefreshToken(String(TOKEN));
        mockHttpReset();
        mockHttpPush(503, "{}");
        sched.maintain();
        CHECK(sched.hasFailed());
        CHECK(!sched.isAuthInvalid());
        CHECK_STR(sched.getRefreshToken().c_str(), TOKEN);   // still kept
    }

    // 9c. Successful bootstrap -> AUTHENTICATED, token kept, not invalid.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        sched.setRefreshToken(String(TOKEN));
        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"AT\",\"expires_in\":3600}");
        sched.maintain();
        CHECK(sched.isAuthenticated());
        CHECK(!sched.hasFailed());
        CHECK(!sched.isAuthInvalid());
        CHECK_STR(sched.getRefreshToken().c_str(), TOKEN);
    }

    // 9d. Steady-state transient failure self-heals: LINKED, the token expires,
    //     a refresh fails on a network blip (503) -> ERROR (not invalid); when
    //     the network returns maintain() recovers WITHOUT re-registration. This
    //     is the long-outage case -- an outage never yields invalid_grant.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));
        CHECK(sched.isLinked());

        ntp.set(100000);                     // well past the token expiry
        mockHttpReset();
        mockHttpPush(503, "{}");             // transient refresh failure
        sched.maintain();
        CHECK(sched.hasFailed());
        CHECK(!sched.isAuthInvalid());

        mockHttpReset();
        mockHttpPush(200, "{\"access_token\":\"AT2\",\"expires_in\":3600}");
        sched.maintain();                    // network back
        CHECK(sched.isAuthenticated());
        CHECK(!sched.hasFailed());
    }

    // 9e. Steady-state invalid_grant is sticky: LINKED, a refresh returns 400 ->
    //     ERROR + isAuthInvalid, and maintain() does not retry the dead token.
    {
        FakeNtp ntp;
        TestSchedular sched(String("i"), String("s"), &ntp);
        driveToAuthenticated(sched, ntp, /*now=*/2000, /*expiresIn=*/3600);
        mockHttpReset();
        mockHttpPush(200, "{\"items\":[{\"id\":\"c\",\"summary\":\"Cal\"}]}");
        sched.setCalendar(String("Cal"));
        CHECK(sched.isLinked());

        ntp.set(100000);
        mockHttpReset();
        mockHttpPush(400, "{\"error\":\"invalid_grant\"}");
        sched.maintain();
        CHECK(sched.hasFailed());
        CHECK(sched.isAuthInvalid());

        mockHttpReset();
        sched.maintain();                    // dead token: no retry issued
        CHECK(mockHttpCursor() == 0);
        CHECK(sched.isAuthInvalid());
    }
}


int main() {
    test_state_predicates();
    test_start_registration();
    test_handle_registration();
    test_maintain();
    test_set_calendar();
    test_sync_at();
    test_malformed_body();
    test_is_valid_timestamp();
    test_auth_failure_cause();

    if (g_failures == 0) {
        std::printf("OK - all tests passed\n");
        return 0;
    }
    std::printf("FAILED - %d check(s)\n", g_failures);
    return 1;
}
