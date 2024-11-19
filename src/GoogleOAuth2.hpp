#pragma once


#include "GoogleSchedular.hpp"


class GoogleOAuth2 {

    public:

    enum Response {
        ERROR,
        PENDING,
        OK,
    };

    
    GoogleOAuth2(const String& clientId, const String& clientSecret) : _clientId(clientId), _clientSecret(clientSecret), _refreshToken(), _accessToken(), _httpClient(), _wifiClient()
    {
        _wifiClient.setInsecure();
        _httpClient.useHTTP10(true);
    }

    String getRefreshToken(void) const { return _refreshToken; }
    void setRefreshToken(const String& tok) { _refreshToken = tok; }

    // POST https://oauth2.googleapis.com/device/code
    const GoogleOAuth2::Response requestDeviceAndUserCode(JsonDocument& response, const String& scope)
    {
        int httpCode;
        JsonDocument request;
        request[F("client_id")]    = _clientId;
        request[F("scope")]        = scope;

        _postJsonRequest(F("/device/code"), httpCode, response, request);

        if (httpCode != HTTP_CODE_OK) {
            return ERROR;
        }

        /*
            device_code     : this unique device
            expires_in      : uint16_t  in seconds, lifetime of this data
            interval        : uint8_t   in seconds, pool interval for checking the user response
            user_code       : char[15]  what the user must fill on the given URL
            verification_url: string    envoyer l'utilisateur dessus, QRCODE?
        */
        _refreshToken = response["device_code"].as<String>();

        return OK;
    }
    
    // POST https://oauth2.googleapis.com/token
    const GoogleOAuth2::Response pollAuthorization(JsonDocument& response)
    {
        int httpCode;
        JsonDocument request;
        request[F("client_id")]        = _clientId;
        request[F("client_secret")]    = _clientSecret;
        request[F("device_code")]      = _refreshToken;
        request[F("grant_type")]       = F("urn:ietf:params:oauth:grant-type:device_code");

        _postJsonRequest(F("/token"), httpCode, response, request);

        switch (httpCode) {
            case HTTP_CODE_PRECONDITION_REQUIRED:
                /*
                    error           : access_denied | authorization_pending | slow_down | *
                    error_description
                */
                return PENDING;
            
            case HTTP_CODE_OK:
                /*
                    access_token    : keep that
                    expires_in      : in seconds, lifetime of access_token
                    refresh_token   : then use this token for a new access_token
                    scope           : // useless
                    token_type      : Bearer
                */
                _refreshToken = response[F("refresh_token")].as<String>();
                _accessToken = response[F("access_token")].as<String>();

                return OK;
        }

        return ERROR;
    }

    // POST https://oauth2.googleapis.com/token
    const GoogleOAuth2::Response refreshAccessToken(JsonDocument& response)
    {
        int httpCode;
        JsonDocument request;
        request[F("client_id")]        = _clientId;
        request[F("client_secret")]    = _clientSecret;
        request[F("grant_type")]       = F("refresh_token");
        request[F("refresh_token")]    = _refreshToken;

        _postJsonRequest(F("/token"), httpCode, response, request);
        
        if (httpCode != HTTP_CODE_OK) {
            return ERROR;
        }

        /*
            access_token    : keep that
            expires_in      : in seconds, lifetime of access_token
            scope           : // useless
            token_type      : Bearer
        */
        _accessToken = response[F("access_token")].as<String>();

        return OK;
    }

    protected:

    void _postJsonRequest(const String path, int& httpCode, JsonDocument& response, const JsonDocument& request)
    {
        String payload;
        serializeJson(request, payload);

        _httpClient.begin(_wifiClient, F("oauth2.googleapis.com"), 443, path, true);
        _httpClient.addHeader(F("Content-Type"), F("application/json"));
        
        httpCode = _httpClient.POST(payload);
        deserializeJson(response, _wifiClient);
        _wifiClient.stop();
        _httpClient.end();
    }

    const String _clientId;
    const String _clientSecret;
    String _refreshToken;
    String _accessToken;

    HTTPClient _httpClient;
    WiFiClientSecure _wifiClient;
};
