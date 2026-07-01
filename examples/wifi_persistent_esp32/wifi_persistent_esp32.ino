/**
 * Permanent-install example for ESP32.
 *
 * Same robust flow as examples/wifi_persistent (silent reconnect on boot,
 * transient retry, re-pair only on a rejected credential, safe output state),
 * but the refresh_token is stored in NVS via Preferences -- the idiomatic ESP32
 * store, the same one the WiFi driver uses for its credentials.
 *
 * Mechanism vs policy: the library provides setRefreshToken / getRefreshToken,
 * maintain, hasFailed / isAuthInvalid, syncAt. This sketch owns the policy:
 * where the token is stored, when to retry, when to re-register, and what the
 * actuator does when offline.
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>

#include <FastTimer.hpp>
#include <TimestampNtp.hpp>
#include <GoogleSchedular.hpp>


#define STASSID "**** SSID ****"
#define STAPSK  "***password***"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
#define LED_ON  HIGH   // most ESP32 dev boards drive the onboard LED active-high
#define LED_OFF LOW

const String GOOGLE_API_CLIENT_ID     = "**** CLIENT_ID ****";
const String GOOGLE_API_CLIENT_SECRET = "**** CLIENT_SECRET ****";

const unsigned int LOCAL_PORT    = 3669;
const char*        NTP_HOST      = "2.europe.pool.ntp.org";
const String       CALENDAR_NAME = "ArduinoRelay";

const char* NVS_NAMESPACE = "gcal";
const char* NVS_KEY       = "rt";


TimestampNtp<WiFiUDP> ntp;
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, &ntp);
ShortTimer8<ShortTimerPrecision::P_minutes> timer1mn;
Preferences prefs;


// --- token persistence (NVS / Preferences) --------------------------------

String loadToken()
{
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
    const String t = prefs.getString(NVS_KEY, "");
    prefs.end();
    return t;
}

// Write only when the value actually changed: the refresh_token is stable, so
// this is effectively write-once and NVS wear stays negligible.
void saveToken(const String& t)
{
    if (t.length() == 0 || t == loadToken()) {
        return;
    }
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putString(NVS_KEY, t);
    prefs.end();
}

void clearToken()
{
    prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
    prefs.remove(NVS_KEY);
    prefs.end();
}


// --- helpers --------------------------------------------------------------

void safeOutput()
{
    digitalWrite(LED_BUILTIN, LED_OFF);   // offline / failed -> known-safe state
}

void syncTime()
{
    ntp.request(NTP_HOST);
    const unsigned long t0 = millis();
    while (!ntp.listenSync() && (millis() - t0) < 3000) {
        delay(10);
    }
}

// Interactive device-flow pairing: first install, or after the stored token was
// rejected. Blocks (a human is present) until authenticated.
void pair()
{
    String url, code;
    gs.startRegistration(url, code);
    Serial.print("Pair at "); Serial.print(url);
    Serial.print(" with code "); Serial.println(code);

    FastTimer<FastTimerPrecision::P_1s_4m> t;
    while (gs.isInitialized()) {
        t.update();
        if (t.isTickBy64()) {
            syncTime();
        }
        delay(2000);
        ntp.listen();
        gs.maintain();
    }
    saveToken(gs.getRefreshToken());
}


// --- setup / loop ---------------------------------------------------------

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    safeOutput();

    Serial.begin(115200);
    while (!Serial) {
        delay(5);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(STASSID, STAPSK);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    ntp.begin(LOCAL_PORT);
    syncTime();

    // Silent reconnect: reuse the stored refresh_token; pair only if none.
    const String rt = loadToken();
    if (rt.length()) {
        gs.setRefreshToken(rt);
    } else {
        pair();
    }
}

void loop()
{
    ntp.listen();

    if (!timer1mn.hasChanged()) {
        delay(100);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        safeOutput();
        return;                 // let the WiFi stack auto-reconnect
    }

    syncTime();
    gs.maintain();              // refreshes the token / bootstraps as needed

    if (gs.hasFailed()) {
        safeOutput();
        if (gs.isAuthInvalid()) {
            // The refresh_token is dead (revoked, or 7-day testing-mode expiry):
            // forget it and re-register. Needs a human, once.
            Serial.println("refresh_token rejected, re-registering");
            clearToken();
            pair();
        } else {
            // Transient (WiFi / DNS / 5xx), whatever the outage length: keep the
            // token; the library self-recovers on the next cycle once the network
            // is back. Nothing to do but wait.
            Serial.println("transient failure, will retry");
        }
        return;
    }

    // Rotation safety: persist only if the library obtained a new token.
    saveToken(gs.getRefreshToken());

    if (!gs.isLinked()) {
        gs.setCalendar(CALENDAR_NAME);
        if (!gs.isLinked()) {
            safeOutput();
            return;
        }
    }

    if (gs.syncAt(ntp.c_str())) {
        const bool active = !gs.getEventList().empty();
        digitalWrite(LED_BUILTIN, active ? LED_ON : LED_OFF);
        for (String e : gs.getEventList()) {
            Serial.println("- " + e);
        }
    } else {
        safeOutput();
    }
}
