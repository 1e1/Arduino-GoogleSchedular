#pragma once


#include "GoogleSchedular.hpp"
#include "GoogleOAuth2.hpp"


class GoogleApiCalendar : public GoogleOAuth2 {

    public: 
    
    static constexpr char SCOPE[] PROGMEM = "https://www.googleapis.com/auth/calendar.readonly";


    GoogleApiCalendar(const String& clientId, const String& clientSecret): GoogleOAuth2(clientId, clientSecret) {}

    // GET https://www.googleapis.com/calendar/v3/users/me/calendarList?fields=items(id,summary)
    const GoogleOAuth2::Response getCalendars(JsonDocument& response)
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

    const GoogleOAuth2::Response getEvents(JsonDocument& response, const String& calendarId, const String& timeMin, const String& timeMax)
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

    void _getRequest(const String path, int& httpCode, JsonDocument& response) {
        _httpClient.begin(_wifiClient, F("www.googleapis.com"), 443, path, true);
        _httpClient.addHeader(F("Authorization"), FPSTR("Bearer ") + _accessToken);

        httpCode = _httpClient.GET();
        deserializeJson(response, _wifiClient);
        _wifiClient.stop();
        _httpClient.end();
    }

    const String _buildEventsUri(const String& calendarId, const String& timeMin, const String& timeMax) const
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