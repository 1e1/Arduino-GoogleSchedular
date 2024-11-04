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


#ifndef SCHEDULAR_NAME_SEPARATOR
#define SCHEDULAR_NAME_SEPARATOR ','
#endif


class GoogleSchedular : public GoogleApiCalendar {

    public:

    /* 
    bits are CADE: 
    - C: Calendar => has calendar.id
    - A: Authorized => has access_token
    - D: initializing Device=> has device_code
    - E: Failed => has Error
    */
    enum State {
        VOID          = B0000,
        INIT          = B0010,
        AUTHENTICATED = B0110,
        LINKED        = B1110,
        ERROR         = B0001,
    };

    static constexpr uint8_t EXPIRATION_TIME_MARGIN = 64;


    GoogleSchedular(const String& clientId, const String& clientSecret, TimestampRFC3339Ntp& ts) : GoogleApiCalendar(clientId, clientSecret), _timestamp(ts), _expirationTimestamp(0), _state(State::VOID), _eventList() {}


    const boolean hasFailed(void)       { return this->_state == State::ERROR; }
    const boolean isInitialized(void)   { return this->_state == State::INIT;  }
    const boolean isAuthenticated(void) { return this->_state & B0100; }
    const boolean isLinked(void)        { return this->_state == State::LINKED; }

    std::list<String>getEventList(void) { return this->_eventList; }

    const boolean hasExpired(void)
    {
        return  this->_expirationTimestamp < this->_timestamp.getTimestampUnix();
    }

    void setCalendar(String calendarName)
    {
        if (this->_state & State::AUTHENTICATED) {
            JsonDocument doc;
            GoogleOAuth2::Response ret = this->getCalendars(doc);

            if (ret == GoogleOAuth2::OK) {
                this->_state = State::AUTHENTICATED;

                JsonArray items = doc[F("items")].as<JsonArray>();
                for (JsonObject item : items) {
                    if (calendarName.equals(item[F("summary")].as<String>())) {
                        this->_calendarId = item[F("id")].as<String>();
                        this->_state = State::LINKED;
                        break;
                    }
                }
            }
            }
    }

    void startRegistration(String& url, String& code)
    {
        this->_state = State::VOID;

        String scope = FPSTR(GoogleApiCalendar::SCOPE);
        JsonDocument doc;
        GoogleOAuth2::Response ret = this->requestDeviceAndUserCode(doc, scope);

        if (ret == GoogleOAuth2::OK) {
            url  = doc[F("verification_url")].as<String>();
            code = doc[F("user_code")].as<String>();

            uint8_t interval = doc[F("interval")];
            this->_calendarId = String(interval);

            this->_setExpirationTimestamp(interval);
            this->_state = State::INIT;
        }
    }

    void maintain(void)
    {
        if (this->isAuthenticated()) {
            this->maintainAuthorization(false);
        } else if (this->isInitialized()) {
            this->handleRegistration();
        }
    }


    void handleRegistration(void)
    {
        if (this->_expirationTimestamp < this->_timestamp.getTimestampUnix()) {
            JsonDocument doc;
            GoogleOAuth2::Response ret = this->pollAuthorization(doc);

            switch (ret) {
                case GoogleOAuth2::PENDING: {
                    uint8_t interval = this->_calendarId.toInt();
                    this->_setExpirationTimestamp(interval);
                    break;
                }
                case GoogleOAuth2::OK: {
                    const uint16_t expiresInSeconds = doc[F("expires_in")];
                    this->_setSecureExpirationTimestamp(expiresInSeconds);
                    this->_state = State::AUTHENTICATED;
                    break;
                }
                default: {
                    this->_setExpirationTimestamp(0);
                    this->_state = State::ERROR;
                }
            }
        }
    }

    void maintainAuthorization(const boolean force=false)
    {
        if (force || this->hasExpired()) {
            JsonDocument doc;
            GoogleOAuth2::Response ret = this->refreshAccessToken(doc);
            if (ret == GoogleOAuth2::OK) {
                const uint16_t expiresInSeconds = doc[F("expires_in")];
                this->_setSecureExpirationTimestamp(expiresInSeconds);
            } else {
                this->_state = State::ERROR;
            }
        }
    }

    void syncAt(String& ts)
    {
        const char org = ts[17];

        JsonDocument doc;
        String t0 = ts;
        t0.setCharAt(18, '0');
        ts.setCharAt(18, '9');

        GoogleOAuth2::Response ret = this->getEvents(doc, this->_calendarId, t0, ts);

        ts.setCharAt(17, org);

        if (ret != GoogleOAuth2::OK) {
            this->_state = State::ERROR;
        }

        this->_eventList.clear();

        for (JsonObject item : doc[F("items")].as<JsonArray>()) {
            // String summary = item["summary"].as<String>();
            // !!! valid by &fields=items(summary) only !!!
            String summary = item.begin()->value().as<String>();
            this->_eventList.push_back(summary);
            /*
            uint8_t startIndex = 0;
            uint8_t endIndex;
            do {
                endIndex = summary.indexOf('\n', startIndex);
                String line = summary.substring(startIndex, endIndex).trim();

                this->_eventList.push_back(line);
                startIndex = endIndex + 1;
            } while (endIndex != -1);
            */
        }
    }


    protected:

    void _setExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        this->_expirationTimestamp = this->_timestamp.getTimestampUnix(expiresInSeconds);
    }

    void _setSecureExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        this->_expirationTimestamp = this->_timestamp.getTimestampUnix(expiresInSeconds - GoogleSchedular::EXPIRATION_TIME_MARGIN);
    }

    
    State _state;
    String _calendarId;
    TimestampRFC3339Ntp& _timestamp;
    unsigned long _expirationTimestamp;
    std::list<String> _eventList;

};
