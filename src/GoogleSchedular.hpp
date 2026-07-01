#pragma once

// https://developers.google.com/calendar/api/guides/quota
// https://cloud.google.com/api-keys/docs/quotas


#include <Arduino.h>
#include <Udp.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <WiFiClientSecureBearSSL.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#endif
#include <list>

#include <ArduinoJson.h>
#include <TimestampNtp.hpp>


#include "GoogleOAuth2.hpp"
#include "GoogleApiCalendar.hpp"


//#ifndef SCHEDULAR_NAME_SEPARATOR
//#define SCHEDULAR_NAME_SEPARATOR ','
//#endif


/**
 * Use a Google Calendar as a scheduler for an Arduino / ESP project.
 *
 * The name is a portmanteau of Scheduler + Calendar: "Schedular". It is the
 * intended spelling of the library and its public API.
 *
 * GoogleSchedular is the top layer: it drives the OAuth2 device flow to
 * completion, resolves a calendar by name, and exposes the titles of the events
 * currently running. It is designed for microcontrollers, so it favours a small
 * and predictable footprint over defensive generality. The notable lightweight
 * choices are:
 *
 *  - A single 4-bit state (see State / the CADE bitmask) encodes the whole
 *    lifecycle, so the queries in isAuthenticated()/isLinked()/... are plain bit
 *    tests instead of extra flags.
 *  - Members are reused for more than one purpose when their lifetimes do not
 *    overlap: _calendarId temporarily holds the OAuth polling interval during
 *    registration (before any calendar is known), and _refreshToken holds the
 *    device_code (see GoogleOAuth2). This keeps the object small.
 *  - Timestamps are mutated in place rather than copied where possible
 *    (see syncAt), to avoid transient String allocations on the heap.
 *
 * Time comes from an injected NTP source (Ntp*), used both to time-box the
 * requests and to know when the access_token has to be refreshed.
 */
class GoogleSchedular : public GoogleApiCalendar {

    public:

    /*
    State is a 4-bit CADE bitmask, one bit per acquired credential/condition:
    - C: Calendar   => a calendar.id has been resolved
    - A: Authorized => an access_token is held
    - D: Device     => a device_code is held (registration in progress)
    - E: Error      => the last operation failed
    The named states below are the only valid bit combinations; helpers such as
    isAuthenticated() test a single bit so intermediate states still match.
    */
    enum State {
        VOID          = 0b0000,
        INIT          = 0b0010,
        AUTHENTICATED = 0b0110,
        LINKED        = 0b1110,
        ERROR         = 0b0001,
    };

    // Seconds subtracted from the token lifetime when scheduling a refresh, so
    // the access_token is renewed slightly before Google actually expires it and
    // an in-flight request never fails on a just-expired token.
    static constexpr uint8_t EXPIRATION_TIME_MARGIN = 64;


    // Init list ordered to match member declaration order below (avoids -Wreorder).
    GoogleSchedular(const String& clientId, const String& clientSecret, Ntp* ntp) : GoogleApiCalendar(clientId, clientSecret), _state(State::VOID), _ntp(ntp), _expirationTimestamp(0), _eventList() {}

    // Lifecycle predicates, all cheap bit tests on the CADE state.
    bool hasFailed(void) const       { return _state == State::ERROR; }
    bool isInitialized(void) const   { return _state == State::INIT;  }
    bool isAuthenticated(void) const { return _state & 0b0100; }
    bool isLinked(void) const        { return _state == State::LINKED; }

    // True when the current failure is Google rejecting the credential itself:
    // the token endpoint answered HTTP 400 (invalid_grant) => the refresh_token
    // is dead (revoked/expired) and only re-registration fixes it. A transient
    // failure (no network, DNS, 5xx => negative code / 5xx) leaves this false, so
    // the sketch can retry with backoff instead of forcing a re-pair.
    bool isAuthInvalid(void) const   { return _state == State::ERROR && lastAuthHttpCode() == 400; }

    // Titles of the events matched by the last syncAt() call, oldest first.
    std::list<String>getEventList(void) const { return _eventList; }


    bool hasExpired(void)
    {
        return _expirationTimestamp < _ntp->time();
    }

    // Resolves a calendar by its display name (summary) and stores its id.
    // Requires an authenticated session. On a network/parse failure the state
    // goes to ERROR (so the caller can tell "request failed" from "calendar not
    // found"); on success it moves to LINKED if the name matches, otherwise
    // stays AUTHENTICATED. The comparison is done while streaming the
    // (id, summary) list, so only the matched id is kept.
    void setCalendar(String calendarName)
    {
        if (_state & State::AUTHENTICATED) {
            JsonDocument doc;
            const GoogleOAuth2::Response ret = getCalendars(doc);

            if (ret != GoogleOAuth2::OK) {
                _state = State::ERROR;
                return;
            }

            _state = State::AUTHENTICATED;

            const JsonArray items = doc[F("items")].as<JsonArray>();
            for (JsonObject item : items) {
                if (calendarName.equals(item[F("summary")].as<String>())) {
                    _calendarId = item[F("id")].as<String>();
                    _state = State::LINKED;
                    break;
                }
            }
        }
    }

