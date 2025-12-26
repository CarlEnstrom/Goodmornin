#include "audio.h"
#include "alarms.h"

static const int AUDIO_LEDC_CHANNEL = 0;
static const int AUDIO_LEDC_RES_BITS = 8;
static const uint32_t AUDIO_PWM_CARRIER_HZ = 100000;

static const int SR_11025 = 11025;
static const int SR_16000 = 16000;
static const int SR_22050 = 22050;

String lastAudioError;
AudioPlayer audio;

class AudioPlayer::StreamHolder {
public:
  StreamHolder(HTTPClient* h, WiFiClient* c) : http(h), client(c) {}
  ~StreamHolder() { stop(); }
  void stop() {
    if (http) { http->end(); delete http; http = nullptr; }
    if (client) { delete client; client = nullptr; }
  }
  HTTPClient* http = nullptr;
  WiFiClient* client = nullptr;
};

static void ledcInitCarrier(int pin, int channel, uint32_t freqHz, int resBits) {
#if defined(ARDUINO_ESP32_MAJOR) && (ARDUINO_ESP32_MAJOR >= 3)
  ledcAttachChannel(pin, freqHz, resBits, channel);
#else
  ledcSetup(channel, freqHz, resBits);
  ledcAttachPin(pin, channel);
#endif
}

static void ledcWriteDuty(int channel, uint32_t duty) {
  ledcWrite(channel, duty);
}

void AudioPlayer::begin(int pwmPin) {
  audioPin = pwmPin;
  ledcInitCarrier(audioPin, AUDIO_LEDC_CHANNEL, AUDIO_PWM_CARRIER_HZ, AUDIO_LEDC_RES_BITS);
  writeDutyMid();
  setSampleRate(SR_16000);
}

void AudioPlayer::setSampleRate(int sr) {
  if (sr != SR_11025 && sr != SR_16000 && sr != SR_22050) sr = SR_16000;
  sampleRate = sr;
  if (timer) {
    esp_timer_stop(timer);
    esp_timer_start_periodic(timer, 1000000ULL / (uint64_t)sampleRate);
  }
}

bool AudioPlayer::isPlaying() const { return playing; }
String AudioPlayer::lastError() const { return lastErr; }

void AudioPlayer::stop() {
  playing = false;
  if (stream) {
    stream->stop();
    delete stream;
    stream = nullptr;
  }
  if (file) file.close();
  rbReset();
  writeDutyMid();
}

bool AudioPlayer::playLocal(const String& path, uint8_t vol) {
  stop();
  volume = vol;

  String p = path;
  if (!p.startsWith("/")) p = "/" + p;
  File f = LittleFS.open(p, "r");
  if (!f) { lastErr = "file_not_found"; return false; }
  file = f;

  String lp = p; lp.toLowerCase();
  if (lp.endsWith(".wav")) {
    if (!wavReadHeaderFromFile()) { file.close(); return false; }
    startTimer();
    playing = true;
    sourceKind = "wav_file";
    return true;
  }

  if (lp.endsWith(".mp3")) {
    // Stub decoder
    mp3dec = {};
    mp3InFilled = 0;
    startTimer();
    playing = true;
    sourceKind = "mp3_file";
    return true;
  }

  file.close();
  lastErr = "unsupported_ext";
  return false;
}

bool AudioPlayer::playUrl(const String& url, uint8_t vol) {
  stop();
  volume = vol;

  HTTPClient* http = new HTTPClient();
  WiFiClient* cli = nullptr;
  WiFiClientSecure* scli = nullptr;

  bool secure = url.startsWith("https://") || url.startsWith("HTTPS://");
  if (secure) {
    scli = new WiFiClientSecure();
    scli->setInsecure();
    if (!http->begin(*scli, url)) {
      delete http; delete scli;
      lastErr = "http_begin_failed";
      return false;
    }
  } else {
    cli = new WiFiClient();
    if (!http->begin(*cli, url)) {
      delete http; delete cli;
      lastErr = "http_begin_failed";
      return false;
    }
  }

  int code = http->GET();
  if (code <= 0) {
    http->end();
    delete http; if (scli) delete scli; if (cli) delete cli;
    lastErr = "http_get_failed";
    return false;
  }
  if (code != 200) {
    http->end();
    delete http; if (scli) delete scli; if (cli) delete cli;
    lastErr = "http_status_" + String(code);
    return false;
  }

  stream = new StreamHolder(http, secure ? (WiFiClient*)scli : cli);
  if (!stream || !stream->client) {
    stop();
    lastErr = "http_no_stream";
    return false;
  }

  String u = url; u.toLowerCase();
  if (u.indexOf(".wav") > 0) {
    if (!wavReadHeaderFromStream()) { stop(); return false; }
    startTimer();
    playing = true;
    sourceKind = "wav_url";
    return true;
  }
  if (u.indexOf(".mp3") > 0) {
    mp3dec = {};
    mp3InFilled = 0;
    startTimer();
    playing = true;
    sourceKind = "mp3_url";
    return true;
  }

  if (wavReadHeaderFromStream()) {
    startTimer();
    playing = true;
    sourceKind = "wav_url_guess";
    return true;
  }

  lastErr = "unknown_url_format";
  stop();
  return false;
}

