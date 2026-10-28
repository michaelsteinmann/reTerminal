#pragma once
// Weather mode for swissmicros_gallery.yaml: a few days of forecast for two
// places from Open-Meteo (MeteoSwiss models for Switzerland, no API key).
//
// The last forecast lives in RTC memory, so it survives deep sleep and is
// shown again (marked as old) when a later fetch fails. It is lost on power loss.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include "esp_attr.h"
#include "esphome/components/display/display.h"
#include "esphome/components/json/json_util.h"

static constexpr int WEATHER_PLACES = 2;
static constexpr int WEATHER_DAYS = 10;      // fetched; beyond that forecasts are mostly guesswork
static constexpr int WEATHER_PAGE_DAYS = 5;  // shown at once; the white buttons turn pages
static constexpr int WEATHER_PAGES = WEATHER_DAYS / WEATHER_PAGE_DAYS;
static constexpr int WEATHER_REFRESH_HOUR = 5;      // local time of the daily fetch
static constexpr int64_t WEATHER_RETRY_S = 3 * 60 * 60;  // after a failed fetch

// Places in display order, top to bottom. Must match the URL below.
static const char *const WEATHER_NAMES[WEATHER_PLACES] = {"Sagogn", "Stäfa"};
static constexpr int WEATHER_AWAY = 0, WEATHER_HOME = 1;  // the device stays in Stäfa
static const char *const WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=46.792,47.241&longitude=9.240,8.725"
    "&current=temperature_2m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max,"
    "sunshine_duration,daylight_duration"
    "&timezone=Europe%2FZurich&forecast_days=10";

struct WeatherDay {
  int8_t weekday;  // 0 = Sunday
  int8_t day, month;
  int16_t code;    // WMO weather code
  float t_max, t_min, precip_mm;
  int8_t precip_prob;  // %, -1 = unknown
  int8_t sun;          // sunshine in % of daylight, -1 = unknown
  float sun_h;         // hours of sunshine
};

struct WeatherPlace {
  float t_now;
  WeatherDay days[WEATHER_DAYS];
};

struct WeatherData {
  bool valid;
  bool stale;           // the last fetch failed; showing older data
  char updated[6];      // local "HH:MM" of the forecast
  WeatherPlace places[WEATHER_PLACES];
};

RTC_DATA_ATTR static WeatherData weather_data = {};
RTC_DATA_ATTR static int64_t weather_next_fetch_s = 0;
RTC_DATA_ATTR static int weather_page = 0;

// Day of week for a Gregorian date, 0 = Sunday (Sakamoto).
inline int weather_weekday(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3)
    y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Whole degrees; "+ 0.0f" turns -0 into 0 so it doesn't print as "-0".
inline float weather_round(float t) { return std::round(t) + 0.0f; }