    // Non-blocking variant of startRegistration() for callers that do not want
    // to print the URL/code immediately. The user_code is parked in _accessToken
    // (unused at this stage) and the state is forced back to VOID so nothing runs
    // until getQuietUserCode() is polled. Reusing _accessToken avoids a dedicated
    // buffer for a value that is only read once.
    void startQuietRegistration()
    {
        String url;
        startRegistration(url, _accessToken);
        _state = State::VOID;
    }

    // Returns the pending user_code once (arming registration), then "" on later
    // calls. Pairs with startQuietRegistration().
    String getQuietUserCode(void) {
        if (_state == State::VOID) {
            _state = State::INIT;
            return _accessToken;
        }

        return "";
    }

    // Starts the OAuth2 device flow. On success `url` and `code` are what the
    // user must open and type; the device then polls until validated.
    // The polling interval returned by Google is stashed in _calendarId (no
    // calendar is known yet at this point) and read back in handleRegistration();
    // this reuse keeps the object free of a dedicated interval member.
    void startRegistration(String& url, String& code)
    {
        _state = State::VOID;

        const String scope = GoogleApiCalendar::scope();
        JsonDocument doc;
        const GoogleOAuth2::Response ret = requestDeviceAndUserCode(doc, scope);

        if (ret == GoogleOAuth2::OK) {
            url  = doc[F("verification_url")].as<String>();
            code = doc[F("user_code")].as<String>();

            const uint8_t interval = doc[F("interval")];
            _calendarId = String(interval);

            _setExpirationTimestamp(interval);
            _state = State::INIT;
        }
    }

    // Single entry point to keep the session healthy; call it periodically from
    // loop(). It dispatches on the current state: refresh the token when
    // authenticated, keep polling while registering, or bootstrap from a
    // pre-seeded refresh_token. Doing everything from one small method keeps the
    // caller's loop light and the control flow in one place.
    void maintain(void)
    {
        // A transient failure (ERROR that is NOT a rejected credential) is
        // recoverable from the refresh_token: drop the stale access_token so the
        // branch below re-authorizes and self-heals. This covers both a refresh
        // that failed on a network blip and a syncAt() that hit one, however long
        // the outage lasts -- a network outage never yields invalid_grant, only
        // negative/5xx codes, so isAuthInvalid() stays false and we keep retrying.
        // A genuine invalid_grant (400) is left sticky for the sketch to re-pair.
        if (hasFailed() && !isAuthInvalid() && _refreshToken.length() > 8) {
            _accessToken = "";
        }

        // already authenticated
        if (isAuthenticated()) {
            // renew authentication if needed
            maintainAuthorization(false);
        // waiting for a device limited validation
        } else if (isInitialized()) {
            // authenticate depending the remote server
            handleRegistration();
        // no running authentication process (prevent "null" value)
        } else if (_refreshToken.length()>8) {
            if (_accessToken.isEmpty() && !isAuthInvalid()) {
                // Bootstrap / recover from the refresh_token: fetch an access_token.
                maintainAuthorization(true);
                // Success is signalled by a now-present access_token (this also
                // recovers from a prior transient ERROR). On failure the token is
                // KEPT (the sketch owns its lifecycle, e.g. persisted in flash)
                // and the state stays ERROR; the sketch reads isAuthInvalid() to
                // tell a dead credential (must re-register) from a transient
                // failure (retry), and owns the retry cadence.
                if (!_accessToken.isEmpty()) {
                    _state = State::AUTHENTICATED;
                }
            } else {
                // access_token present (startQuietRegistration), or the
                // credential was rejected (invalid_grant) -> nothing to do here.
            }
        }
    }


    // Polls Google once the current interval has elapsed while the device waits
    // for the user to validate the code. PENDING re-arms the timer using the
    // interval stashed in _calendarId; OK promotes to AUTHENTICATED; anything
    // else fails the session. Reusing _expirationTimestamp as the poll timer
    // avoids a second timing member.
    void handleRegistration(void)
    {
        if (_expirationTimestamp < _ntp->time()) {
            JsonDocument doc;
            const GoogleOAuth2::Response ret = pollAuthorization(doc);

            switch (ret) {
                case GoogleOAuth2::PENDING: {
                    const uint8_t interval = _calendarId.toInt();
                    _setExpirationTimestamp(interval);
                    break;
                }
                case GoogleOAuth2::OK: {
                    const uint16_t expiresInSeconds = doc[F("expires_in")];
                    _setSecureExpirationTimestamp(expiresInSeconds);
                    _state = State::AUTHENTICATED;
                    break;
                }
                default: {
                    _setExpirationTimestamp(0);
                    _state = State::ERROR;
                }
            }
        }
    }

    // Refreshes the access_token when it is about to expire (or when forced).
    // The next refresh is scheduled with EXPIRATION_TIME_MARGIN of slack.
    void maintainAuthorization(const bool force=false)
    {
        if (force || hasExpired()) {
            JsonDocument doc;
            const GoogleOAuth2::Response ret = refreshAccessToken(doc);
            if (ret == GoogleOAuth2::OK) {
                const uint16_t expiresInSeconds = doc[F("expires_in")];
                _setSecureExpirationTimestamp(expiresInSeconds);
            } else {
                _state = State::ERROR;
            }
        }
    }

