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


ShortTimer8<ShortTimer_precision_t::P_minutes> timer1mn;


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

            if (gs.getState() != GoogleSchedular::INIT) {
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
            FastTimer<FastTimer_precision_t::P_1s_4m> timer1s;
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
            } while(gs.getState() == GoogleSchedular::INIT);
            Serial.println();

            if (gs.getState() != GoogleSchedular::READY) {
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
            if (gs.getState() != GoogleSchedular::RUN) {
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

        if (gs.getState() == GoogleSchedular::ERROR) {
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
