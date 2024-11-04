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
        int httpCode = this->_getRequest(F("/calendar/v3/users/me/calendarList?fields=items(id,summary)&minAccessRole=reader&showHidden=true"));

        yield();

        GoogleOAuth2::Response ret = ERROR;

        if (httpCode == HTTP_CODE_OK) {
            String payload = this->_http.getString();
            deserializeJson(response, payload);
            /*
            items[] =
                id      : GoogleID
                summary : title
            */

            ret = OK;
        }

        this->_http.end();

        return ret;
    }

    const GoogleOAuth2::Response getEvents(JsonDocument& response, const String& calendarId, const String& timeMin, const String& timeMax)
    {
        String uri = this->_buildEventsUri(calendarId, timeMin, timeMax);
        int httpCode = this->_getRequest(uri);

        yield();

        GoogleOAuth2::Response ret = ERROR;

        if (httpCode == HTTP_CODE_OK) {
            String payload = this->_http.getString();
            deserializeJson(response, payload);
            /*
            items[] =
                summary : title
            */

            ret = OK;
        }

        this->_http.end();

        return ret;
    }

    protected:

    const int _getRequest(const String path) {
        this->_http.begin(this->_client, F("www.googleapis.com"), 443, path, true);
        this->_http.addHeader(F("Authorization"), FPSTR("Bearer ") + this->_accessToken);

        return this->_http.GET();
    }

    const String _buildEventsUri(const String& calendarId, const String& timeMin, const String& timeMax)
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