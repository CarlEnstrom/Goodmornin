#pragma once
#include <Arduino.h>

static const int MAX_ALARMS = 10;

enum AudioType : uint8_t { AUDIO_LOCAL = 0, AUDIO_URL = 1 };

struct AlarmConfig {
  uint32_t version;
  uint32_t id;
  bool enabled;

  char label[32];

  uint8_t hour;
  uint8_t minute;

  uint8_t days_mask;      // bit0=Mon..bit6=Sun
  char once_date[11];     // YYYY-MM-DD or ""

  int16_t snooze_minutes;

  int8_t gpio_pin;
  uint16_t long_press_ms; // 0 = global default

  char inbound_token[48];

  char on_set_url[160];
  char on_fire_url[160];
  char on_snooze_url[160];
  char on_dismiss_url[160];

  AudioType audio_type;
  char local_path[96];
  char url[240];
  char fallback_local_path[96];

  uint8_t volume; // 0-100

  uint32_t last_fired_unix;

  uint8_t reserved[24];
};

struct AlarmRuntime {
  time_t next_fire_unix = 0;
  bool ringing = false;
  bool snoozed = false;
  time_t snooze_until = 0;
  time_t current_fire_unix = 0;
};