    // Developer aid: returns true iff `ts` is a well-formed RFC3339 UTC instant
    // of the exact shape this library expects: "YYYY-MM-DDThh:mm:ssZ" (20 chars,
    // 'Z' suffix, digits where digits belong). Handy to assert an NTP timestamp
    // while debugging before handing it to syncAt(). Never reads past ts's
    // terminator, so it is safe on any C-string (including nullptr / empty).
    static bool isValidTimestamp(const char* ts)
    {
        if (ts == nullptr) {
            return false;
        }
        // '0' marks a digit slot; every other position must match literally.
        const char* const layout = "0000-00-00T00:00:00Z";
        for (uint8_t i = 0; i < 20; ++i) {
            const char c = ts[i];
            if (c == '\0') {
                return false;           // shorter than 20 chars
            }
            if (layout[i] == '0') {
                if (c < '0' || c > '9') {
                    return false;       // expected a digit
                }
            } else if (c != layout[i]) {
                return false;           // expected a separator / 'Z'
            }
        }
        return ts[20] == '\0';          // and exactly 20 chars
    }

    // Refreshes getEventList() with the events active at RFC3339 timestamp `ts`
    // (e.g. "2024-11-04T07:30:15Z"), and returns whether the sync succeeded.
    // The window [timeMin, timeMax] is built on the stack, with NO heap
    // allocation: index 18 is the seconds units digit ("...:1[5]Z"); timeMin
    // forces it to '0' and timeMax to '9', a ~10 s bucket around the instant.
    //
    // `ts` is taken as const char* precisely so it can be fed straight from
    // TimestampNtp::c_str() (zero-copy) instead of an allocated String -- a
    // matter of the BOARD'S LONGEVITY, not raw speed: syncing every minute for
    // months, an allocating path would repeatedly malloc/free short Strings and
    // slowly fragment the small ESP heap until an allocation fails. Stack copies
    // keep the per-sync heap footprint constant (only the request URI is
    // allocated), so an always-on device stays stable over long uptimes.
    //
    // Preconditions (return false, state untouched): a calendar must be linked,
    // and ts must be non-null. For lightness ts is otherwise TRUSTED to be a
    // 20-char RFC3339 instant -- call isValidTimestamp(ts) yourself if unsure;
    // a shorter buffer is undefined. On a network/parse failure the state goes
    // to ERROR and it returns false; on success it returns true.
    bool syncAt(const char* ts)
    {
        if (!isLinked()) {
            return false;               // no calendar resolved yet
        }
        if (ts == nullptr) {
            return false;               // no timestamp
        }

        GoogleOAuth2::Response ret;
        JsonDocument doc;
        {
            // Copy into two stack buffers so the source (possibly the NTP
            // client's internal c_str() buffer) is never mutated.
            char t0[21];
            char t1[21];
            memcpy(t0, ts, 20); t0[20] = '\0';
            memcpy(t1, ts, 20); t1[20] = '\0';
            t0[18] = '0';
            t1[18] = '9';

            ret = getEvents(doc, _calendarId, t0, t1);
        }

        if (ret != GoogleOAuth2::OK) {
            _state = State::ERROR;
            return false;
        }

        _eventList.clear();

        const JsonArray items = doc[F("items")].as<JsonArray>();

        for (JsonObject item : items) {
            // Read the first (and, thanks to fields=items(summary), only)
            // member of each item by iterator instead of by the "summary"
            // key. This skips a key lookup / string compare per event.
            // !!! only valid because the query masks fields to items(summary) !!!
            const String summary = item.begin()->value().as<String>();
            _eventList.push_back(summary);
        }
        return true;
    }

    // Backward-compatible overload for String callers. Prefer the const char*
    // form fed by TimestampNtp::c_str() to avoid the extra String allocation.
    bool syncAt(const String& ts) { return syncAt(ts.c_str()); }


    protected:

    // Arm _expirationTimestamp exactly `expiresInSeconds` from now. Used for
    // short-lived, non-token deadlines such as the registration poll interval.
    void _setExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        _expirationTimestamp = _ntp->time() + expiresInSeconds;
    }

    // Same, minus EXPIRATION_TIME_MARGIN of safety slack. Used for the
    // access_token so it is refreshed before Google's real expiry.
    void _setSecureExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        _expirationTimestamp = _ntp->time() + expiresInSeconds - GoogleSchedular::EXPIRATION_TIME_MARGIN;
    }

    // NOTE: _calendarId doubles as scratch storage for the OAuth polling
    // interval (as a String) until a real calendar id is set; _expirationTimestamp
    // doubles as both the poll-interval timer and the token-refresh deadline.
    // These overlaps are safe because the phases never run at the same time.
    State _state;
    String _calendarId;
    Ntp* _ntp = nullptr;
    unsigned long _expirationTimestamp;
    std::list<String> _eventList;

};