void AudioPlayer::loop() {
  if (!playing) return;

  while (rbCount < (RB_CAP / 2)) {
    if (sourceKind.startsWith("wav")) {
      if (!fillWav()) break;
    } else if (sourceKind.startsWith("mp3")) {
      if (!fillMp3()) break;
    } else {
      lastErr = "internal_source_unknown";
      stop();
      break;
    }
  }

  if (inputEnded && rbCount == 0) stop();
}

void AudioPlayer::startTimer() {
  inputEnded = false;
  if (!timer) {
    esp_timer_create_args_t args {};
    args.callback = &AudioPlayer::timerThunk;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "audio";
    esp_timer_create(&args, &timer);
  }
  esp_timer_start_periodic(timer, 1000000ULL / (uint64_t)sampleRate);
}

void AudioPlayer::writeDutyMid() {
  int maxDuty = (1 << AUDIO_LEDC_RES_BITS) - 1;
  ledcWriteDuty(AUDIO_LEDC_CHANNEL, maxDuty / 2);
}

void AudioPlayer::timerThunk(void* arg) { static_cast<AudioPlayer*>(arg)->onTick(); }

void AudioPlayer::onTick() {
  if (!playing) { writeDutyMid(); return; }

  int16_t s = 0;
  if (rbPop(s)) {
    int32_t v = (int32_t)s;
    v = (v * (int32_t)volume) / 100;

    int maxDuty = (1 << AUDIO_LEDC_RES_BITS) - 1;
    int32_t u = v + 32768;
    if (u < 0) u = 0;
    if (u > 65535) u = 65535;

    int duty = (int)(u >> (16 - AUDIO_LEDC_RES_BITS));
    if (duty < 0) duty = 0;
    if (duty > maxDuty) duty = maxDuty;
    ledcWriteDuty(AUDIO_LEDC_CHANNEL, (uint32_t)duty);
  } else {
    writeDutyMid();
  }
}

void AudioPlayer::rbReset() { rbHead = rbTail = rbCount = 0; }

bool AudioPlayer::rbPush(int16_t s) {
  if (rbCount >= RB_CAP) return false;
  rb[rbHead] = s;
  rbHead = (rbHead + 1) % RB_CAP;
  rbCount++;
  return true;
}

bool AudioPlayer::rbPop(int16_t& s) {
  if (rbCount <= 0) return false;
  s = rb[rbTail];
  rbTail = (rbTail + 1) % RB_CAP;
  rbCount--;
  return true;
}

bool AudioPlayer::readBytes(File& f, uint8_t* buf, size_t n) {
  size_t r = f.read(buf, n);
  return r == n;
}

bool AudioPlayer::readBytesStream(WiFiClient& s, uint8_t* buf, size_t n, uint32_t timeoutMs) {
  uint32_t start = millis();
  size_t got = 0;
  while (got < n && (millis() - start) < timeoutMs) {
    int av = s.available();
    if (av > 0) {
      int r = s.read(buf + got, (int)(n - got));
      if (r > 0) got += (size_t)r;
    } else {
      if (!s.connected()) break;
      delay(1);
    }
  }
  return got == n;
}

