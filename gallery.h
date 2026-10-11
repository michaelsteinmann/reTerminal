#pragma once
// Wake-cause handling for swissmicros_gallery.yaml.
//
// The image index lives in NVS (globals with restore_value) so it survives a
// flat battery. The time of the next daily change lives in RTC memory, which
// survives deep sleep together with the system clock (both reset on power loss).

#include <ctime>
#include "esp_sleep.h"
#include "esp_attr.h"

static constexpr int64_t GALLERY_DAY_S = 24 * 60 * 60;
static constexpr int GALLERY_KEY_PREV = 5;  // left white button
static constexpr int GALLERY_KEY_NEXT = 4;  // right white button

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

inline uint32_t gallery_ms_until_next_change() {
  const int64_t left = gallery_next_change_s - time(nullptr);
  return left > 0 ? uint32_t(left) * 1000u : 1000u;
}
