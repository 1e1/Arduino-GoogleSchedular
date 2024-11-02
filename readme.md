# Google Schedular

Use your Google Calendar as a Scheduler for your Arduino projects.
Get summary(*) of current events from the target calendar. 

(*) a summary is like an event name/title/label. This is what you see as event name in your calendar. 


## Setup

It requires an Google OAuth2 credentials. 
https://developers.google.com/identity/protocols/oauth2/limited-input-device#prerequisites

- Get your Google Account
- Got to https://console.cloud.google.com/
- Create a Google Dev Project (ie "MyGoogleSchedular")
- Go to Consent tab: he (infinite?) time it will take to develop your Google project, as you will have to manually add the Internet addresses of your users (including yourself)
- Go to Credentials tab: create a OAuth2.0 Credentials for your device, you will get the `CLIENT_ID` and the `CLIENT_SECRET`

Official explanations: 
- Device OAuth
  https://developers.google.com/identity/protocols/oauth2/limited-input-device
  => GOOGLE_API_CLIENT_ID
  => GOOGLE_API_CLIENT_SECRET


## Run

- Retrieve the user_code to give to your user:
  ````
ArduinoJson doc;
GoogleOAuth2::Response ret = gs->requestDeviceAndUserCode(doc, GoogleApiCalendar::SCOPE);
  ```
  ```
device_code     : this unique device
expires_in      : uint16_t  in seconds, lifetime of this data
interval        : uint8_t   in seconds, pool interval for checking the user response
user_code       : char[15]  what the user must fill on the given URL
verification_url: string    envoyer l'utilisateur dessus, QRCODE?
  ```
- Loop for getting the user authorization:
  `GoogleOAuth2::Response ret = gs->pollAuthorization(doc);`
  Meanwhile, open a browser on the given URL (https://google.com/device) and authenticate yourself 
  (with an email registered in the test program if your application is not published)
- List the user calendars:
  `gs->getCalendars(doc);`
  ```
items[] =
    id      : GoogleID
    summary : calendar title
  ```
- List events between two datetimes:
  `gs->getEvents(doc, calendarId, "2020-12-31T20:00:00Z", "2020-12-31T21:00:00Z")`
  ```
items[] =
    summary : event title
  ```


## Limitations

Visit: 
- https://developers.google.com/calendar/api/guides/quota
- https://cloud.google.com/api-keys/docs/quotas

