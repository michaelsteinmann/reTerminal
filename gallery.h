#pragma once
// Wake-cause handling and empty-battery protection for swissmicros_gallery.yaml.
//
// The image index lives in NVS (globals with restore_value) so it survives a
// flat battery. The time of the next daily change lives in RTC memory, which
// survives deep sleep together with the system clock (both reset on power loss).

#include <cmath>
#include <ctime>
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esphome/core/preferences.h"

static constexpr int64_t GALLERY_DAY_S = 24 * 60 * 60;
static constexpr int GALLERY_KEY_PREV = 5;  // left white button
static constexpr int GALLERY_KEY_NEXT = 4;  // right white button
static constexpr int GALLERY_KEY_SETUP = 3;  // green button

RTC_DATA_ATTR static int64_t gallery_next_change_s = 0;

// Returns how many images to step (-1, 0, +1, or more after missed days).
inline int gallery_step_for_wakeup() {
  const int64_t now = time(nullptr);
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
      if (pins & (1ULL << GALLERY_KEY_PREV))
        return -1;
      if (pins & (1ULL << GALLERY_KEY_NEXT))
        return 1;
      return 0;
    }
    case ESP_SLEEP_WAKEUP_TIMER: {
      int steps = 0;
      while (gallery_next_change_s <= now + 60) {  // tolerate an early timer
        gallery_next_change_s += GALLERY_DAY_S;
        steps++;
      }
      return steps;
    }
    default:
      // Power-on, reset or flashing: keep the stored image, restart the day.
      gallery_next_change_s = now + GALLERY_DAY_S;
      return 0;
  }
}

// LiPo voltage (under light load) to state of charge.
inline int gallery_battery_percent(float v) {
  static const float table[][2] = {{4.15f, 100}, {3.96f, 90}, {3.91f, 80}, {3.85f, 70}, {3.80f, 60}, {3.75f, 50},
                                   {3.68f, 40},  {3.58f, 30}, {3.49f, 20}, {3.41f, 10}, {3.30f, 5},  {3.27f, 0}};
  if (v >= table[0][0])
    return 100;
  for (size_t i = 1; i < sizeof(table) / sizeof(table[0]); i++) {
    if (v >= table[i][0]) {
      const float t = (v - table[i][0]) / (table[i - 1][0] - table[i][0]);
      return int(table[i][1] + t * (table[i - 1][1] - table[i][1]) + 0.5f);
    }
  }
  return 0;
}

inline bool gallery_setup_requested() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
         (esp_sleep_get_ext1_wakeup_status() & (1ULL << GALLERY_KEY_SETUP));
}

// Power-on, reset or flashing (as opposed to waking from deep sleep).
inline bool gallery_power_on() { return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED; }

inline uint32_t gallery_ms_until_next_change() {
  const int64_t left = gallery_next_change_s - time(nullptr);
  return left > 0 ? uint32_t(left) * 1000u : 1000u;
}

// ---- Empty-battery protection ---------------------------------------------
// A panel refresh draws the most current; on an empty battery the voltage
// collapses and the chip resets mid-refresh, leaving a half-drawn image. The
// battery voltage is written to flash before every refresh and cleared after
// it; finding it still set at boot means the last refresh crashed at that
// voltage. Until the battery is charged above it again, no refresh is started.

static constexpr float GALLERY_RECHARGED_MARGIN_V = 0.15f;  // above the battery's rebound after the load is gone

struct GalleryBatteryLog {
  float refresh_v;  // voltage when the running refresh started; 0 = no refresh running
  float crash_v;    // voltage of the last refresh that crashed; 0 = none
};

inline esphome::ESPPreferenceObject &gallery_battery_pref() {
  static esphome::ESPPreferenceObject pref =
      esphome::global_preferences->make_preference<GalleryBatteryLog>(0x6A11E7B1u, true);
  return pref;
}

inline GalleryBatteryLog &gallery_battery_log() {
  static GalleryBatteryLog log{};
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    if (!gallery_battery_pref().load(&log))
      log = {};
  }
  return log;
}

inline void gallery_battery_save() {
  gallery_battery_pref().save(&gallery_battery_log());
  esphome::global_preferences->sync();
}

// At boot: a refresh still marked as running means it died. Only count resets
// that a dying battery causes, and only on battery voltage (on USB it is
// above 4 V, e.g. when flashing interrupts a refresh).
inline void gallery_battery_check_crash() {
  auto &log = gallery_battery_log();
  if (log.refresh_v <= 0)
    return;
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool crash_reset = reason == ESP_RST_BROWNOUT || reason == ESP_RST_POWERON || reason == ESP_RST_PANIC ||
                           reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
  if (crash_reset && log.refresh_v < 3.9f) {
    log.crash_v = log.refresh_v;
    ESP_LOGW("gallery", "previous refresh crashed at %.2f V (reset reason %d)", log.crash_v, (int) reason);
  }
  log.refresh_v = 0;
  gallery_battery_save();
}

// Whether the battery can take a refresh: not below min_percent, and charged
// clearly above the voltage of the last crashed refresh.
inline bool gallery_battery_can_refresh(float v, int min_percent) {
  if (std::isnan(v))
    return true;
  if (gallery_battery_percent(v) < min_percent)
    return false;
  const float crash_v = gallery_battery_log().crash_v;
  return crash_v <= 0 || v >= crash_v + GALLERY_RECHARGED_MARGIN_V;
}

inline float gallery_battery_crash_v() { return gallery_battery_log().crash_v; }

inline void gallery_refresh_begin(float v) {
  gallery_battery_log().refresh_v = std::isnan(v) ? 0.01f : v;
  gallery_battery_save();
}

// After a completed refresh. clear_crash: the crash banner was on that image.
inline void gallery_refresh_end(bool clear_crash) {
  auto &log = gallery_battery_log();
  log.refresh_v = 0;
  if (clear_crash)
    log.crash_v = 0;
  gallery_battery_save();
}