// Parses the Open-Meteo response (an array, one entry per place) and schedules
// the next fetch for WEATHER_REFRESH_HOUR local time. Returns false on bad data.
inline bool weather_parse(const std::string &body) {
  JsonDocument doc = esphome::json::parse_json(body);
  JsonArray places = doc.as<JsonArray>();
  if (places.size() != WEATHER_PLACES) {
    ESP_LOGW("weather", "unexpected response (%u bytes): %.120s", (unsigned) body.size(), body.c_str());
    return false;
  }
  WeatherData data = {};
  int hour = -1, minute = 0;
  for (int p = 0; p < WEATHER_PLACES; p++) {
    JsonObject place = places[p];
    JsonObject daily = place["daily"];
    if (daily["time"].as<JsonArray>().size() < WEATHER_DAYS)
      return false;
    WeatherPlace &out = data.places[p];
    out.t_now = weather_round(place["current"]["temperature_2m"] | NAN);
    // "2026-10-28T06:15", local time
    if (p == 0 && sscanf(place["current"]["time"] | "", "%*d-%*d-%*dT%d:%d", &hour, &minute) != 2)
      hour = -1;
    for (int i = 0; i < WEATHER_DAYS; i++) {
      WeatherDay &d = out.days[i];
      int y = 0, m = 1, dd = 1;
      sscanf(daily["time"][i] | "", "%d-%d-%d", &y, &m, &dd);
      d.weekday = weather_weekday(y, m, dd);
      d.day = dd;
      d.month = m;
      d.code = daily["weather_code"][i] | -1;
      d.t_max = weather_round(daily["temperature_2m_max"][i] | NAN);
      d.t_min = weather_round(daily["temperature_2m_min"][i] | NAN);
      d.precip_mm = daily["precipitation_sum"][i] | NAN;
      d.precip_prob = daily["precipitation_probability_max"][i] | -1;
      const float sun_s = daily["sunshine_duration"][i] | NAN, day_s = daily["daylight_duration"][i] | NAN;
      d.sun_h = std::isnan(sun_s) ? 0 : sun_s / 3600;
      d.sun = day_s > 0 && !std::isnan(sun_s) ? int8_t(std::lround(std::fmin(100.0f, std::fmax(0.0f, 100 * sun_s / day_s)))) : -1;
    }
  }
  if (hour < 0)
    return false;
  snprintf(data.updated, sizeof(data.updated), "%02d:%02d", hour, minute);
  data.valid = true;
  weather_data = data;

  // The device clock isn't set, but the forecast says what time it is locally.
  int64_t until = int64_t(WEATHER_REFRESH_HOUR * 60 - (hour * 60 + minute)) * 60;
  if (until <= 30 * 60)  // already past (or nearly at) today's refresh: tomorrow's
    until += 24 * 60 * 60;
  weather_next_fetch_s = time(nullptr) + until;
  ESP_LOGI("weather", "forecast at %s, next fetch in %d min", data.updated, int(until / 60));
  return true;
}

inline void weather_fetch_failed() {
  weather_data.stale = true;
  weather_next_fetch_s = time(nullptr) + WEATHER_RETRY_S;
}

// ---- Trip tip ----------------------------------------------------------------
// Suggest going to Sagogn when it is much nicer there than at home for at least
// WEATHER_TIP_DAYS days in a row: often in autumn and winter, when Stäfa is under
// the fog or in cold air and Sagogn (930 m) is above it. A day counts when it is
// sunny and dry away and, compared with home, much sunnier, drier or warmer.
static constexpr int WEATHER_TIP_DAYS = 3;
static constexpr int WEATHER_TIP_SUN_LEAD = 30;  // percentage points more sunshine away
static constexpr int WEATHER_TIP_MIN_SUN = 50;   // % sunshine away, so it is actually nice
static constexpr float WEATHER_WET_MM = 1.0f;    // rain from this much on spoils a day
static constexpr float WEATHER_TIP_WARMER = 4.0f;  // degrees warmer away (cold air pooling at home)
static constexpr int WEATHER_TIP_SUN_SLACK = 10;   // ...while at most this many points less sunny

inline bool weather_much_nicer_away(int i) {
  const WeatherDay &away = weather_data.places[WEATHER_AWAY].days[i];
  const WeatherDay &home = weather_data.places[WEATHER_HOME].days[i];
  if (away.sun < WEATHER_TIP_MIN_SUN || !(away.precip_mm < WEATHER_WET_MM))
    return false;
  return away.sun >= home.sun + WEATHER_TIP_SUN_LEAD || home.precip_mm >= WEATHER_WET_MM ||
         (away.t_max >= home.t_max + WEATHER_TIP_WARMER && away.sun >= home.sun - WEATHER_TIP_SUN_SLACK);
}

// The first run of at least WEATHER_TIP_DAYS much nicer days away, at full length.
inline bool weather_tip(int &first, int &len) {
  if (!weather_data.valid)
    return false;
  for (int i = 0; i < WEATHER_DAYS;) {
    int n = 0;
    while (i + n < WEATHER_DAYS && weather_much_nicer_away(i + n))
      n++;
    if (n >= WEATHER_TIP_DAYS) {
      first = i;
      len = n;
      return true;
    }
    i += n + 1;
  }
  return false;
}

