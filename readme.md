# Google Schedular

Use your Google Calendar as a Scheduler for your Arduino projects.


## Limitations

Visit: 
- https://developers.google.com/calendar/api/guides/quota
- https://cloud.google.com/api-keys/docs/quotas


### Notes

- Device OAuth
  https://developers.google.com/identity/protocols/oauth2/limited-input-device
  Create dedicated webpage? 

- GET https://www.googleapis.com/calendar/v3/users/me/calendarList?fields=items(id,summary)
{ items: [{ id: "123", summary: "ArduinoRelay" }] }

- GET https://www.googleapis.com/calendar/v3/calendars/{calendar.id}/events?fields=items(summary)&timeMin=2024-10-25T23:27:00Z&timeMax=2024-10-25T21:27:59Z
{ items: [{ summary: "R1" }] }

