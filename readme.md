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

As soon as you get the `verification_url` and `user_code`, go to this URL
(most probably https://www.google.com/device)
then authenticate with the given code and your Google Account. 


## Code

Create the Schedular with a NTP client for knowing the expiration times,
and the current datetime when requesting the Google Calendar. 

```
WiFiUDP udp;
TimestampRFC3339Ntp ntp(udp);
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, ntp);
```

Get user temporary code for merging the user account to this session.
```
Serial.println("|- starting registration");
{
    String url;
    String code; 
    gs.startRegistration(url, code);

    Serial.print("URL : "); Serial.println(url);
    Serial.print("CODE: "); Serial.println(code);
}
```

Waiting the refresh_access / access_token from Google when the user is authenticated on Google. 
```
Serial.print("|- waiting for validation");
{
    FastTimer<FastTimer_precision_t::P_1s_4m> timer1s;
    do {
        timer1s.update();
        if (timer1s.isTickBy64()) {
            ntp.request(NTP_HOST);
        } else {
            delay(100);
        }

        ntp.listen();
        gs.maintain();
    } while(gs.isInitialized());
```

Getting the targeted calendar identifier from its name. 
```
gs.setCalendar(CALENDAR_NAME);
```

Generating the timestamp string, 
then request the current event titles/summaries/labels. 
```
    ntp.syncRFC3339();
    String ts = ntp.getTimestampRFC3339();
    gs.syncAt(ts);

    for(String e : gs.getEventList()) {
        Serial.println(e);
    }
}
```

Don't forget to maintain the user session with `gs.maintaint()`
Example: 
```
void loop()
{
    if (timer1mn.hasChanged()) {
        ntp.request(NTP_HOST);

        ntp.syncRFC3339(timer1mn.getElapsedTime());
        String ts = ntp.getTimestampRFC3339();
        gs.syncAt(ts);

        Serial.print("-- ");
        Serial.println(ts);
        for(String e : gs.getEventList()) {
            Serial.println("- " + e);
        }
    } else {
        delay(100);
    }

    if (ntp.listen()) {
        gs.maintain();
    }
}
```


## Limitations

Visit: 
- https://developers.google.com/calendar/api/guides/quota
- https://cloud.google.com/api-keys/docs/quotas

