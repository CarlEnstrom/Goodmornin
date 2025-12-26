// src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <LittleFS.h>

#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "alarms.h"
#include "audio.h"

#include <time.h>
#include <sys/time.h>

#include <map>
#include <vector>
#include <functional>

static const uint32_t FW_CONFIG_VERSION = 1;

static const uint32_t BUTTON_DEBOUNCE_MS = 50;
static const uint32_t DEFAULT_LONG_PRESS_MS = 1200;
static const int DEFAULT_AUDIO_PWM_PIN = 5;

static const size_t MAX_UPLOAD_BYTES = 2 * 1024 * 1024;
static const time_t MIN_VALID_EPOCH = 1700000000;

static AsyncWebServer server(80);
static Preferences prefs;

static bool wifiConnected = false;
static bool ntpSynced = false;
static time_t lastGoodUnix = 0;

static String deviceId;
static String adminToken;

struct WebhookLastResult {
  int httpStatus = 0;
  String error;
  time_t ts = 0;
};
static WebhookLastResult lastWebhookGlobal;

static AlarmConfig alarms[MAX_ALARMS];
static AlarmRuntime alarmRt[MAX_ALARMS];

static int activeAlarmIndex = -1;

struct WebhookJob {
  String url;
  String body;
  uint8_t attempt = 0;
  uint32_t nextAttemptMs = 0;
  uint32_t alarmId = 0;
  String event;
};
static const int MAX_WEBHOOK_JOBS = 12;
static WebhookJob webhookJobs[MAX_WEBHOOK_JOBS];
static int webhookJobCount = 0;

static uint64_t chipIdU64() { return ESP.getEfuseMac(); }

static String chipIdHex() {
  char buf[17];
  uint64_t id = chipIdU64();
  snprintf(buf, sizeof(buf), "%08lx%08lx", (uint32_t)(id >> 32), (uint32_t)(id & 0xFFFFFFFF));
  return String(buf);
}

static bool isValidEpoch(time_t t) { return t >= MIN_VALID_EPOCH; }

static String isoNow() {
  time_t now = time(nullptr);
  struct tm t;
  if (!localtime_r(&now, &t)) return String();
  char tmp[32];
  strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &t);
  String s(tmp);
  if (s.length() >= 5) {
    int n = s.length();
    s = s.substring(0, n - 2) + ":" + s.substring(n - 2);
  }
  return s;
}

static bool startsWithIgnoreCase(const String& s, const char* prefix) {
  String ss = s; ss.toLowerCase();
  String pp(prefix); pp.toLowerCase();
  return ss.startsWith(pp);
}

static String sanitizeFileName(const String& input) {
  String out; out.reserve(input.length());
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (ok) out += c;
  }
  if (out.length() > 64) out = out.substring(0, 64);
  if (out.length() == 0) out = "file";
  return out;
}

static bool hasAllowedExt(const String& name) {
  String n = name; n.toLowerCase();
  return n.endsWith(".wav") || n.endsWith(".mp3");
}

static bool fileExists(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  f.close();
  return true;
}

static bool isFileUsedByAnyAlarm(const String& path) {
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].id == 0) continue;
    if (String(alarms[i].local_path) == path) return true;
    if (String(alarms[i].fallback_local_path) == path) return true;
  }
  return false;
}

