#pragma once

// https://developers.google.com/calendar/api/guides/quota
// https://cloud.google.com/api-keys/docs/quotas


#include <Arduino.h>
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


    const boolean hasFailed(void) const       { return _state == State::ERROR; }
    const boolean isInitialized(void) const   { return _state == State::INIT;  }
    const boolean isAuthenticated(void) const { return _state & B0100; }
    const boolean isLinked(void) const        { return _state == State::LINKED; }

    std::list<String>getEventList(void) const { return _eventList; }

    const boolean hasExpired(void)
    {
        return _expirationTimestamp < _timestamp.getTimestampUnix();
    }

    void setCalendar(String calendarName)
    {
        if (_state & State::AUTHENTICATED) {
            JsonDocument doc;
            const GoogleOAuth2::Response ret = getCalendars(doc);

            if (ret == GoogleOAuth2::OK) {
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
    }

    void startQuietRegistration()
    {
        String url;
        startRegistration(url, _accessToken);
        _state = State::VOID;
    }

    const String getQuietUserCode(void) {
        if (_state == State::VOID) {
            _state = State::INIT;
            return _accessToken;
        }

        return "";
    }

    void startRegistration(String& url, String& code)
    {
        _state = State::VOID;

        const String scope = FPSTR(GoogleApiCalendar::SCOPE);
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

    void maintain(void)
    {
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
            if (_accessToken.isEmpty()) {
                // has been init with refreshToken only
                maintainAuthorization(true);
                // refresh_token is incorrect
                if (_state == State::ERROR) {
                    _refreshToken.remove(0);
                // first authentication
                } else {
                    _state = State::AUTHENTICATED;
                }
            } else {
                // has been startQuietRegistration()
            }
        }
    }


    void handleRegistration(void)
    {
        if (_expirationTimestamp < _timestamp.getTimestampUnix()) {
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

    void maintainAuthorization(const boolean force=false)
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

    void syncAt(String& ts)
    {
        GoogleOAuth2::Response ret;
        JsonDocument doc;
        {
            const char org = ts[17];
            String t0 = ts;
            t0.setCharAt(18, '0');
            ts.setCharAt(18, '9');

            ret = getEvents(doc, _calendarId, t0, ts);

            ts.setCharAt(17, org);
        }

        if (ret == GoogleOAuth2::OK) {
            _state = State::ERROR;
        } else {
            _eventList.clear();

            const JsonArray items = doc[F("items")].as<JsonArray>();

            for (JsonObject item : items) {
                // String summary = item["summary"].as<String>();
                // !!! valid by &fields=items(summary) only !!!
                const String summary = item.begin()->value().as<String>();
                _eventList.push_back(summary);
                /*
                uint8_t startIndex = 0;
                uint8_t endIndex;
                do {
                    endIndex = summary.indexOf('\n', startIndex);
                    String line = summary.substring(startIndex, endIndex).trim();

                    _eventList.push_back(line);
                    startIndex = endIndex + 1;
                } while (endIndex != -1);
                */
            }
        }
    }


    protected:

    void _setExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        _expirationTimestamp = _timestamp.getTimestampUnix(expiresInSeconds);
    }

    void _setSecureExpirationTimestamp(const uint16_t expiresInSeconds)
    {
        _expirationTimestamp = _timestamp.getTimestampUnix(expiresInSeconds - GoogleSchedular::EXPIRATION_TIME_MARGIN);
    }

    
    State _state;
    String _calendarId;
    TimestampRFC3339Ntp& _timestamp;
    unsigned long _expirationTimestamp;
    std::list<String> _eventList;

};