// Woken by a white button: only turn the page, the stored forecast is recent enough.
inline bool weather_page_wake() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
         (esp_sleep_get_ext1_wakeup_status() & ((1ULL << GALLERY_KEY_PREV) | (1ULL << GALLERY_KEY_NEXT)));
}

// Returns whether the page changed.
inline bool weather_turn_page(int steps) {
  const int page = std::min(std::max(weather_page + steps, 0), WEATHER_PAGES - 1);
  const bool changed = page != weather_page;
  weather_page = page;
  return changed;
}

inline uint32_t weather_ms_until_next_fetch() {
  const int64_t left = weather_next_fetch_s - time(nullptr);
  return left > 0 ? uint32_t(left) * 1000u : 1000u;
}

// ---- Icons, drawn by hand (yellow alone hardly shows on white) -------------
// The daily WMO code is the most severe weather of the whole day, so a foggy
// night makes a sunny day a "fog" day. The sun and clouds are therefore drawn
// from the day's sunshine; rain, snow and thunder come from the code.

enum WeatherKind { KIND_DRY, KIND_FOG, KIND_RAIN, KIND_POURING, KIND_SLEET, KIND_SNOW, KIND_HEAVY_SNOW, KIND_THUNDER };

inline WeatherKind weather_kind(int code) {
  switch (code) {
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 80: case 81: return KIND_RAIN;
    case 65: case 82: return KIND_POURING;
    case 66: case 67: return KIND_SLEET;
    case 71: case 73: case 77: case 85: return KIND_SNOW;
    case 75: case 86: return KIND_HEAVY_SNOW;
    case 95: case 96: case 99: return KIND_THUNDER;
    case 45: case 48: return KIND_FOG;
    default: return KIND_DRY;
  }
}

// 4 sunny, 3 mostly sunny, 2 partly sunny, 1 mostly cloudy, 0 cloudy.
inline int weather_sun_level(const WeatherDay &d) {
  if (d.sun < 0)  // unknown: go by the code
    return d.code >= 0 && d.code <= 1 ? 4 : d.code == 2 ? 2 : 0;
  return d.sun >= 80 ? 4 : d.sun >= 60 ? 3 : d.sun >= 40 ? 2 : d.sun >= 20 ? 1 : 0;
}