static bool requireAdmin(AsyncWebServerRequest* request) {
  if (adminToken.length() == 0) return true;

  String token;
  if (request->hasHeader("X-Admin-Token")) token = request->getHeader("X-Admin-Token")->value();
  if (token.length() == 0 && request->hasParam("admin_token")) token = request->getParam("admin_token")->value();
  if (token.length() == 0 && request->hasParam("token")) token = request->getParam("token")->value();

  if (token == adminToken) return true;
  request->send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

static int findAlarmIndexById(uint32_t id) {
  for (int i = 0; i < MAX_ALARMS; i++) if (alarms[i].id == id) return i;
  return -1;
}

static const char* TZ_STOCKHOLM = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static void setupTimezone() {
  setenv("TZ", TZ_STOCKHOLM, 1);
  tzset();
}

static void saveLastGoodTimeIfValid() {
  time_t now = time(nullptr);
  if (!isValidEpoch(now)) return;
  lastGoodUnix = now;
  prefs.putULong64("last_good", (uint64_t)lastGoodUnix);
}

static void restoreLastGoodTime() {
  uint64_t v = prefs.getULong64("last_good", 0);
  if (v >= (uint64_t)MIN_VALID_EPOCH) {
    struct timeval tv;
    tv.tv_sec = (time_t)v;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    lastGoodUnix = (time_t)v;
  }
}

static void startNtp() {
  // RÄTT: starta SNTP med TZ + DST
  configTzTime(TZ_STOCKHOLM, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

static void updateNtpStatus() {
  struct tm t;
  if (getLocalTime(&t, 10)) ntpSynced = isValidEpoch(time(nullptr));
  else ntpSynced = false;
}


static bool parseOnceDate(const char* s, int& y, int& m, int& d) {
  if (!s || strlen(s) != 10) return false;
  if (s[4] != '-' || s[7] != '-') return false;
  y = String(s).substring(0, 4).toInt();
  m = String(s).substring(5, 7).toInt();
  d = String(s).substring(8, 10).toInt();
  if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  return true;
}

static time_t makeLocalEpoch(int y, int mo, int d, int hh, int mm, int ss) {
  struct tm t {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = -1;
  return mktime(&t);
}

static uint8_t weekdayBitMon0(const struct tm& t) {
  int w = t.tm_wday;          // 0=Sun..6=Sat
  int mon0 = (w == 0) ? 6 : (w - 1); // Mon=0..Sun=6
  return (uint8_t)mon0;
}

static time_t computeNextFire(const AlarmConfig& a, time_t now) {
  if (!a.enabled || a.id == 0) return 0;

  if (strlen(a.once_date) == 10) {
    int y, mo, d;
    if (!parseOnceDate(a.once_date, y, mo, d)) return 0;
    time_t t = makeLocalEpoch(y, mo, d, a.hour, a.minute, 0);
    if (t <= now) return 0;
    if (a.last_fired_unix == (uint32_t)t) return 0;
    return t;
  }

  if (a.days_mask == 0) return 0;

  struct tm nowTm {};
  localtime_r(&now, &nowTm);

  for (int dayOffset = 0; dayOffset < 8; dayOffset++) {
    struct tm cand = nowTm;
    cand.tm_mday += dayOffset;
    cand.tm_hour = a.hour;
    cand.tm_min = a.minute;
    cand.tm_sec = 0;
    cand.tm_isdst = -1;

    time_t candEpoch = mktime(&cand);
    if (candEpoch <= now) continue;

    struct tm candTm {};
    localtime_r(&candEpoch, &candTm);
    uint8_t wd = weekdayBitMon0(candTm);
    if ((a.days_mask & (1 << wd)) == 0) continue;
    if (a.last_fired_unix == (uint32_t)candEpoch) continue;
    return candEpoch;
  }
  return 0;
}

static void recomputeAllNextFires() {
  time_t now = time(nullptr);
  for (int i = 0; i < MAX_ALARMS; i++) {
    alarmRt[i].next_fire_unix = computeNextFire(alarms[i], now);
    alarmRt[i].ringing = false;
    alarmRt[i].snoozed = false;
    alarmRt[i].snooze_until = 0;
    alarmRt[i].current_fire_unix = 0;
  }
  activeAlarmIndex = -1;
}

/* Webhooks */
static void enqueueWebhook(const String& url, const String& body, uint32_t alarmId, const String& event) {
  if (url.length() == 0) return;
  if (webhookJobCount >= MAX_WEBHOOK_JOBS) return;

  WebhookJob& j = webhookJobs[webhookJobCount++];
  j.url = url;
  j.body = body;
  j.attempt = 0;
  j.nextAttemptMs = millis();
  j.alarmId = alarmId;
  j.event = event;
}

static void processWebhookQueue() {
  if (webhookJobCount == 0) return;

  uint32_t nowMs = millis();
  for (int i = 0; i < webhookJobCount; i++) {
    WebhookJob& j = webhookJobs[i];
    if (nowMs < j.nextAttemptMs) continue;

    HTTPClient http;
    int code = -1;
    String err;

    if (startsWithIgnoreCase(j.url, "https://")) {
      WiFiClientSecure client;
      client.setInsecure();
      if (!http.begin(client, j.url)) err = "begin_failed";
      else {
        http.addHeader("Content-Type", "application/json");
        code = http.POST((uint8_t*)j.body.c_str(), j.body.length());
        if (code <= 0) err = "post_failed";
      }
      http.end();
    } else {
      if (!http.begin(j.url)) err = "begin_failed";
      else {
        http.addHeader("Content-Type", "application/json");
        code = http.POST((uint8_t*)j.body.c_str(), j.body.length());
        if (code <= 0) err = "post_failed";
      }
      http.end();
    }

    bool success = (code >= 200 && code < 300);
    lastWebhookGlobal.httpStatus = code;
    lastWebhookGlobal.error = success ? "" : (err.length() ? err : "http_" + String(code));
    lastWebhookGlobal.ts = time(nullptr);

    if (success) {
      for (int k = i; k < webhookJobCount - 1; k++) webhookJobs[k] = webhookJobs[k + 1];
      webhookJobCount--;
      i--;
      continue;
    }

    j.attempt++;
    if (j.attempt >= 3) {
      for (int k = i; k < webhookJobCount - 1; k++) webhookJobs[k] = webhookJobs[k + 1];
      webhookJobCount--;
      i--;
      continue;
    }

    uint32_t backoff = (j.attempt == 1) ? 1000 : (j.attempt == 2) ? 3000 : 9000;
    j.nextAttemptMs = nowMs + backoff;
  }
}

static String buildEventPayload(const AlarmConfig& a, const String& event, const String& source, const JsonObjectConst detailObj = JsonObjectConst()) {
  JsonDocument doc;

  doc["device_id"] = deviceId;
  doc["alarm_id"] = a.id;
  doc["event"] = event;
  doc["source"] = source;

  time_t now = time(nullptr);
  doc["ts_iso"] = isoNow();
  doc["ts_unix"] = (int64_t)now;

  time_t next = 0;
  int idx = findAlarmIndexById(a.id);
  if (idx >= 0) next = alarmRt[idx].next_fire_unix;

  if (next > 0) {
    struct tm t;
    localtime_r(&next, &t);
    char tmp[32];
    strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &t);
    String s(tmp);
    if (s.length() >= 5) {
      int n = s.length();
      s = s.substring(0, n - 2) + ":" + s.substring(n - 2);
    }
    doc["next_fire_iso"] = s;
  } else {
    doc["next_fire_iso"] = "";
  }

  doc["alarm_enabled"] = a.enabled;

  JsonObject detail = doc["detail"].to<JsonObject>();
  if (!detailObj.isNull()) {
    for (JsonPairConst kv : detailObj) detail[kv.key()] = kv.value();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

static void fireOutboundEvent(const AlarmConfig& a, const String& event, const String& source, const String& url) {
  JsonDocument tmp;
  JsonObject detail = tmp.to<JsonObject>();
  String body = buildEventPayload(a, event, source, detail);
  enqueueWebhook(url, body, a.id, event);
}

/* NVS */
static String alarmKey(int i) { return "al" + String(i); }

static void saveAlarmToNvs(int i) {
  String key = alarmKey(i);
  prefs.putBytes(key.c_str(), &alarms[i], sizeof(AlarmConfig));
}

static void loadAlarmFromNvs(int i) {
  String key = alarmKey(i);
  if (!prefs.isKey(key.c_str())) {
    memset(&alarms[i], 0, sizeof(AlarmConfig));
    alarms[i].version = FW_CONFIG_VERSION;
    return;
  }

  size_t len = prefs.getBytesLength(key.c_str());
  if (len == sizeof(AlarmConfig)) {
    prefs.getBytes(key.c_str(), &alarms[i], sizeof(AlarmConfig));
  } else {
    memset(&alarms[i], 0, sizeof(AlarmConfig));
    alarms[i].version = FW_CONFIG_VERSION;
  }

  if (alarms[i].version != FW_CONFIG_VERSION) {
    uint32_t keepId = alarms[i].id;
    memset(&alarms[i], 0, sizeof(AlarmConfig));
    alarms[i].version = FW_CONFIG_VERSION;
    if (keepId != 0) alarms[i].id = keepId;
  }
}

static void loadAllFromNvs() {
  adminToken = prefs.getString("admin", "");
  int audioPin = prefs.getInt("audpin", DEFAULT_AUDIO_PWM_PIN);

  for (int i = 0; i < MAX_ALARMS; i++) loadAlarmFromNvs(i);

  audio.begin(audioPin);
  restoreLastGoodTime();
  recomputeAllNextFires();
}

static void ensurePinsConfigured() {
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].id == 0) continue;
    if (alarms[i].gpio_pin <= 0) continue;
    pinMode(alarms[i].gpio_pin, INPUT_PULLUP);
  }
}

static uint32_t genAlarmId() {
  uint32_t m = (uint32_t)millis();
  uint32_t h = (uint32_t)(chipIdU64() & 0xFFFFFFFF);
  uint32_t id = (m ^ (h * 2654435761UL)) | 1UL;
  return id;
}

/* Alarm actions */
static void stopActiveAlarm(const String& source, bool sendDismiss) {
  if (activeAlarmIndex < 0) return;

  AlarmConfig& a = alarms[activeAlarmIndex];
  AlarmRuntime& r = alarmRt[activeAlarmIndex];

  audio.stop();
  r.ringing = false;
  r.snoozed = false;
  r.snooze_until = 0;
  activeAlarmIndex = -1;

  if (sendDismiss && strlen(a.on_dismiss_url) > 0) {
    fireOutboundEvent(a, "dismissed", source, String(a.on_dismiss_url));
  }

  time_t now = time(nullptr);
  r.next_fire_unix = computeNextFire(a, now);
}

static void snoozeActiveAlarm(const String& source) {
  if (activeAlarmIndex < 0) return;
  AlarmConfig& a = alarms[activeAlarmIndex];
  AlarmRuntime& r = alarmRt[activeAlarmIndex];

  audio.stop();
  r.ringing = false;
  r.snoozed = true;

  int sm = a.snooze_minutes;
  if (sm <= 0) sm = 5;
  time_t now = time(nullptr);
  r.snooze_until = now + (time_t)sm * 60;
  r.next_fire_unix = r.snooze_until;

  if (strlen(a.on_snooze_url) > 0) {
    fireOutboundEvent(a, "snoozed", source, String(a.on_snooze_url));
  }
}

static void fireAlarmNow(int idx, const String& source, bool isScheduled) {
  if (idx < 0 || idx >= MAX_ALARMS) return;

  if (activeAlarmIndex >= 0 && activeAlarmIndex != idx) stopActiveAlarm("system", false);

  AlarmConfig& a = alarms[idx];
  AlarmRuntime& r = alarmRt[idx];

  activeAlarmIndex = idx;
  r.ringing = true;
  r.snoozed = false;
  r.snooze_until = 0;

  time_t now = time(nullptr);
  r.current_fire_unix = isScheduled ? r.next_fire_unix : now;

  a.last_fired_unix = (uint32_t)r.current_fire_unix;
  saveAlarmToNvs(idx);

  bool ok = playAlarmAudioWithFallback(a);
  if (!ok) {
    JsonDocument tmp;
    JsonObject detail = tmp.to<JsonObject>();
    detail["error"] = lastAudioError;
    String body = buildEventPayload(a, "audio_error", source, detail);
    if (strlen(a.on_fire_url) > 0) enqueueWebhook(String(a.on_fire_url), body, a.id, "audio_error");
  }

  if (strlen(a.on_fire_url) > 0) fireOutboundEvent(a, "fired", source, String(a.on_fire_url));

  if (strlen(a.once_date) == 10) {
    a.enabled = false;
    a.once_date[0] = 0;
    saveAlarmToNvs(idx);
  }

  r.next_fire_unix = computeNextFire(a, now);
}

static void schedulerTick() {
  time_t now = time(nullptr);
  if (!isValidEpoch(now)) return;

  static uint32_t lastSaveMs = 0;
  if (millis() - lastSaveMs > 60000) {
    saveLastGoodTimeIfValid();
    lastSaveMs = millis();
  }

  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].id == 0) continue;
    if (!alarms[i].enabled) continue;

    AlarmRuntime& r = alarmRt[i];
    if (r.next_fire_unix == 0) r.next_fire_unix = computeNextFire(alarms[i], now);
    if (r.next_fire_unix == 0) continue;

    if (now >= r.next_fire_unix) {
      fireAlarmNow(i, "system", true);
      break;
    }
  }
}

