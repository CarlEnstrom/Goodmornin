# ESP32-C3 Alarmklocka (WiFi + NTP + Web UI + LittleFS + PWM Audio)

## Översikt
En väckarklocka för ESP32-C3 som:
- Ansluter till WiFi eller startar AP-portal om credentials saknas
- Hämtar tid via NTP och använder tidszon Europe/Stockholm (DST)
- Har web UI (mobilvänligt) och REST API för att hantera multipla alarm
- Lagrar config i NVS (Preferences) och filer i LittleFS under /audio/
- Spelar ljud via PWM (LEDC) med extern analog kedja

## Byggkrav
- PlatformIO (VS Code eller CLI)
- Ett ESP32-C3-kort som stöds av `esp32-c3-devkitm-1` eller byt board i `platformio.ini`

## Bygga och flasha firmware
1. Klona projektet.
2. Bygg:
   pio run
3. Flash:
   pio run -t upload

## Ladda upp LittleFS (web UI + filer)
1. Lägg filer i `data/`
2. Upload filesystem image:
   pio run -t uploadfs

## Första start och WiFi
- Om inga WiFi-credentials finns i NVS startar enheten AP-läge.
- Anslut till AP "AlarmClock-xxxxxx" och öppna:
  http://192.168.4.1
- Ange SSID och lösenord. Enheten startar om.
- Efter omstart nås web UI via enhetens IP i ditt nät.

## Admin token
- Admin token är valfri.
- Om satt krävs den för:
  - alla POST/PUT/DELETE under /api som ändrar data
  - filupload och filradering
  - test_audio och fire via UI
- UI lagrar token i webbläsarens localStorage och skickar den i headern:
  X-Admin-Token: <token>

## Ljudformat (rekommenderat)
WAV ska alltid fungera. MP3 är best effort och beror på decoder och filens parametrar.

Rekommendation:
- WAV: mono, 16-bit PCM, 16000 Hz
- MP3: mono, 16-22.05 kHz, låg bitrate, korta klipp

### ffmpeg-exempel
Konvertera till rekommenderad WAV:
ffmpeg -i input.mp3 -ac 1 -ar 16000 -c:a pcm_s16le output.wav

Kort klipp 5 sek:
ffmpeg -i input.mp3 -t 5 -ac 1 -ar 16000 -c:a pcm_s16le clip.wav

## PWM Audio koppling
ESP32-C3 kan inte driva högtalare direkt.

Rekommenderad kedja:
1) PWM pin (AUDIO_PWM_PIN) -> RC lågpassfilter
2) Lågpass -> analog förstärkare (t ex PAM8302/PAM8403)
3) Förstärkare -> högtalare

En enkel RC:
- R: 1 kΩ i serie från PWM pin
- C: 10 nF till GND efter R
Detta ger grov filtrering. Justera värden efter din förstärkare och önskat band.

## Konfigurerbara pinnar
- Audio PWM pin default: GPIO5
- Alarm-knapp per alarm: gpio_pin i config
Knappar kopplas till GND vid tryck, intern pullup används.

Undvik pins som krockar med boot/flash på din specifika C3-modul.

## REST API (inbound)
GET  /api/status
GET  /api/alarms
GET  /api/alarms/{id}
POST /api/alarms                    (admin)
PUT  /api/alarms/{id}               (admin)
POST /api/alarms/{id}/enable        (admin)
POST /api/alarms/{id}/disable       (admin)
POST /api/alarms/{id}/snooze        (admin)
POST /api/alarms/{id}/dismiss       (admin)
POST /api/alarms/{id}/fire          (admin)
POST /api/alarms/{id}/test_audio    (admin)

Filer:
GET    /api/files                   (admin)
GET    /api/files/space             (admin)
POST   /api/files/upload            (admin, multipart form-data field "file")
DELETE /api/files?path=/audio/x.wav (admin)

Config:
GET  /api/config/export             (admin)
POST /api/config/import             (admin)

System:
POST /api/system/restart            (admin)

### curl-exempel
Status:
curl http://<ip>/api/status

Lista alarm:
curl http://<ip>/api/alarms

Skapa alarm (admin token i header):
curl -X POST http://<ip>/api/alarms \
  -H "Content-Type: application/json" \
  -H "X-Admin-Token: <token>" \
  -d '{"label":"Test","enabled":false,"hour":7,"minute":30,"days_bitmask":31}'

Tvinga ring:
curl -X POST http://<ip>/api/alarms/<id>/fire -H "X-Admin-Token: <token>"

## Inbound webhook per alarm
POST /wh/alarm/{id}?token=...

Body:
{
  "action":"set|enable|disable|snooze|dismiss|fire",
  "hour":7,
  "minute":30,
  "days_bitmask":3,
  "once_date":"2026-01-05",
  "audio_source":{
    "type":"local|url",
    "local_path":"/audio/default.wav",
    "url":"http://...",
    "fallback_local_path":"/audio/default.wav"
  }
}

Token valideras mot alarmets inbound_webhook_token.

## Outbound webhook-protokoll (JSON)
Enheten POST:ar JSON med retry 1s, 3s, 9s:

{
  "device_id": "esp32c3-<chipid>",
  "alarm_id": 3,
  "event": "set|fired|snoozed|dismissed|enabled|disabled|audio_error",
  "source": "webgui|webhook|gpio|system",
  "ts_iso": "2025-12-25T22:15:03+01:00",
  "ts_unix": 1766697303,
  "next_fire_iso": "2025-12-26T07:30:00+01:00",
  "alarm_enabled": true,
  "detail": { "http_status": 0, "error": "timeout" }
}

## Notis om MP3 i denna leverans
Firmware är fullt körbar och robust för WAV.
MP3 är "best effort" men kräver att du ersätter MP3-dekodningsstubben i src/main.cpp med en riktig minimp3-implementation om du vill ha faktisk MP3-dekodning på enheten.
