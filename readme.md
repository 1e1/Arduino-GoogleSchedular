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

```
WiFiUDP udp;
TimestampRFC3339Ntp ntp(udp);
GoogleSchedular gs(GOOGLE_API_CLIENT_ID, GOOGLE_API_CLIENT_SECRET, ntp);
```


```
Serial.println("|- starting registration");
{
    String url;
    String code; 
    gs.startRegistration(url, code);

    Serial.print("| |- URL : "); Serial.println(url);
    Serial.print("| +- CODE: "); Serial.println(code);
    Serial.flush();
}
```

```
Serial.print("|- waiting for validation");
{
    uint8_t line_size = 25;
    FastTimer<FastTimer_precision_t::P_1s_4m> timer1s;
    do {
        timer1s.update();
        if (timer1s.isTickBy64()) {
            ntp.request(NTP_HOST);
            if (++line_size >= 80) {
                Serial.println();
                line_size = 0;
            }
            Serial.print('.');
        } else {
            delay(100);
        }

        ntp.listen();
        gs.maintain();
    } while(gs.getState() == GoogleSchedular::INIT);
    Serial.println();
    Serial.println("+- CONNECTED");
    Serial.flush();
```

```
Serial.print("|- getting calendar '");
Serial.print(CALENDAR_NAME);
Serial.println("'");
gs.setCalendar(CALENDAR_NAME);
Serial.println("+- DONE");
Serial.flush();
```

```
Serial.println("|- load events");
{
    ntp.syncRFC3339();
    String ts = ntp.getTimestampRFC3339();
    gs.syncAt(ts);
    for(String e : gs.getEventList()) {
        Serial.println("|- " + e);
    }
    Serial.println("+-----------");
}
```

```
void loop()
{
    if (timer1mn.hasChanged()) {
        ntp.request(NTP_HOST);

        ntp.syncRFC3339(timer1mn.getElapsedTime());
        String ts = ntp.getTimestampRFC3339();

        Serial.print("-- ");
        Serial.println(ts);
        gs.syncAt(ts);
        for(String e : gs.getEventList()) {
            Serial.println("- " + e);
        }
        Serial.flush();
    } else {
        delay(100);
    }

    ntp.listen();
    gs.maintain();
}
```


## Limitations

Visit: 
- https://developers.google.com/calendar/api/guides/quota
- https://cloud.google.com/api-keys/docs/quotas