/* Button handling */
struct ButtonState {
  bool lastLevel = true;
  uint32_t lastChangeMs = 0;
  uint32_t pressStartMs = 0;
  bool longFired = false;
};
static ButtonState btn;

static void buttonTick() {
  if (activeAlarmIndex < 0) return;
  int pin = alarms[activeAlarmIndex].gpio_pin;
  if (pin <= 0) return;

  bool level = (digitalRead(pin) == HIGH);
  uint32_t nowMs = millis();

  if (level != btn.lastLevel) {
    if (nowMs - btn.lastChangeMs >= BUTTON_DEBOUNCE_MS) {
      btn.lastLevel = level;
      btn.lastChangeMs = nowMs;

      if (!level) {
        btn.pressStartMs = nowMs;
        btn.longFired = false;
      } else {
        if (!btn.longFired) snoozeActiveAlarm("gpio");
      }
    }
  } else {
    if (!level && !btn.longFired) {
      uint32_t lp = alarms[activeAlarmIndex].long_press_ms;
      if (lp == 0) lp = DEFAULT_LONG_PRESS_MS;
      if (nowMs - btn.pressStartMs >= lp) {
        btn.longFired = true;
        stopActiveAlarm("gpio", true);
      }
    }
  }
}

/* JSON body collector */
static std::map<AsyncWebServerRequest*, std::vector<uint8_t>> gBody;

static void attachJsonBodyCollector() {
  server.onRequestBody([](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total == 0) return;

    auto& buf = gBody[request];
    if (index == 0) {
      buf.clear();
      buf.reserve(total);
    }

    // Skydd om klient skickar mer än utlovat
    if (buf.size() + len > total) {
      buf.clear();
      return;
    }

    buf.insert(buf.end(), data, data + len);
  });
}


static void withJsonBody(AsyncWebServerRequest* req, std::function<void(JsonDocument&)> fn) {
  // Hämta buffrad body från gBody om den finns (satt i onRequestBody)
  String body;
  auto it = gBody.find(req);
  if (it != gBody.end()) {
    body = String((const char*)it->second.data(), it->second.size());
    gBody.erase(it);
  } else if (req->hasParam("plain", true)) {
    body = req->getParam("plain", true)->value();
  }

  if (body.length() == 0) { req->send(400, "application/json", "{\"error\":\"missing_body\"}"); return; }

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    req->send(400, "application/json",
              String("{\"error\":\"bad_json\",\"detail\":\"") + e.c_str() + "\"}");
    return;
  }

  fn(doc);
}

/* Upload */
struct UploadCtx {
  File file;
  size_t written = 0;
  bool ok = false;
  String path;
  String error;
};
static std::map<AsyncWebServerRequest*, UploadCtx> gUpload;

/* WiFi */
static bool connectWiFi(const String& ssid, const String& pass, uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(200);
  }
  return false;
}

