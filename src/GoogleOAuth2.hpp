#pragma once


#include "GoogleSchedular.hpp"


class GoogleOAuth2 {

    public:

    enum Response {
        ERROR,
        PENDING,
        OK,
    };

    
    GoogleOAuth2(const String& clientId, const String& clientSecret) : _clientId(clientId), _clientSecret(clientSecret), _http(), _client() {
        this->_client.setInsecure();
    }

    String getRefreshToken(void) const { return this->_refreshToken; }
    void setRefreshToken(const String& tok) { this->_refreshToken = tok; }

    // POST https://oauth2.googleapis.com/device/code
    const GoogleOAuth2::Response requestDeviceAndUserCode(JsonDocument& response, const String& scope)
    {
        int httpCode;
        {
            JsonDocument doc;
            doc[F("client_id")]    = this->_clientId;
            doc[F("scope")]        = scope;

            httpCode = this->_postJsonRequest(F("/device/code"), doc);
        }

        yield();

        GoogleOAuth2::Response ret = ERROR;

        if (httpCode == HTTP_CODE_OK) {
            const String payload = this->_http.getString();
            deserializeJson(response, payload);
            /*
                device_code     : this unique device
                expires_in      : uint16_t  in seconds, lifetime of this data
                interval        : uint8_t   in seconds, pool interval for checking the user response
                user_code       : char[15]  what the user must fill on the given URL
                verification_url: string    envoyer l'utilisateur dessus, QRCODE?
            */
            this->_refreshToken = response["device_code"].as<String>();
            ret = OK;
        }

        this->_http.end();
        
        return ret;
    }
    
    // POST https://oauth2.googleapis.com/token
    const GoogleOAuth2::Response pollAuthorization(JsonDocument& response)
    {
        int httpCode;
        {
            JsonDocument doc;
            doc[F("client_id")]        = this->_clientId;
            doc[F("client_secret")]    = this->_clientSecret;
            doc[F("device_code")]      = this->_refreshToken;
            doc[F("grant_type")]       = F("urn:ietf:params:oauth:grant-type:device_code");

            httpCode = this->_postJsonRequest(F("/token"), doc);
        }

        yield();

        GoogleOAuth2::Response ret = ERROR;

        if (httpCode == HTTP_CODE_PRECONDITION_REQUIRED) {
            /*
                error           : access_denied | authorization_pending | slow_down | *
                error_description
            */
           ret = PENDING;
        }

        if (httpCode == HTTP_CODE_OK) {
            const String payload = this->_http.getString();
            deserializeJson(response, payload);
            /*
                access_token    : keep that
                expires_in      : in seconds, lifetime of access_token
                refresh_token   : then use this token for a new access_token
                scope           : // useless
                token_type      : Bearer
            */
            this->_refreshToken = response[F("refresh_token")].as<String>();
            this->_accessToken = response[F("access_token")].as<String>();

            ret = OK;
        }

        this->_http.end();

        return ret;
    }

    // POST https://oauth2.googleapis.com/token
    const GoogleOAuth2::Response refreshAccessToken(JsonDocument& response)
    {
        int httpCode;
        {
            JsonDocument doc;
            doc[F("client_id")]        = this->_clientId;
            doc[F("client_secret")]    = this->_clientSecret;
            doc[F("grant_type")]       = F("refresh_token");
            doc[F("refresh_token")]    = this->_refreshToken;

            httpCode = this->_postJsonRequest(F("/token"), doc);
        }

        yield();

        GoogleOAuth2::Response ret = ERROR;

        if (httpCode == HTTP_CODE_OK) {
            const String payload = this->_http.getString();
            deserializeJson(response, payload);
            /*
                access_token    : keep that
                expires_in      : in seconds, lifetime of access_token
                scope           : // useless
                token_type      : Bearer
            */
            this->_accessToken = response[F("access_token")].as<String>();

            ret = OK;
        }

        this->_http.end();

        return ret;
    }

    protected:

    const int _postJsonRequest(const String path, const JsonDocument& doc) {
        String payload;
        serializeJson(doc, payload);
        
        this->_http.begin(this->_client, F("oauth2.googleapis.com"), 443, path, true);
        this->_http.addHeader(F("Content-Type"), F("application/json"));
        
        return this->_http.POST(payload);
    }

    const String _clientId;
    const String _clientSecret;
    String _refreshToken;
    String _accessToken;

    HTTPClient _http;
    WiFiClientSecure _client;
};
