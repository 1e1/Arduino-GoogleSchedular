/**
  * Serial output:
  * ```
  * 08:30:12.635 -> ....WiFi connected
  * 08:30:15.437 -> 2024-11-04T07:30:15Z
  * 08:30:15.437 -> +-- GOOGLE ---
  * 08:30:15.463 -> |- starting registration
  * 08:30:16.552 -> | |- URL : https://www.google.com/device
  * 08:30:16.586 -> | +- CODE: ABC-DEF-GHI
  * 08:30:16.623 -> |- waiting for validation.......
  * 08:30:52.577 -> +- CONNECTED
  * 08:30:52.577 -> |- getting calendar 'ArduinoRelay'
  * 08:30:53.802 -> +- DONE
  * 08:30:53.802 -> |- load events
  * 08:30:53.994 -> |- P2
  * 08:30:53.994 -> |- P1
  * 08:30:53.994 -> +-----------
  * 08:30:53.994 -> *** START ***
  * 08:31:11.085 -> -- 2024-11-04T07:30:48Z
  * 08:31:11.387 -> - P2
  * 08:31:11.387 -> - P1
  * 08:31:11.387 -> ----------
  * 08:31:11.387 -> [HW] Free heap: 19480 bytes
  * 08:31:11.453 -> ----------
  * 08:32:11.059 -> -- 2024-11-04T07:31:13Z
  * 08:32:11.479 -> - P2
  * 08:32:11.479 -> - P1
  * 08:32:11.479 -> ----------
  * 08:32:11.479 -> [HW] Free heap: 19480 bytes
  * 08:32:11.511 -> ----------
  * 08:33:10.998 -> -- 2024-11-04T07:32:14Z
  * 08:33:11.194 -> - P2
  * 08:33:11.194 -> - P1
  * 08:33:11.194 -> ----------
  * 08:33:11.194 -> [HW] Free heap: 19672 bytes
  * 08:34:11.309 -> ----------
  * 08:35:11.084 -> -- 2024-11-04T07:34:16Z
  * 08:35:11.180 -> ----------
  * 08:35:11.180 -> [HW] Free heap: 19720 bytes
  * 08:35:11.220 -> ----------
  * 08:36:11.055 -> -- 2024-11-04T07:35:17Z
  * 08:36:11.222 -> ----------
  * 08:36:11.222 -> [HW] Free heap: 19720 bytes
  * ```
  */



#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#include <FastTimer.hpp>
#include <TimestampNtp.hpp>

#include <GoogleSchedular.hpp>


#define STASSID "**** SSID ****"
#define STAPSK  "***password***"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif


// see https://developers.google.com/identity/protocols/oauth2/limited-input-device
const String GOOGLE_API_CLIENT_ID = "**** CLIENT_ID ****";
const String GOOGLE_API_CLIENT_SECRET = "**** CLIENT_SECRET ****";


const unsigned int LOCAL_PORT = 3669;
const char* NTP_HOST = "2.europe.pool.ntp.org";
const String CALENDAR_NAME = "ArduinoRelay";


WiFiUDP udp;
TimestampRFC3339Ntp ntp(udp);
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, ntp);


ShortTimer8<ShortTimerPrecision::P_minutes> timer1mn;


void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); 

    // start Serial
    {
        Serial.begin(9600);
        while (!Serial) {
            delay(5);
        }
        Serial.flush();
        Serial.println("Serial OK");
    }

    // start WiFi
    {
        WiFi.begin(STASSID, STAPSK);
        while (WiFi.status() != WL_CONNECTED) {
            Serial.print('.');
            delay(500);
        }
        Serial.println("WiFi connected");
    }

    // start UDP
    {
        udp.begin(LOCAL_PORT);
        ntp.request(NTP_HOST);
        do {
            delay(10);
        } while(!ntp.listenSync());
        String ts = ntp.getTimestampRFC3339();
        Serial.println(ts);
    }
    
    Serial.println("+-- GOOGLE ---");
    {
        Serial.println("|- starting registration");
        {
            String url;
            String code; 
            gs.startRegistration(url, code);

            if (!gs.isInitialized()) {
                Serial.println("*** CRASH startRegistration() ***");
                crash();
            }

            Serial.print("| |- URL : "); Serial.println(url);
            Serial.print("| +- CODE: "); Serial.println(code);
            Serial.flush();
        }
        Serial.print("|- waiting for validation");
        {
            uint8_t line_size = 25;
            FastTimer<FastTimerPrecision::P_1s_4m> timer1s;
            do {
                timer1s.update();
                if (timer1s.isTickBy64()) {
                    ntp.request(NTP_HOST);
                    if (++line_size >= 80) {
                        Serial.println();
                        line_size = 0;
                    }
                    Serial.print('.');
                }

                delay(5000);

                ntp.listen();
                gs.maintain();
            } while(gs.isInitialized());
            Serial.println();

            if (!gs.isAuthenticated()) {
                Serial.println("*** CRASH handleRegistration() ***");
                crash();
            }
            Serial.println("+- CONNECTED");
            Serial.flush();
        }
        Serial.print("|- getting calendar '");
        Serial.print(CALENDAR_NAME);
        Serial.println("'");
        {
            gs.setCalendar(CALENDAR_NAME);
            if (!gs.isLinked()) {
                Serial.println("*** CRASH setCalendar() ***");
                crash();
            }

            Serial.println("+- DONE");
            Serial.flush();
        }
        Serial.println("|- load events");
        {
            ntp.syncRFC3339();
            String ts = ntp.getTimestampRFC3339();
            gs.syncAt(ts);
            for(String e : gs.getEventList()) {
                Serial.println("|- " + e);
            }
            Serial.println("+-----------");
        }

    }

    Serial.println("*** START ***");
    Serial.flush();
}

void loop()
{
    if (timer1mn.hasChanged()) {
        gs.maintain();
        ntp.request(NTP_HOST);

        digitalWrite(LED_BUILTIN, LOW);

        ntp.syncRFC3339(timer1mn.getElapsedTime());
        String ts = ntp.getTimestampRFC3339();

        Serial.print("-- ");
        Serial.println(ts);
        gs.syncAt(ts);
        for(String e : gs.getEventList()) {
            Serial.println("- " + e);
        }

        Serial.println("----------");
        Serial.printf("[HW] Free heap: %d bytes\n", ESP.getFreeHeap());
        Serial.println("----------");
        Serial.flush();

        if (gs.hasFailed()) {
            Serial.println("*** CRASH ***");
            crash();
        }
    } else {
        delay(100);
    }

    ntp.listen();
}

void crash()
{
    Serial.println("*** CRASH ***");
    
    bool isLedOn; 
    while (true) {
        isLedOn = !isLedOn;
        if (isLedOn) {
            digitalWrite(LED_BUILTIN, HIGH);
        } else {
            digitalWrite(LED_BUILTIN, LOW);
        }

        delay(666);
    }
}