static const char* wifiSetupHtml =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi Setup</title>"
"<style>body{font-family:system-ui;margin:24px;max-width:520px}label{display:block;margin-top:12px}"
"input{width:100%;padding:10px;font-size:16px}button{margin-top:16px;padding:10px 14px;font-size:16px}"
".hint{opacity:.75;font-size:13px;margin-top:10px}</style></head><body>"
"<h2>ESP32-C3 Alarmklocka</h2>"
"<p>Ange WiFi-uppgifter. Enheten startar om efter sparning.</p>"
"<form method='POST' action='/setup'>"
"<label>SSID</label><input name='ssid' required>"
"<label>Lösenord</label><input name='pass' type='password'>"
"<label>Admin token (valfritt)</label><input name='admin' placeholder='lämna tomt för ingen'>"
"<label>Audio PWM pin (valfritt)</label><input name='audpin' placeholder='t ex 5'>"
"<button type='submit'>Spara och starta om</button>"
"</form>"
"<p class='hint'>AP-läge körs på 192.168.4.1</p>"
"</body></html>";

static void startApMode() {
  wifiConnected = false;
  WiFi.mode(WIFI_AP);

  String apSsid = "AlarmClock-" + chipIdHex().substring(0, 6);
  WiFi.softAP(apSsid.c_str(), nullptr);
  Serial.printf("AP: %s IP: %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void startWiFiFlow() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length() == 0) {
    startApMode();
    return;
  }

  bool ok = connectWiFi(ssid, pass);
  if (!ok) {
    startApMode();
    return;
  }

  wifiConnected = true;
  Serial.printf("WiFi OK: %s IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
}

/* API helpers */
static void jsonAlarm(JsonObject o, const AlarmConfig& a, const AlarmRuntime& r) {
  o["id"] = a.id;
  o["enabled"] = a.enabled;
  o["label"] = a.label;
  o["hour"] = a.hour;
  o["minute"] = a.minute;
  o["days_bitmask"] = a.days_mask;
  o["once_date"] = a.once_date;
  o["snooze_minutes"] = a.snooze_minutes;
  o["gpio_pin"] = a.gpio_pin;
  o["long_press_ms"] = a.long_press_ms;
  o["volume"] = a.volume;

  JsonObject audioObj = o["audio_source"].to<JsonObject>();
  audioObj["type"] = (a.audio_type == AUDIO_URL) ? "url" : "local";
  audioObj["local_path"] = a.local_path;
  audioObj["url"] = a.url;
  audioObj["fallback_local_path"] = a.fallback_local_path;

  JsonObject wh = o["outbound_webhooks"].to<JsonObject>();
  wh["on_set_url"] = a.on_set_url;
  wh["on_fire_url"] = a.on_fire_url;
  wh["on_snooze_url"] = a.on_snooze_url;
  wh["on_dismiss_url"] = a.on_dismiss_url;

  o["next_fire_unix"] = (int64_t)r.next_fire_unix;
  o["ringing"] = r.ringing;
  o["snoozed"] = r.snoozed;
  o["snooze_until_unix"] = (int64_t)r.snooze_until;
  o["last_fired_unix"] = (int64_t)a.last_fired_unix;
}

static bool applyAlarmFromJson(AlarmConfig& a, JsonObjectConst in, String& err) {
  if (!in["label"].isNull()) strlcpy(a.label, in["label"].as<const char*>(), sizeof(a.label));
  if (!in["enabled"].isNull()) a.enabled = in["enabled"].as<bool>();
  if (!in["hour"].isNull()) a.hour = (uint8_t)in["hour"].as<int>();
  if (!in["minute"].isNull()) a.minute = (uint8_t)in["minute"].as<int>();
  if (!in["days_bitmask"].isNull()) a.days_mask = (uint8_t)in["days_bitmask"].as<int>();

  if (!in["once_date"].isNull()) {
    const char* od = in["once_date"].as<const char*>();
    if (od && strlen(od) > 0) {
      if (strlen(od) != 10) { err = "once_date_invalid"; return false; }
      strlcpy(a.once_date, od, sizeof(a.once_date));
    } else a.once_date[0] = 0;
  }

  if (!in["snooze_minutes"].isNull()) a.snooze_minutes = (int16_t)in["snooze_minutes"].as<int>();
  if (!in["gpio_pin"].isNull()) a.gpio_pin = (int8_t)in["gpio_pin"].as<int>();
  if (!in["long_press_ms"].isNull()) a.long_press_ms = (uint16_t)in["long_press_ms"].as<int>();
  if (!in["inbound_webhook_token"].isNull()) strlcpy(a.inbound_token, in["inbound_webhook_token"].as<const char*>(), sizeof(a.inbound_token));
  if (!in["volume"].isNull()) a.volume = (uint8_t)in["volume"].as<int>();

  if (!in["outbound_webhooks"].isNull()) {
    JsonObjectConst wh = in["outbound_webhooks"].as<JsonObjectConst>();
    if (!wh.isNull()) {
      if (!wh["on_set_url"].isNull()) strlcpy(a.on_set_url, wh["on_set_url"].as<const char*>(), sizeof(a.on_set_url));
      if (!wh["on_fire_url"].isNull()) strlcpy(a.on_fire_url, wh["on_fire_url"].as<const char*>(), sizeof(a.on_fire_url));
      if (!wh["on_snooze_url"].isNull()) strlcpy(a.on_snooze_url, wh["on_snooze_url"].as<const char*>(), sizeof(a.on_snooze_url));
      if (!wh["on_dismiss_url"].isNull()) strlcpy(a.on_dismiss_url, wh["on_dismiss_url"].as<const char*>(), sizeof(a.on_dismiss_url));
    }
  }

  if (!in["audio_source"].isNull()) {
    JsonObjectConst as = in["audio_source"].as<JsonObjectConst>();
    if (!as.isNull()) {
      if (!as["type"].isNull()) {
        String t = as["type"].as<String>(); t.toLowerCase();
        a.audio_type = (t == "url") ? AUDIO_URL : AUDIO_LOCAL;
      }
      if (!as["local_path"].isNull()) strlcpy(a.local_path, as["local_path"].as<const char*>(), sizeof(a.local_path));
      if (!as["url"].isNull()) strlcpy(a.url, as["url"].as<const char*>(), sizeof(a.url));
      if (!as["fallback_local_path"].isNull()) strlcpy(a.fallback_local_path, as["fallback_local_path"].as<const char*>(), sizeof(a.fallback_local_path));
    }
  }

  if (a.hour > 23 || a.minute > 59) { err = "time_invalid"; return false; }
  if (a.snooze_minutes < 0 || a.snooze_minutes > 240) { err = "snooze_invalid"; return false; }
  if (a.volume > 100) a.volume = 100;

  return true;
}

/* API handlers */
static void handleStatus(AsyncWebServerRequest* req) {
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["fw_version"] = FW_CONFIG_VERSION;

  doc["wifi_connected"] = wifiConnected;
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["rssi"] = wifiConnected ? WiFi.RSSI() : 0;

  time_t now = time(nullptr);
  doc["time_valid"] = isValidEpoch(now);
  doc["ntp_synced"] = ntpSynced;
  doc["ts_iso"] = isoNow();
  doc["ts_unix"] = (int64_t)now;

  doc["active_alarm_id"] = (activeAlarmIndex >= 0) ? (int64_t)alarms[activeAlarmIndex].id : 0;
  doc["audio_playing"] = audio.isPlaying();
  doc["last_audio_error"] = lastAudioError;

  JsonObject fs = doc["littlefs"].to<JsonObject>();
  fs["total"] = (int64_t)LittleFS.totalBytes();
  fs["used"] = (int64_t)LittleFS.usedBytes();
  fs["free"] = (int64_t)(LittleFS.totalBytes() - LittleFS.usedBytes());

  JsonObject wh = doc["last_webhook"].to<JsonObject>();
  wh["http_status"] = lastWebhookGlobal.httpStatus;
  wh["error"] = lastWebhookGlobal.error;
  wh["ts_unix"] = (int64_t)lastWebhookGlobal.ts;

  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleGetAlarms(AsyncWebServerRequest* req) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].id == 0) continue;
    JsonObject o = arr.add<JsonObject>();
    jsonAlarm(o, alarms[i], alarmRt[i]);
  }
  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleGetAlarmById(AsyncWebServerRequest* req, uint32_t id) {
  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  jsonAlarm(o, alarms[idx], alarmRt[idx]);
  o["inbound_webhook_token"] = alarms[idx].inbound_token;
  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handlePostAlarm(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;
  withJsonBody(req, [&](JsonDocument& doc) {
    JsonObjectConst in = doc.as<JsonObjectConst>();

    int freeIdx = -1;
    for (int i = 0; i < MAX_ALARMS; i++) if (alarms[i].id == 0) { freeIdx = i; break; }
    if (freeIdx < 0) { req->send(409, "application/json", "{\"error\":\"max_alarms\"}"); return; }

    AlarmConfig a {};
    a.version = FW_CONFIG_VERSION;
    a.id = genAlarmId();
    a.enabled = true;
    strlcpy(a.label, "Alarm", sizeof(a.label));
    a.hour = 7;
    a.minute = 30;
    a.days_mask = 0x1F;
    a.snooze_minutes = 5;
    a.gpio_pin = 0;
    a.long_press_ms = 0;
    a.audio_type = AUDIO_LOCAL;
    strlcpy(a.local_path, "/audio/default.wav", sizeof(a.local_path));
    a.volume = 80;

    String err;
    if (!applyAlarmFromJson(a, in, err)) {
      req->send(400, "application/json", String("{\"error\":\"") + err + "\"}");
      return;
    }

    alarms[freeIdx] = a;
    alarmRt[freeIdx].next_fire_unix = computeNextFire(alarms[freeIdx], time(nullptr));
    saveAlarmToNvs(freeIdx);
    ensurePinsConfigured();

    if (strlen(a.on_set_url) > 0) fireOutboundEvent(a, "set", "webgui", String(a.on_set_url));

    JsonDocument outDoc;
    outDoc["id"] = a.id;
    String out; serializeJson(outDoc, out);
    req->send(201, "application/json", out);
  });
}

static void handlePutAlarm(AsyncWebServerRequest* req, uint32_t id) {
  if (!requireAdmin(req)) return;

  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  withJsonBody(req, [&](JsonDocument& doc) {
    JsonObjectConst in = doc.as<JsonObjectConst>();
    String err;
    if (!applyAlarmFromJson(alarms[idx], in, err)) {
      req->send(400, "application/json", String("{\"error\":\"") + err + "\"}");
      return;
    }

    saveAlarmToNvs(idx);
    ensurePinsConfigured();
    alarmRt[idx].next_fire_unix = computeNextFire(alarms[idx], time(nullptr));

    if (strlen(alarms[idx].on_set_url) > 0) fireOutboundEvent(alarms[idx], "set", "webgui", String(alarms[idx].on_set_url));
    req->send(200, "application/json", "{\"ok\":true}");
  });
}

static void handleDeleteAlarm(AsyncWebServerRequest* req, uint32_t id) {
  if (!requireAdmin(req)) return;

  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  memset(&alarms[idx], 0, sizeof(AlarmConfig));
  alarmRt[idx] = AlarmRuntime{};
  saveAlarmToNvs(idx);
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleEnableDisable(AsyncWebServerRequest* req, uint32_t id, bool en) {
  if (!requireAdmin(req)) return;

  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  alarms[idx].enabled = en;
  saveAlarmToNvs(idx);
  alarmRt[idx].next_fire_unix = computeNextFire(alarms[idx], time(nullptr));

  String ev = en ? "enabled" : "disabled";
  if (strlen(alarms[idx].on_set_url) > 0) fireOutboundEvent(alarms[idx], ev, "webgui", String(alarms[idx].on_set_url));

  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleSnoozeDismissFire(AsyncWebServerRequest* req, uint32_t id, const String& action) {
  if (!requireAdmin(req)) return;

  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  if (action == "fire") {
    fireAlarmNow(idx, "webgui", false);
    req->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (activeAlarmIndex < 0 || alarms[activeAlarmIndex].id != id) {
    req->send(409, "application/json", "{\"error\":\"not_ringing\"}");
    return;
  }

  if (action == "snooze") { snoozeActiveAlarm("webgui"); req->send(200, "application/json", "{\"ok\":true}"); return; }
  if (action == "dismiss") { stopActiveAlarm("webgui", true); req->send(200, "application/json", "{\"ok\":true}"); return; }

  req->send(400, "application/json", "{\"error\":\"bad_action\"}");
}

static void handleTestAudio(AsyncWebServerRequest* req, uint32_t id) {
  if (!requireAdmin(req)) return;

  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  bool ok = playAlarmAudioWithFallback(alarms[idx]);
  JsonDocument doc;
  doc["ok"] = ok;
  doc["last_audio_error"] = lastAudioError;
  String out; serializeJson(doc, out);
  req->send(ok ? 200 : 500, "application/json", out);
}

static void handleFilesList(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  File root = LittleFS.open("/audio", "r");
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = String(f.name()).substring(String("/audio/").length());
        o["path"] = String(f.name());
        o["size"] = (int64_t)f.size();
      }
      f = root.openNextFile();
    }
  }

  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleFilesSpace(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;

  JsonDocument doc;
  doc["total"] = (int64_t)LittleFS.totalBytes();
  doc["used"] = (int64_t)LittleFS.usedBytes();
  doc["free"] = (int64_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleFilesDelete(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;

  if (!req->hasParam("path")) { req->send(400, "application/json", "{\"error\":\"missing_path\"}"); return; }
  String path = req->getParam("path")->value();
  if (!path.startsWith("/audio/")) { req->send(400, "application/json", "{\"error\":\"bad_path\"}"); return; }
  if (!fileExists(path.c_str())) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }
  if (isFileUsedByAnyAlarm(path)) { req->send(409, "application/json", "{\"error\":\"file_in_use\"}"); return; }

  bool ok = LittleFS.remove(path);
  req->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"delete_failed\"}");
}

static void handleConfigExport(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;

  JsonDocument doc;
  doc["device_id"] = deviceId;

  JsonObject sys = doc["system"].to<JsonObject>();
  sys["admin_token"] = adminToken;
  sys["audio_pwm_pin"] = prefs.getInt("audpin", DEFAULT_AUDIO_PWM_PIN);
  sys["wifi_ssid"] = prefs.getString("ssid", "");
  sys["wifi_pass"] = prefs.getString("pass", "");

  JsonArray arr = doc["alarms"].to<JsonArray>();
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (alarms[i].id == 0) continue;
    JsonObject o = arr.add<JsonObject>();
    jsonAlarm(o, alarms[i], alarmRt[i]);
    o["inbound_webhook_token"] = alarms[i].inbound_token;
  }

  String out; serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleConfigImport(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;

  withJsonBody(req, [&](JsonDocument& doc) {
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) { req->send(400, "application/json", "{\"error\":\"bad_json\"}"); return; }

    if (!root["system"].isNull()) {
      JsonObjectConst sys = root["system"].as<JsonObjectConst>();
      if (!sys.isNull()) {
        if (!sys["admin_token"].isNull()) {
          adminToken = sys["admin_token"].as<String>();
          prefs.putString("admin", adminToken);
        }
        if (!sys["audio_pwm_pin"].isNull()) {
          int pin = sys["audio_pwm_pin"].as<int>();
          prefs.putInt("audpin", pin);
          audio.begin(pin);
        }
        if (!sys["wifi_ssid"].isNull()) prefs.putString("ssid", sys["wifi_ssid"].as<const char*>());
        if (!sys["wifi_pass"].isNull()) prefs.putString("pass", sys["wifi_pass"].as<const char*>());
      }
    }

    for (int i = 0; i < MAX_ALARMS; i++) {
      memset(&alarms[i], 0, sizeof(AlarmConfig));
      alarms[i].version = FW_CONFIG_VERSION;
      saveAlarmToNvs(i);
    }

    if (!root["alarms"].isNull()) {
      JsonArrayConst arr = root["alarms"].as<JsonArrayConst>();
      int idx = 0;
      for (JsonObjectConst aIn : arr) {
        if (idx >= MAX_ALARMS) break;

        AlarmConfig a {};
        a.version = FW_CONFIG_VERSION;
        a.id = aIn["id"].as<uint32_t>();
        if (a.id == 0) a.id = genAlarmId();

        if (!aIn["inbound_webhook_token"].isNull()) {
          strlcpy(a.inbound_token, aIn["inbound_webhook_token"].as<const char*>(), sizeof(a.inbound_token));
        }

        String err;
        if (!applyAlarmFromJson(a, aIn, err)) continue;

        alarms[idx] = a;
        saveAlarmToNvs(idx);
        idx++;
      }
    }

    ensurePinsConfigured();
    recomputeAllNextFires();
    req->send(200, "application/json", "{\"ok\":true}");
  });
}

static void handleRestart(AsyncWebServerRequest* req) {
  if (!requireAdmin(req)) return;
  req->send(200, "application/json", "{\"ok\":true}");
  delay(300);
  ESP.restart();
}

/* Inbound webhook /wh/alarm/{id}?token=... */
static void handleAlarmWebhook(AsyncWebServerRequest* req, uint32_t id) {
  int idx = findAlarmIndexById(id);
  if (idx < 0) { req->send(404, "application/json", "{\"error\":\"not_found\"}"); return; }

  if (!req->hasParam("token")) { req->send(401, "application/json", "{\"error\":\"missing_token\"}"); return; }
  String token = req->getParam("token")->value();
  if (token != String(alarms[idx].inbound_token)) { req->send(401, "application/json", "{\"error\":\"bad_token\"}"); return; }

  withJsonBody(req, [&](JsonDocument& doc) {
    JsonObjectConst in = doc.as<JsonObjectConst>();
    String action = in["action"].as<String>(); action.toLowerCase();

    if (action == "set") {
      String err;
      if (!applyAlarmFromJson(alarms[idx], in, err)) { req->send(400, "application/json", String("{\"error\":\"") + err + "\"}"); return; }
      saveAlarmToNvs(idx);
      alarmRt[idx].next_fire_unix = computeNextFire(alarms[idx], time(nullptr));
      if (strlen(alarms[idx].on_set_url) > 0) fireOutboundEvent(alarms[idx], "set", "webhook", String(alarms[idx].on_set_url));
      req->send(200, "application/json", "{\"ok\":true}");
      return;
    }

    if (action == "enable") { alarms[idx].enabled = true; saveAlarmToNvs(idx); alarmRt[idx].next_fire_unix = computeNextFire(alarms[idx], time(nullptr));
      if (strlen(alarms[idx].on_set_url) > 0) fireOutboundEvent(alarms[idx], "enabled", "webhook", String(alarms[idx].on_set_url));
      req->send(200, "application/json", "{\"ok\":true}"); return;
    }

    if (action == "disable") { alarms[idx].enabled = false; saveAlarmToNvs(idx); alarmRt[idx].next_fire_unix = 0;
      if (strlen(alarms[idx].on_set_url) > 0) fireOutboundEvent(alarms[idx], "disabled", "webhook", String(alarms[idx].on_set_url));
      req->send(200, "application/json", "{\"ok\":true}"); return;
    }

    if (action == "fire") { fireAlarmNow(idx, "webhook", false); req->send(200, "application/json", "{\"ok\":true}"); return; }

    if (action == "snooze") {
      if (activeAlarmIndex >= 0 && alarms[activeAlarmIndex].id == id) { snoozeActiveAlarm("webhook"); req->send(200, "application/json", "{\"ok\":true}"); }
      else req->send(409, "application/json", "{\"error\":\"not_ringing\"}");
      return;
    }

    if (action == "dismiss") {
      if (activeAlarmIndex >= 0 && alarms[activeAlarmIndex].id == id) { stopActiveAlarm("webhook", true); req->send(200, "application/json", "{\"ok\":true}"); }
      else req->send(409, "application/json", "{\"error\":\"not_ringing\"}");
      return;
    }

    req->send(400, "application/json", "{\"error\":\"bad_action\"}");
  });
}

/* Routing helper */
static bool parseIdFromPath(const String& path, const char* prefix, uint32_t& idOut) {
  if (!path.startsWith(prefix)) return false;
  String rest = path.substring(strlen(prefix));
  if (rest.length() == 0) return false;
  int slash = rest.indexOf('/');
  String idStr = (slash >= 0) ? rest.substring(0, slash) : rest;
  idOut = (uint32_t)idStr.toInt();
  return idOut != 0;
}

/* Default audio */
static void ensureDefaultAudio() {
  if (!LittleFS.exists("/audio")) LittleFS.mkdir("/audio");
  if (!LittleFS.exists("/audio/default.wav")) {
    File f = LittleFS.open("/audio/default.wav", "w");
    if (f) {
      const uint32_t sr = 16000;
      const uint16_t ch = 1;
      const uint16_t bits = 16;
      const uint32_t samples = sr * 1;
      const uint32_t dataSize = samples * 2;

      uint8_t h[44] = {0};
      memcpy(h + 0, "RIFF", 4);
      uint32_t riffSize = 36 + dataSize;
      h[4] = riffSize & 0xFF; h[5] = (riffSize >> 8) & 0xFF; h[6] = (riffSize >> 16) & 0xFF; h[7] = (riffSize >> 24) & 0xFF;
      memcpy(h + 8, "WAVE", 4);
      memcpy(h + 12, "fmt ", 4);
      uint32_t fmtSize = 16;
      h[16] = fmtSize & 0xFF; h[17] = (fmtSize >> 8) & 0xFF; h[18] = (fmtSize >> 16) & 0xFF; h[19] = (fmtSize >> 24) & 0xFF;
      uint16_t audioFmt = 1;
      h[20] = audioFmt & 0xFF; h[21] = (audioFmt >> 8) & 0xFF;
      h[22] = ch & 0xFF; h[23] = (ch >> 8) & 0xFF;
      h[24] = sr & 0xFF; h[25] = (sr >> 8) & 0xFF; h[26] = (sr >> 16) & 0xFF; h[27] = (sr >> 24) & 0xFF;
      uint32_t byteRate = sr * ch * (bits / 8);
      h[28] = byteRate & 0xFF; h[29] = (byteRate >> 8) & 0xFF; h[30] = (byteRate >> 16) & 0xFF; h[31] = (byteRate >> 24) & 0xFF;
      uint16_t blockAlign = ch * (bits / 8);
      h[32] = blockAlign & 0xFF; h[33] = (blockAlign >> 8) & 0xFF;
      h[34] = bits & 0xFF; h[35] = (bits >> 8) & 0xFF;
      memcpy(h + 36, "data", 4);
      h[40] = dataSize & 0xFF; h[41] = (dataSize >> 8) & 0xFF; h[42] = (dataSize >> 16) & 0xFF; h[43] = (dataSize >> 24) & 0xFF;

      f.write(h, 44);
      uint8_t zeros[512] = {0};
      uint32_t rem = dataSize;
      while (rem > 0) {
        uint32_t n = min((uint32_t)sizeof(zeros), rem);
        f.write(zeros, n);
        rem -= n;
      }
      f.close();
    }
  }
}

static void ensureAtLeastOneAlarm() {
  bool any = false;
  for (int i = 0; i < MAX_ALARMS; i++) if (alarms[i].id != 0) any = true;
  if (any) return;

  AlarmConfig a {};
  a.version = FW_CONFIG_VERSION;
  a.id = genAlarmId();
  a.enabled = false;
  strlcpy(a.label, "Vardagar", sizeof(a.label));
  a.hour = 7;
  a.minute = 30;
  a.days_mask = 0x1F;
  a.snooze_minutes = 5;
  a.gpio_pin = 0;
  a.long_press_ms = 0;
  a.audio_type = AUDIO_LOCAL;
  strlcpy(a.local_path, "/audio/default.wav", sizeof(a.local_path));
  a.volume = 80;

  alarms[0] = a;
  saveAlarmToNvs(0);
  alarmRt[0].next_fire_unix = computeNextFire(alarms[0], time(nullptr));
}

/* Server */
static void setupServer() {
  // WiFi setup UI (always available)
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", wifiSetupHtml);
  });

  server.on("/setup", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid", true)) { req->send(400, "text/plain", "Missing ssid"); return; }
    String ssid = req->getParam("ssid", true)->value();
    String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";
    String admin = req->hasParam("admin", true) ? req->getParam("admin", true)->value() : "";
    String audpinStr = req->hasParam("audpin", true) ? req->getParam("audpin", true)->value() : "";
    int audpin = audpinStr.length() ? audpinStr.toInt() : prefs.getInt("audpin", DEFAULT_AUDIO_PWM_PIN);

    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("admin", admin);
    prefs.putInt("audpin", audpin);

    req->send(200, "text/plain", "Sparat. Startar om...");
    delay(400);
    ESP.restart();
  });

  // Static UI from LittleFS
  //server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
  req->send(LittleFS, "/index.html", "text/html");
});

