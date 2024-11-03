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
ShortTimer8<ShortTimer_precision_t::P_minutes> timer1mn;
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, ntp, CALENDAR_NAME);



void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH); 

    // Open serial communications and wait for port to open:
    Serial.begin(9600);
    while (!Serial) {
        ; // wait for serial port to connect. Needed for native USB port only
    }
    Serial.flush();


    WiFi.begin(STASSID, STAPSK);
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print('.');
        delay(500);
    }
    Serial.println();

    // start UDP
    udp.begin(LOCAL_PORT);
    
    Serial.println("+-- GOOGLE ---");
    {
        Serial.println("|- starting registration...");
        {
            String url;
            String code; 
            gs.startRegistration(url, code);

            if (!gs.hasAuthorization()) {
                Serial.println("*** CRASH startRegistration() ***");
                crash();
            }

            Serial.print("| |- URL : "); Serial.println(url);
            Serial.print("| +- CODE: "); Serial.println(code);
            Serial.flush();
        }
        Serial.println("|- waiting for validation...");
        {
            gs.validateRegistration();

            if (!gs.hasAuthorization()) {
                Serial.println("*** CRASH validateRegistration() ***");
                crash();
            }

            Serial.println("+- CONNECTED");
            Serial.flush();
        }

    }

    Serial.println("*** START ***");
    Serial.flush();
}

void loop()
{
    if (timer1mn.hasChanged()) {
        // ?.maintain()?
        ntp.request(NTP_HOST);
        if (!gs.maintainEveryMinute()) {
            Serial.println("*** CRASH refreshing access_token ***");
            crash();
        }

        digitalWrite(LED_BUILTIN, LOW);

        ntp.syncRFC3339(timer1mn.getElapsedTime());
        String ts = ntp.getTimestampRFC3339();

        Serial.print("-- ");
        Serial.println(ts);
        if (gs.syncAt(ts)) {
            for(String e : gs.getEventList()) {
                Serial.println("- " + e);
            }
        } else {
            Serial.println("| FAILED |");
        }

        Serial.println("----------");
        Serial.printf("[HW] Free heap: %d bytes\n", ESP.getFreeHeap());
        Serial.println("----------");
        Serial.flush();

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
