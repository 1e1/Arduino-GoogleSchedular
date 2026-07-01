# Google Schedular

[![CI](https://github.com/1e1/Arduino-GoogleSchedular/actions/workflows/ci.yml/badge.svg)](https://github.com/1e1/Arduino-GoogleSchedular/actions/workflows/ci.yml)

Use your Google Calendar as a scheduler for your Arduino projects.
Get the summary(*) of the current events from the target calendar.

(*) a summary is like an event name/title/label. This is what you see as event name in your calendar.

> **Name:** *Schedular* is a portmanteau of **Scheduler** + **Calendar** — it is the intended spelling of the library and its API.

The library targets ESP8266 / ESP32 and is built to stay small: minimal RAM,
minimal flash, minimal network traffic. See [Design](#design) for the trade-offs
this implies.


## Setup

It requires Google OAuth2 credentials. 
https://developers.google.com/identity/protocols/oauth2/limited-input-device#prerequisites

- Get your Google Account
- Go to https://console.cloud.google.com/
- Create a Google Dev Project (e.g. "MyGoogleSchedular")
- Go to the Consent tab: keep the project in testing mode and manually add the Google accounts of your users (including yourself) as test users
- Go to the Credentials tab: create OAuth 2.0 Credentials for a device; you will get the `CLIENT_ID` and the `CLIENT_SECRET`

Official explanations: 
- Device OAuth
  https://developers.google.com/identity/protocols/oauth2/limited-input-device
  => GOOGLE_API_CLIENT_ID
  => GOOGLE_API_CLIENT_SECRET


## Run

As soon as you get the `verification_url` and `user_code`, go to this URL
(most probably https://www.google.com/device)
then authenticate with the given code and your Google Account. 


## Code

Create the Schedular with a NTP client for knowing the expiration times,
and the current datetime when requesting the Google Calendar. 

```
TimestampNtp<WiFiUDP> ntp;
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, &ntp);

// ...

ntp.begin();
```

Get user temporary code for merging the user account to this session.
```
String url;
String code; 
gs.startRegistration(url, code);

Serial.print("URL : "); Serial.println(url);
Serial.print("CODE: "); Serial.println(code);
```

Waiting the refresh_access / access_token from Google when the user is authenticated on Google. 
```
FastTimer<FastTimerPrecision::P_1s_4m> timer1s;
do {
    timer1s.update();
    if (timer1s.isTickBy64()) {
        ntp.request(NTP_HOST);
    } else {
        delay(100);
    }

    ntp.listen();
    gs.maintain();
} while(gs.isInitialized());
```

Getting the targeted calendar identifier from its name. 
```
gs.setCalendar(CALENDAR_NAME);
```

Generating the timestamp string, 
then request the current event titles/summaries/labels. 
Use `c_str()` for a zero-copy timestamp (no heap allocation): over long
uptimes this avoids fragmenting the small ESP heap with per-sync `String`s.
```
ntp.syncRFC3339();
const char* ts = ntp.c_str();
gs.syncAt(ts);

for(String e : gs.getEventList()) {
    Serial.println(e);
}
```

`syncAt()` returns `false` if it is called before a calendar is linked, on a
null timestamp, or on a network/parse failure (which also sets the error state).
For lightness it otherwise trusts `ts` to be a 20-char RFC3339 UTC instant; while
debugging you can assert it first with the static helper
`GoogleSchedular::isValidTimestamp(ts)`.

Don't forget to maintain the user session with `gs.maintain()`
Example: 
```
void loop()
{
    if (timer1mn.hasChanged()) {
        ntp.request(NTP_HOST);

        ntp.syncRFC3339(timer1mn.getElapsedTime());
        const char* ts = ntp.c_str();
        gs.syncAt(ts);

        Serial.print("-- ");
        Serial.println(ts);
        for(String e : gs.getEventList()) {
            Serial.println("- " + e);
        }
    } else {
        delay(100);
    }

    if (ntp.listen()) {
        gs.maintain();
    }
}
```


## Persisting the session (permanent installs)

By default the `refresh_token` lives only in RAM, so a reboot means re-pairing.
For an always-on device, save it once and reconnect silently on boot. The
library provides only the mechanism; the sketch owns the policy:

- **Store the `refresh_token` in flash, write-once.** It changes almost never, so
  flash wear is negligible. Use `Preferences`/NVS on ESP32, LittleFS or
  EEPROM-emulation on ESP8266. Never persist the `access_token` (it is ephemeral).
- **Boot:** `gs.setRefreshToken(stored)` then `gs.maintain()` gets a fresh
  access_token; if nothing is stored, run `startRegistration()` once.
- **On failure**, distinguish the cause: `gs.isAuthInvalid()` is true only when
  Google rejected the credential (HTTP 400 invalid_grant — revocation, or the
  7-day expiry of a "testing" app) — the token is dead, clear it and re-register.
  Otherwise the failure is transient (no network / DNS / 5xx, of *any* duration —
  an outage never yields invalid_grant): `maintain()` keeps the token and
  self-recovers on a later call, so the sketch just waits and retries.

Complete examples: `examples/wifi_persistent` (ESP8266, LittleFS) and
`examples/wifi_persistent_esp32` (ESP32, Preferences/NVS) — both do silent
reconnect + transient retry + re-pair on rejection + safe output state.


## Design

This library is deliberately lightweight. On a microcontroller with a few kB of
free heap, a small and predictable footprint matters more than defensive
generality, so several design choices trade robustness or genericity for size
and speed. They are intentional — this section documents them so they are not
mistaken for accidents.

**Network / payload**
- Every Calendar API call uses the `fields` mask to fetch the strict minimum
  (`items(id,summary)` for calendars, `items(summary)` for events). Smaller
  responses mean a smaller `JsonDocument` and less socket traffic.
- `singleEvents=true` lets Google expand recurring events server-side, so the
  device never has to compute occurrences itself.
- Responses are read in HTTP/1.0 mode and streamed straight from the socket into
  ArduinoJson — the full body is never buffered in a `String`.
- A single `HTTPClient` / `WiFiClientSecure` pair is reused for all requests and
  closed after each one, so only one connection is ever alive.

**TLS**
- `WiFiClientSecure::setInsecure()` is used on purpose: the peer certificate is
  not validated. Pinning a CA costs RAM/CPU and requires periodic root-certificate
  maintenance. Since the client only ever contacts Google endpoints over TLS,
  the library accepts unauthenticated-peer TLS as its lightweight default. If your
  threat model needs it, replace `setInsecure()` with certificate validation.

**Memory footprint**
- A single 4-bit state (the `CADE` bitmask: Calendar / Authorized / Device /
  Error) encodes the whole lifecycle; the `isAuthenticated()` / `isLinked()` /
  ... helpers are plain bit tests.
- Members are reused across non-overlapping phases instead of adding fields:
  `_refreshToken` temporarily holds the `device_code` during registration, and
  `_calendarId` temporarily holds the OAuth polling interval before any calendar
  is resolved.
- `syncAt(const char*)` builds the `[timeMin, timeMax]` window on the stack by
  flipping the seconds-units digit (index 18) to `'0'` / `'9'` — a ~10 s bucket
  around the instant — with no heap allocation. Fed from `TimestampNtp::c_str()`
  it keeps the per-sync heap footprint constant, which matters for a device that
  syncs every minute for months (heap-fragmentation avoidance = longevity).

**Trade-offs to be aware of**
- `getEventList()` returns the event list by value (a copy). It is convenient for
  the typical "iterate and print" usage; avoid calling it in a hot path.
- Errors are surfaced through the state machine (`hasFailed()`), not exceptions.


## Limitations

Visit: 
- https://developers.google.com/calendar/api/guides/quota
- https://cloud.google.com/api-keys/docs/quotas



## Dependencies

- **ArduinoJson** `>= 7.0.0` — the code uses the elastic `JsonDocument` API.
- **FastTimer** `>= 3.1.0` — https://github.com/1e1/Arduino-FastTimer
  It also bundles `TimestampNtp.hpp`, the NTP client this library relies on, so
  no separate timestamp dependency is needed. 3.1.0 brings a correct
  Gregorian date algorithm and a zero-copy `c_str()` timestamp accessor
  (allocation-free alternative to `getTimestampRFC3339()`).


## Infos

A concrete example on https://github.com/1e1/arduino-webcontroller-relay
