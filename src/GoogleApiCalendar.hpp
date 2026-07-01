#pragma once


#include "GoogleSchedular.hpp"
#include "GoogleOAuth2.hpp"


/**
 * Thin Google Calendar API v3 wrapper built on top of GoogleOAuth2.
 *
 * It exposes only what a scheduler needs: listing the user's calendars and
 * reading event titles. Every request uses the Calendar API `fields` mask to
 * ask the server for the strict minimum, which is the single biggest lever for
 * keeping this library light on a microcontroller:
 *
 *  - getCalendars() requests only items(id, summary).
 *  - getEvents()    requests only items(summary), and relies on singleEvents=true
 *    so recurring events are already expanded server-side.
 *
 * Trimming the payload server-side means a smaller JsonDocument, less socket
 * traffic and less heap churn on the device. Requests reuse the shared HTTP/TLS
 * clients and the streaming reader inherited from GoogleOAuth2.
 */
class GoogleApiCalendar : public GoogleOAuth2 {

    public: 
    
    // OAuth scope for read-only Calendar access, returned as an FPSTR handle.
    // A `static constexpr char SCOPE[]` member would be odr-used by FPSTR(...)
    // (its address is taken) and then require an out-of-line definition to link
    // pre-C++17 (AVR / gnu++11). For this non-template, header-only class such a
    // definition would clash across translation units, unlike FastTimer's
    // templated NTP_PACKET. A function holding a local static PROGMEM string
    // links everywhere -- single- or multi-TU, C++11 through C++17 -- with no
    // separate definition, and keeps the literal in flash.
    static const __FlashStringHelper* scope()
    {
        static const char s[] PROGMEM = "https://www.googleapis.com/auth/calendar.readonly";
        return FPSTR(s);
    }


    GoogleApiCalendar(const String& clientId, const String& clientSecret): GoogleOAuth2(clientId, clientSecret) {}

    // GET https://www.googleapis.com/calendar/v3/users/me/calendarList?fields=items(id,summary)
    GoogleOAuth2::Response getCalendars(JsonDocument& response)
    {
        int httpCode;
        _getRequest(F("/calendar/v3/users/me/calendarList?fields=items(id,summary)&minAccessRole=reader&showHidden=true"), httpCode, response);

        if (httpCode == HTTP_CODE_OK) {
            /*
            items[] =
                id      : GoogleID
                summary : title
            */

            return OK;
        }

        return ERROR;
    }

    // timeMin/timeMax are taken as const char* so the caller can pass a
    // zero-copy timestamp (e.g. TimestampNtp::c_str()) without wrapping it in a
    // heap-allocated String; they are appended straight to the URI below.
    GoogleOAuth2::Response getEvents(JsonDocument& response, const String& calendarId, const char* timeMin, const char* timeMax)
    {
        int httpCode;
        const String uri = _buildEventsUri(calendarId, timeMin, timeMax);
        _getRequest(uri, httpCode, response);

        if (httpCode == HTTP_CODE_OK) {
            /*
            items[] =
                summary : title
            */

            return OK;
        }

        return ERROR;
    }

    protected:

    // Authenticated GET that streams the JSON reply straight into `response`.
    // Same lightweight strategy as GoogleOAuth2::_postJsonRequest: HTTP/1.0 so
    // the body is read without chunked-decoding, shared TLS client closed after
    // each call. The Bearer token is the access_token kept by GoogleOAuth2.
    void _getRequest(const String path, int& httpCode, JsonDocument& response) {
        _httpClient.begin(_wifiClient, F("www.googleapis.com"), 443, path, true);
        // Build the header in a String first: on the ESP32 core "FPSTR(..) + String"
        // is ambiguous (a FlashStringHelper* also converts to integer), so
        // concatenate explicitly to compile on both ESP8266 and ESP32.
        String auth = FPSTR("Bearer ");
        auth += _accessToken;
        _httpClient.addHeader(F("Authorization"), auth);

        httpCode = _httpClient.GET();
        // Demote a malformed body to a failure (see GoogleOAuth2::_postJsonRequest).
        const DeserializationError err = deserializeJson(response, _wifiClient);
        if (err && httpCode == HTTP_CODE_OK) {
            httpCode = 0;
        }
        _wifiClient.stop();
        _httpClient.end();
    }

    // Builds the events endpoint URI in place by appending to a single String,
    // so the query is assembled with as few reallocations as possible.
    // fields=items(summary) keeps the response to bare event titles; timeMin and
    // timeMax bound the query to the caller's window (see GoogleSchedular::syncAt).
    String _buildEventsUri(const String& calendarId, const char* timeMin, const char* timeMax) const
    {

        String uri = F("/calendar/v3/calendars/");
        uri += calendarId;
        uri += F("/events?fields=items(summary)&singleEvents=true&timeMin=");
        uri += timeMin;
        uri += F("&timeMax=");
        uri += timeMax;
        
        return uri;
    }

};