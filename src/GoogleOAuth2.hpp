#pragma once


#include "GoogleSchedular.hpp"


/**
 * OAuth 2.0 client for the Google "limited-input device" flow.
 *
 * This is the lowest layer of the library: it only turns credentials into an
 * access_token and knows nothing about calendars. It is written for the tight
 * RAM/flash budget of an ESP8266/ESP32, so a few deliberate trade-offs are made
 * to stay lightweight:
 *
 *  - A single reusable HTTPClient + WiFiClientSecure are kept as members and
 *    reused for every request, instead of allocating one per call.
 *  - useHTTP10(true) disables chunked transfer decoding so the JSON body can be
 *    streamed straight from the socket into ArduinoJson (see _postJsonRequest),
 *    avoiding a full in-RAM copy of the response.
 *  - setInsecure() skips X.509 certificate validation on purpose. Pinning a CA
 *    on a device with a few kB of free heap costs RAM and CPU and needs periodic
 *    root-certificate updates; the flow only ever talks to Google endpoints over
 *    TLS, so we accept unauthenticated-peer TLS as the light-weight default.
 *  - _refreshToken is reused as scratch storage for the transient device_code
 *    during registration (see requestDeviceAndUserCode / pollAuthorization),
 *    so no extra String member is needed for a value that only lives until the
 *    user validates the device.
 */
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

    // HTTP status of the last token request (refreshAccessToken). Lets the caller
    // tell a rejected credential (400 invalid_grant) from a transient failure
    // (negative code / 5xx). 0 before any attempt. See GoogleSchedular::isAuthInvalid().
    int lastAuthHttpCode(void) const { return _lastAuthHttpCode; }

    // POST https://oauth2.googleapis.com/device/code
    GoogleOAuth2::Response requestDeviceAndUserCode(JsonDocument& response, const String& scope)
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
            verification_url: string    where to send the user (QR code friendly)
        */
        // Reuse _refreshToken to hold the transient device_code until the user
        // validates the device; pollAuthorization() replaces it with the real
        // refresh_token on success. Saves one String member.
        _refreshToken = response["device_code"].as<String>();

        return OK;
    }
    
    // POST https://oauth2.googleapis.com/token
    GoogleOAuth2::Response pollAuthorization(JsonDocument& response)
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
    GoogleOAuth2::Response refreshAccessToken(JsonDocument& response)
    {
        int httpCode;
        JsonDocument request;
        request[F("client_id")]        = _clientId;
        request[F("client_secret")]    = _clientSecret;
        request[F("grant_type")]       = F("refresh_token");
        request[F("refresh_token")]    = _refreshToken;

        _postJsonRequest(F("/token"), httpCode, response, request);
        _lastAuthHttpCode = httpCode;

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

    // Sends `request` as a JSON body and streams the JSON reply directly from
    // the socket into `response`. Thanks to useHTTP10(true) the body is read
    // without chunked-decoding, so no intermediate String holds the full
    // response. The shared HTTP/TLS clients are opened and closed per call to
    // keep only one connection alive at a time.
    void _postJsonRequest(const String path, int& httpCode, JsonDocument& response, const JsonDocument& request)
    {
        String payload;
        serializeJson(request, payload);

        _httpClient.begin(_wifiClient, F("oauth2.googleapis.com"), 443, path, true);
        _httpClient.addHeader(F("Content-Type"), F("application/json"));
        
        httpCode = _httpClient.POST(payload);
        // A truncated/garbled body on an otherwise-OK response would silently
        // yield empty fields; demote it to a failure so callers hit the error path.
        const DeserializationError err = deserializeJson(response, _wifiClient);
        if (err && httpCode == HTTP_CODE_OK) {
            httpCode = 0;
        }
        _wifiClient.stop();
        _httpClient.end();
    }

    const String _clientId;
    const String _clientSecret;
    String _refreshToken;
    String _accessToken;
    int _lastAuthHttpCode = 0;

    HTTPClient _httpClient;
    WiFiClientSecure _wifiClient;
};
