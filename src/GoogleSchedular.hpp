#pragma once

// https://developers.google.com/calendar/api/guides/quota
// https://cloud.google.com/api-keys/docs/quotas


#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <list>

#include <ArduinoJson.h>
#include <FastTimer.hpp>


#include "GoogleOAuth2.hpp"
#include "GoogleApiCalendar.hpp"


class GoogleSchedular : public GoogleApiCalendar {

    public:

    GoogleSchedular(const String& clientId, const String& clientSecret, const TimestampRFC3339Ntp &ts, const String& calendar) : GoogleApiCalendar(clientId, clientSecret), _calendar(calendar), _timestamp(ts), _accessTokenValidityCounter(0), _eventList() {}

    std::list<String>getEventList(void) {
        return this->_eventList;
    }

    void startRegistration(String& url, String& code)
    {
        String scope = FPSTR(GoogleApiCalendar::SCOPE);
        JsonDocument doc;
        GoogleOAuth2::Response ret = this->requestDeviceAndUserCode(doc, scope);

        if (ret == GoogleOAuth2::OK) {
            url  = doc[F("verification_url")].as<String>();
            code = doc[F("user_code")].as<String>();

            this->_accessTokenValidityCounter = doc[F("interval")];
        }
    }


    void validateRegistration(void)
    {
        unsigned long now = millis();
        unsigned long nextInterval;
        JsonDocument doc;

        uint16_t interval = 1000 * this->_accessTokenValidityCounter;

        GoogleOAuth2::Response ret = GoogleOAuth2::PENDING;
        nextInterval = now + interval;
        
        do {
            delay(500);
            now = millis();
            if (nextInterval < now) {
                ret = this->pollAuthorization(doc);

                nextInterval = now + interval;
            }
        } while (ret == GoogleOAuth2::PENDING);
        
        if (ret == GoogleOAuth2::OK) {
            const uint16_t expiresInSeconds = doc[F("expires_in")];
            this->_resetValidAccessTokenCounter(expiresInSeconds);

            ret = this->getCalendars(doc);

            if (ret == GoogleOAuth2::OK) {
                this->_setCalendar(doc);
            }
        } else {
            this->_accessTokenValidityCounter = 0;
        }
    }


    const boolean hasAuthorization(void)
    {
        return this->_accessTokenValidityCounter > 0;
    }


    const bool maintainEveryMinute(void)
    {
        if (!this->hasAuthorization()) {
            JsonDocument doc;
            GoogleOAuth2::Response ret = this->refreshAccessToken(doc);
            if (ret == GoogleOAuth2::OK) {
                const uint16_t expiresInSeconds = doc[F("expires_in")];
                this->_resetValidAccessTokenCounter(expiresInSeconds);
            } else {
                return false;
            }
        }

        this->_accessTokenValidityCounter--;

        return true;
    }


    const boolean syncAt(String& ts)
    {
        const char org = ts[17];

        JsonDocument doc;
        String t0 = ts;
        t0.setCharAt(18, '0');
        ts.setCharAt(18, '9');

        GoogleOAuth2::Response ret = this->getEvents(doc, this->_calendar, t0, ts);

        ts.setCharAt(17, org);

        if (ret != GoogleOAuth2::OK) {
            return false;
        }

        this->_eventList.clear();

        for (JsonObject item : doc[F("items")].as<JsonArray>()) {
            // this->_eventList.emplace_back(item["summary"].as<String>());
            // !!! valid by &fields=items(summary) only !!!
            this->_eventList.emplace_back(item.begin()->value().as<String>());
        }

        return true;
    }


    protected:

    void _resetValidAccessTokenCounter(const uint16_t expiresInSeconds)
    {
        const uint8_t minuteInSeconds = 60;
        if (expiresInSeconds > ((uint8_t) -1) * minuteInSeconds) {
            this->_accessTokenValidityCounter = -1;
        } else {
            this->_accessTokenValidityCounter = expiresInSeconds / minuteInSeconds;
        }
    }


    void _setCalendar(JsonDocument doc)
    {
        JsonArray items = doc[F("items")].as<JsonArray>();
        for (JsonObject item : items) {
            if (this->_calendar.equals(item[F("summary")].as<String>())) {
                this->_calendar = item[F("id")].as<String>();
                return;
            }
        }
    }

    
    String _calendar;
    TimestampRFC3339Ntp _timestamp;
    uint8_t _accessTokenValidityCounter;
    std::list<String> _eventList;

};