server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
  req->send(LittleFS, "/style.css", "text/css");
});

server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* req) {
  req->send(LittleFS, "/app.js", "application/javascript");
});


  // API
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/alarms", HTTP_GET, handleGetAlarms);

  // VIKTIGT: POST /api/alarms med body-callback (annars missing_body)
  server.on("/api/alarms", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      // Tom avsiktligt. Vi svarar när bodyn är komplett i body-callbacken nedan.
    },
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      auto& buf = gBody[req];
      if (index == 0) { buf.clear(); buf.reserve(total); }
      buf.insert(buf.end(), data, data + len);

      if (index + len == total) {
        // Nu är JSON-body komplett, så handlePostAlarm -> withJsonBody kan läsa gBody
        handlePostAlarm(req);
      }
    }
  );

  server.on("/api/files", HTTP_GET, handleFilesList);
  server.on("/api/files/space", HTTP_GET, handleFilesSpace);
  server.on("/api/files", HTTP_DELETE, handleFilesDelete);

  // Upload (din befintliga kod kan vara kvar oförändrad)
  server.on("/api/files/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      if (!requireAdmin(req)) return;

      auto it = gUpload.find(req);
      bool ok = (it != gUpload.end() && it->second.ok);
      String err = (it != gUpload.end()) ? it->second.error : "no_ctx";
      if (it != gUpload.end()) gUpload.erase(it);

      if (ok) req->send(200, "application/json", "{\"ok\":true}");
      else req->send(400, "application/json", String("{\"error\":\"upload_failed\",\"detail\":\"") + err + "\"}");
    },
    [](AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
      if (adminToken.length() && (!req->hasHeader("X-Admin-Token") || req->getHeader("X-Admin-Token")->value() != adminToken)) {
        return;
      }

      auto& ctx = gUpload[req];

      if (index == 0) {
        ctx.ok = false;
        ctx.error = "";
        ctx.written = 0;

        String clean = sanitizeFileName(filename);
        if (!hasAllowedExt(clean)) { ctx.error = "bad_ext"; return; }

        if (!LittleFS.exists("/audio")) LittleFS.mkdir("/audio");
        ctx.path = "/audio/" + clean;

        ctx.file = LittleFS.open(ctx.path, "w");
        if (!ctx.file) { ctx.error = "open_failed"; return; }
      }

      if (!ctx.file) { if (ctx.error.length() == 0) ctx.error = "no_file"; return; }
      if ((ctx.written + len) > MAX_UPLOAD_BYTES) {
        ctx.error = "too_large";
        ctx.file.close();
        LittleFS.remove(ctx.path);
        return;
      }

      size_t w = ctx.file.write(data, len);
      ctx.written += w;

      if (final) {
        ctx.file.close();
        ctx.ok = (ctx.error.length() == 0);
      }
    }
  );

  server.on("/api/config/export", HTTP_GET, handleConfigExport);

  // config/import (om du vill kunna importera JSON)
  server.on("/api/config/import", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      auto& buf = gBody[req];
      if (index == 0) { buf.clear(); buf.reserve(total); }
      buf.insert(buf.end(), data, data + len);
      if (index + len == total) {
        handleConfigImport(req);
      }
    }
  );

  server.on("/api/system/restart", HTTP_POST, handleRestart);

  server.onNotFound([](AsyncWebServerRequest* req) {
    String path = req->url();

    // En enkel CORS/OPTIONS-svar för alla vägar
    if (req->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse* r = req->beginResponse(204);
      r->addHeader("Access-Control-Allow-Origin", "*");
      r->addHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
      r->addHeader("Access-Control-Allow-Headers", "Content-Type,X-Admin-Token");
      req->send(r);
      return;
    }

    // Dynamiska API-routes för alarm och webhooks
    if (path.startsWith("/api/alarms/")) {
      String rest = path.substring(strlen("/api/alarms/"));
      int slash = rest.indexOf('/');
      String idStr = (slash >= 0) ? rest.substring(0, slash) : rest;
      uint32_t id = (uint32_t)idStr.toInt();
        String suffix = (slash >= 0) ? rest.substring(slash) : "";

      if (id != 0) {
        if (suffix.length() == 0 || suffix == "/") {
          if (req->method() == HTTP_GET) { handleGetAlarmById(req, id); return; }
          if (req->method() == HTTP_PUT) { handlePutAlarm(req, id); return; }
          if (req->method() == HTTP_DELETE) { handleDeleteAlarm(req, id); return; }
        }
        if (suffix == "/enable" && req->method() == HTTP_POST) { handleEnableDisable(req, id, true); return; }
        if (suffix == "/disable" && req->method() == HTTP_POST) { handleEnableDisable(req, id, false); return; }
        if (suffix == "/snooze" && req->method() == HTTP_POST) { handleSnoozeDismissFire(req, id, "snooze"); return; }
        if (suffix == "/dismiss" && req->method() == HTTP_POST) { handleSnoozeDismissFire(req, id, "dismiss"); return; }
        if (suffix == "/fire" && req->method() == HTTP_POST) { handleSnoozeDismissFire(req, id, "fire"); return; }
        if (suffix == "/test_audio" && req->method() == HTTP_POST) { handleTestAudio(req, id); return; }
      }
    }

    if (path.startsWith("/wh/alarm/") && req->method() == HTTP_POST) {
      String rest = path.substring(strlen("/wh/alarm/"));
      uint32_t id = (uint32_t)rest.toInt();
      if (id != 0) { handleAlarmWebhook(req, id); return; }
    }

    // Friendly default when UI missing
    if (path == "/" || path == "/index.html") {
      req->send(200, "text/html",
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AlarmClock</title></head><body>"
        "<h3>UI saknas i LittleFS</h3>"
        "<p>Kör uploadfs i PlatformIO. WiFi setup finns på <a href='/wifi'>/wifi</a>.</p>"
        "</body></html>"
      );
      return;
    }

    req->send(404, "text/plain", "Not found");
  });

  attachJsonBodyCollector();
  server.begin();
}