namespace weather_draw {
using esphome::Color;
using esphome::display::Display;
static const Color BLACK(0, 0, 0), WHITE(255, 255, 255), YELLOW(255, 255, 0), BLUE(0, 0, 255), RED(255, 0, 0);

// A line r pixels thick with round ends.
inline void thick_line(Display &it, int x1, int y1, int x2, int y2, int r, Color c) {
  const int n = std::max(std::max(std::abs(x2 - x1), std::abs(y2 - y1)), 1);
  for (int i = 0; i <= n; i++)
    it.filled_circle(x1 + int(std::lround(float(x2 - x1) * i / n)), y1 + int(std::lround(float(y2 - y1) * i / n)), r, c);
}

inline void sun(Display &it, int x, int y, int r) {
  it.filled_circle(x, y, r + 3, BLACK);
  it.filled_circle(x, y, r, YELLOW);
  for (int k = 0; k < 8; k++) {
    const float a = k * float(M_PI) / 4, ca = std::cos(a), sa = std::sin(a);
    thick_line(it, x + int(std::lround((r + 7) * ca)), y + int(std::lround((r + 7) * sa)),
               x + int(std::lround((r + 13) * ca)), y + int(std::lround((r + 13) * sa)), 2, BLACK);
  }
}

// White cloud with a black outline; k scales it (1 = about 60 x 40 pixels).
inline void cloud(Display &it, int x, int y, float k) {
  static const int parts[3][3] = {{-17, 3, 11}, {-1, -6, 15}, {15, 2, 12}};  // circles: dx, dy, r
  auto s = [k](int v) { return int(std::lround(v * k)); };
  for (int o = 3; o >= 0; o -= 3) {
    const Color c = o ? BLACK : WHITE;
    for (auto &p : parts)
      it.filled_circle(x + s(p[0]), y + s(p[1]), s(p[2]) + o, c);
    it.filled_rectangle(x + s(-17) - o, y + s(3) - o, s(32) + 2 * o + 1, s(11) + 2 * o + 1, c);  // flat bottom
  }
}

// One icon of about 64 x 64 pixels centred on (cx, cy).
inline void icon(Display &it, int cx, int cy, const WeatherDay &d) {
  const WeatherKind kind = weather_kind(d.code);
  const int level = weather_sun_level(d);
  if (kind == KIND_DRY || (kind == KIND_FOG && level > 0)) {
    switch (level) {
      case 4: sun(it, cx, cy, 12); break;
      case 3: sun(it, cx - 6, cy - 6, 11); cloud(it, cx + 14, cy + 15, 0.6f); break;
      case 2: sun(it, cx - 10, cy - 9, 10); cloud(it, cx + 6, cy + 10, 0.85f); break;
      case 1: sun(it, cx - 16, cy - 14, 7); cloud(it, cx + 4, cy + 6, 1.0f); break;
      default: cloud(it, cx + 12, cy - 8, 0.65f); cloud(it, cx - 4, cy + 6, 1.0f); break;
    }
    return;
  }
  if (kind == KIND_FOG) {
    static const int lines[3][2] = {{-24, 20}, {-18, 26}, {-24, 14}};
    cloud(it, cx, cy - 8, 0.8f);
    for (int i = 0; i < 3; i++)
      thick_line(it, cx + lines[i][0], cy + 12 + 7 * i, cx + lines[i][1], cy + 12 + 7 * i, 2, BLACK);
    return;
  }
  // Rain, snow, thunder: cloud on top, a sun peeking out when there is enough of it.
  if (level >= 3)
    sun(it, cx - 14, cy - 13, 8);
  else if (level == 2)
    sun(it, cx - 16, cy - 15, 5);
  cloud(it, cx + 4, cy - 8, 0.8f);
  const int base = cy + 10;
  if (kind == KIND_RAIN || kind == KIND_POURING) {
    static const int light[] = {-12, 0, 12}, heavy[] = {-18, -6, 6, 18};
    const int *xs = kind == KIND_RAIN ? light : heavy, n = kind == KIND_RAIN ? 3 : 4;
    for (int i = 0; i < n; i++) {
      const int y = base + 4 * (i % 2);
      thick_line(it, cx + xs[i] + 7, y, cx + xs[i] + 3, y + 10, 2, BLUE);
    }
  } else if (kind == KIND_THUNDER) {
    const int x = cx + 4, y = base - 2;
    it.filled_triangle(x + 4, y, x - 7, y + 12, x + 2, y + 12, RED);
    it.filled_triangle(x - 2, y + 10, x + 7, y + 10, x - 5, y + 24, RED);
  } else {  // snow; sleet has a raindrop between the flakes
    static const int light[] = {-12, 2, 16}, heavy[] = {-17, -4, 9, 22};
    const int *xs = kind == KIND_HEAVY_SNOW ? heavy : light, n = kind == KIND_HEAVY_SNOW ? 4 : 3;
    for (int i = 0; i < n; i++) {
      const int x = cx + xs[i], y = base + 7 + 4 * (i % 2);
      if (kind == KIND_SLEET && i % 2) {
        thick_line(it, x + 3, y - 5, x - 1, y + 5, 2, BLUE);
        continue;
      }
      for (int a = 0; a < 3; a++) {
        const float ang = a * float(M_PI) / 3 + float(M_PI) / 2;
        const int dx = int(std::lround(6 * std::cos(ang))), dy = int(std::lround(6 * std::sin(ang)));
        thick_line(it, x - dx, y - dy, x + dx, y + dy, 1, BLUE);
      }
    }
  }
}
}  // namespace weather_draw
