#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "alarms.h"

extern String lastAudioError;

class AudioPlayer {
public:
  void begin(int pwmPin);
  void setSampleRate(int sr);
  bool isPlaying() const;
  String lastError() const;
  void stop();
  bool playLocal(const String& path, uint8_t vol);
  bool playUrl(const String& url, uint8_t vol);
  void loop();
private:
  class StreamHolder;
  void startTimer();
  void writeDutyMid();
  void onTick();
  void rbReset();
  bool rbPush(int16_t s);
  bool rbPop(int16_t& s);
  bool readBytes(File& f, uint8_t* buf, size_t n);
  bool readBytesStream(WiFiClient& s, uint8_t* buf, size_t n, uint32_t timeoutMs = 3000);
  bool wavParseHeader(uint8_t header[44]);
  bool wavReadHeaderFromFile();
  bool wavReadHeaderFromStream();
  bool fillWav();
  bool fillMp3();
  static void timerThunk(void* arg);
  static uint32_t readLE32(const uint8_t* b);
  static uint16_t readLE16(const uint8_t* b);

  int audioPin = 5;
  int sampleRate = 16000;
  uint8_t volume = 80;

  volatile bool playing = false;
  String lastErr;
  String sourceKind;

  File file;
  StreamHolder* stream = nullptr;

  bool wavOk = false;
  uint16_t wavChannels = 1;
  uint32_t wavSampleRate = 16000;
  uint16_t wavBits = 16;
  uint32_t wavDataRemaining = 0;

  struct mp3dec_t { int dummy; } mp3dec;
  struct mp3dec_frame_info_t {
    int frame_bytes;
    int channels;
    int hz;
    int layer;
    int bitrate_kbps;
  };
  uint8_t mp3In[2048];
  int mp3InFilled = 0;
  bool inputEnded = false;

  static const int RB_CAP = 8192;
  volatile int rbHead = 0, rbTail = 0, rbCount = 0;
  int16_t rb[RB_CAP];

  esp_timer_handle_t timer = nullptr;
};

extern AudioPlayer audio;
bool playAlarmAudioWithFallback(const AlarmConfig& a);