/* setup/loop */
void setup() {
  Serial.begin(115200);
  delay(150);

  deviceId = "esp32c3-" + chipIdHex().substring(0, 12);

  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed");
  }

  prefs.begin("alarmclk", false);

  // Sätt TZ tidigt (Europe/Stockholm)
  setupTimezone();

  loadAllFromNvs();
  ensureDefaultAudio();
  ensureAtLeastOneAlarm();
  ensurePinsConfigured();

  startWiFiFlow();

  if (wifiConnected) {
    startNtp();

    // Vänta in första synken lite mer robust än 200ms
    struct tm tminfo;
    bool ok = false;
    for (int i = 0; i < 30; i++) {               // max ~15s
      if (getLocalTime(&tminfo, 500)) { ok = true; break; }
      delay(500);
    }

    // Uppdatera status och "värm upp" TZ-konvertering
    updateNtpStatus();
    tzset();
    time_t now = time(nullptr);
    struct tm tmp;
    localtime_r(&now, &tmp);

    Serial.printf("NTP ok=%d ntpSynced=%d epoch=%lld local=%s\n",
                  ok ? 1 : 0,
                  ntpSynced ? 1 : 0,
                  (long long)now,
                  isoNow().c_str());
  } else {
    // AP mode: time may be invalid, but UI+API still works
    ntpSynced = false;
  }

  setupServer();

  Serial.printf("Device: %s\n", deviceId.c_str());
  Serial.printf("Admin token set: %s\n", adminToken.length() ? "yes" : "no");
}

void loop() {
  if (wifiConnected) {
    static uint32_t lastNtpCheckMs = 0;
    if (millis() - lastNtpCheckMs > 5000) {
      updateNtpStatus();
      lastNtpCheckMs = millis();
    }
  }

  schedulerTick();
  buttonTick();
  audio.loop();
  processWebhookQueue();

  delay(5);
}