uint32_t AudioPlayer::readLE32(const uint8_t* b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
uint16_t AudioPlayer::readLE16(const uint8_t* b) {
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

bool AudioPlayer::wavParseHeader(uint8_t header[44]) {
  if (memcmp(header, "RIFF", 4) != 0) { lastErr = "wav_not_riff"; return false; }
  if (memcmp(header + 8, "WAVE", 4) != 0) { lastErr = "wav_not_wave"; return false; }
  if (memcmp(header + 12, "fmt ", 4) != 0) { lastErr = "wav_no_fmt"; return false; }

  uint16_t audioFmt = readLE16(header + 20);
  wavChannels = readLE16(header + 22);
  wavSampleRate = readLE32(header + 24);
  wavBits = readLE16(header + 34);

  if (audioFmt != 1) { lastErr = "wav_not_pcm"; return false; }
  if (wavBits != 16) { lastErr = "wav_bits_not_16"; return false; }
  if (wavChannels < 1 || wavChannels > 2) { lastErr = "wav_channels_bad"; return false; }

  if (memcmp(header + 36, "data", 4) != 0) { lastErr = "wav_no_data"; return false; }
  uint32_t dataSize = readLE32(header + 40);
  wavDataRemaining = dataSize;

  if (wavSampleRate == (uint32_t)SR_11025 || wavSampleRate == (uint32_t)SR_16000 || wavSampleRate == (uint32_t)SR_22050) {
    setSampleRate((int)wavSampleRate);
  } else {
    setSampleRate(SR_16000);
  }

  wavOk = true;
  return true;
}

bool AudioPlayer::wavReadHeaderFromFile() {
  uint8_t h[44];
  if (!readBytes(file, h, 44)) { lastErr = "wav_header_read_fail"; return false; }
  return wavParseHeader(h);
}

bool AudioPlayer::wavReadHeaderFromStream() {
  if (!stream || !stream->client) { lastErr = "wav_stream_null"; return false; }
  uint8_t h[44];
  if (!readBytesStream(*stream->client, h, 44)) { lastErr = "wav_header_read_fail"; return false; }
  return wavParseHeader(h);
}

bool AudioPlayer::fillWav() {
  if (!wavOk) { lastErr = "wav_not_ready"; stop(); return false; }
  if (wavDataRemaining == 0) { inputEnded = true; return false; }

  const int chunkFrames = 256;
  uint8_t raw[chunkFrames * 4];

  int bytesPerFrame = wavChannels * 2;
  uint32_t wantBytes = (uint32_t)chunkFrames * (uint32_t)bytesPerFrame;
  if (wantBytes > wavDataRemaining) wantBytes = wavDataRemaining;

  size_t got = 0;
  if (file) {
    got = file.read(raw, wantBytes);
  } else if (stream && stream->client) {
    WiFiClient& s = *stream->client;
    int av = s.available();
    if (av <= 0) {
      if (!s.connected()) { inputEnded = true; return false; }
      delay(1);
      return true;
    }
    got = (size_t)s.read(raw, min((int)wantBytes, av));
  }

  if (got == 0) {
    if (stream && stream->client && !stream->client->connected()) inputEnded = true;
    return false;
  }

  wavDataRemaining -= (uint32_t)got;

  int frames = (int)(got / bytesPerFrame);
  for (int i = 0; i < frames; i++) {
    int16_t l = (int16_t)(raw[i * bytesPerFrame + 0] | (raw[i * bytesPerFrame + 1] << 8));
    int16_t s = l;
    if (wavChannels == 2) {
      int16_t r = (int16_t)(raw[i * bytesPerFrame + 2] | (raw[i * bytesPerFrame + 3] << 8));
      s = (int16_t)(((int32_t)l + (int32_t)r) / 2);
    }
    if (!rbPush(s)) break;
  }
  return true;
}

bool AudioPlayer::fillMp3() {
  // Stub decoder
  if (mp3InFilled < 1024) {
    int r = 0;
    if (file) {
      r = file.read(mp3In + mp3InFilled, sizeof(mp3In) - mp3InFilled);
    } else if (stream && stream->client) {
      WiFiClient& s = *stream->client;
      int av = s.available();
      if (av > 0) r = s.read(mp3In + mp3InFilled, min((int)sizeof(mp3In) - mp3InFilled, av));
      else {
        if (!s.connected()) inputEnded = true;
        delay(1);
        return true;
      }
    }
    if (r > 0) mp3InFilled += r;
    else inputEnded = true;
  }

  if (mp3InFilled == 0) { inputEnded = true; return false; }

  mp3dec_frame_info_t info {};
  info.frame_bytes = 0;
  info.channels = 0;
  info.hz = 0;
  info.layer = 0;
  info.bitrate_kbps = 0;

  if (inputEnded) return false;
  delay(1);
  return true;
}

bool playAlarmAudioWithFallback(const AlarmConfig& a) {
  lastAudioError = "";

  auto tryLocal = [&](const String& p) -> bool {
    if (p.length() == 0) return false;
    bool ok = audio.playLocal(p, a.volume);
    if (!ok) lastAudioError = audio.lastError();
    return ok;
  };
  auto tryUrl = [&](const String& u) -> bool {
    if (u.length() == 0) return false;
    bool ok = audio.playUrl(u, a.volume);
    if (!ok) lastAudioError = audio.lastError();
    return ok;
  };

  bool ok = false;
  if (a.audio_type == AUDIO_URL) {
    ok = tryUrl(String(a.url));
    if (!ok) {
      if (strlen(a.fallback_local_path) > 0) ok = tryLocal(String(a.fallback_local_path));
      if (!ok && LittleFS.exists("/audio/default.wav")) ok = tryLocal("/audio/default.wav");
    }
  } else {
    ok = tryLocal(String(a.local_path));
    if (!ok) {
      if (strlen(a.fallback_local_path) > 0) ok = tryLocal(String(a.fallback_local_path));
      if (!ok && LittleFS.exists("/audio/default.wav")) ok = tryLocal("/audio/default.wav");
    }
  }
  return ok;
}
