/**
 * Permanent-install example.
 *
 * Persists the refresh_token so the board SILENTLY reconnects after a reboot
 * (no human re-pairing), retries transient failures, re-registers only when the
 * credential is actually rejected, and drives the output to a safe state while
 * offline.
 *
 * Separation of concerns: the library provides the MECHANISM
 * (setRefreshToken / getRefreshToken, maintain, hasFailed / isAuthInvalid,
 * syncAt). This sketch owns the POLICY: where the token is stored, when to
 * retry, when to re-register, and what the actuator does when offline.
 *
 * Storage here is LittleFS (ESP8266). On ESP32 swap the three helpers below for
 * Preferences / NVS:
 *     Preferences prefs;                       // global
 *     prefs.begin("gcal", false);              // in setup()
 *     prefs.getString("rt", "");               // loadToken()
 *     prefs.putString("rt", t);                // saveToken()
 *     prefs.remove("rt");                      // clearToken()
 * Both live in flash; a refresh_token changes ~never, so this is write-once and
 * flash wear is negligible. Never persist the access_token (it is ephemeral).
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>

#include <FastTimer.hpp>
#include <TimestampNtp.hpp>
#include <GoogleSchedular.hpp>


#define STASSID "**** SSID ****"
#define STAPSK  "***password***"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

const String GOOGLE_API_CLIENT_ID     = "**** CLIENT_ID ****";
const String GOOGLE_API_CLIENT_SECRET = "**** CLIENT_SECRET ****";

const unsigned int LOCAL_PORT   = 3669;
const char*        NTP_HOST     = "2.europe.pool.ntp.org";
const String       CALENDAR_NAME = "ArduinoRelay";

const char* TOKEN_PATH = "/gcal_rt";   // one small file holding the refresh_token


TimestampNtp<WiFiUDP> ntp;
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, &ntp);
ShortTimer8<ShortTimerPrecision::P_minutes> timer1mn;


// --- token persistence (the swappable part) -------------------------------

String loadToken()
{
    String t;
    File f = LittleFS.open(TOKEN_PATH, "r");
    if (f) {
        t = f.readString();
        f.close();
    }
    return t;
}

// Write only when the value actually changed: since the refresh_token is stable
// this is effectively write-once, so flash wear stays negligible.
void saveToken(const String& t)
{
    if (t.length() == 0 || t == loadToken()) {
        return;
    }
    File f = LittleFS.open(TOKEN_PATH, "w");
    if (f) {
        f.print(t);
        f.close();
    }
}

void clearToken()
{
    LittleFS.remove(TOKEN_PATH);
}


// --- helpers --------------------------------------------------------------

void safeOutput()
{
    // Offline / failed: drive the actuator to a known-safe state.
    // LED_BUILTIN is active-low here, so HIGH == off.
    digitalWrite(LED_BUILTIN, HIGH);
}

void syncTime()
{
    ntp.request(NTP_HOST);
    const unsigned long t0 = millis();
    while (!ntp.listenSync() && (millis() - t0) < 3000) {
        delay(10);
    }
}

// Interactive device-flow pairing. Runs on first install, or after the stored
// token was rejected. Blocks (a human is present) until authenticated.
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

    Serial.begin(9600);
    while (!Serial) {
        delay(5);
    }

    if (!LittleFS.begin()) {
        LittleFS.format();
        LittleFS.begin();
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
            // is back. Nothing to do but wait (this 1-minute loop is the retry
            // interval; widen it or add backoff to cut traffic on long outages).
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
        digitalWrite(LED_BUILTIN, active ? LOW : HIGH);   // active-low: LOW == on
        for (String e : gs.getEventList()) {
            Serial.println("- " + e);
        }
    } else {
        safeOutput();
    }
}
