#pragma once
// Weather mode for swissmicros_gallery.yaml: a few days of forecast for two
// places from Open-Meteo (MeteoSwiss models for Switzerland, no API key).
//
// The last forecast lives in RTC memory, so it survives deep sleep and is
// shown again (marked as old) when a later fetch fails. It is lost on power loss.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include "esp_attr.h"
#include "esphome/components/json/json_util.h"

static constexpr int WEATHER_PLACES = 2;
static constexpr int WEATHER_DAYS = 5;
static constexpr int WEATHER_REFRESH_HOUR = 5;      // local time of the daily fetch
static constexpr int64_t WEATHER_RETRY_S = 3 * 60 * 60;  // after a failed fetch

// Places in display order, top to bottom. Must match the URL below.
static const char *const WEATHER_NAMES[WEATHER_PLACES] = {"Sagogn", "Stäfa"};
static const char *const WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=46.792,47.241&longitude=9.240,8.725"
    "&current=temperature_2m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max"
    "&timezone=Europe%2FZurich&forecast_days=5";

struct WeatherDay {
  int8_t weekday;  // 0 = Sunday
  int8_t day, month;
  int16_t code;    // WMO weather code
  float t_max, t_min, precip_mm;
  int8_t precip_prob;  // %, -1 = unknown
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

inline uint32_t weather_ms_until_next_fetch() {
  const int64_t left = weather_next_fetch_s - time(nullptr);
  return left > 0 ? uint32_t(left) * 1000u : 1000u;
}

// Material Design Icons glyph for a WMO weather code (UTF-8).
inline const char *weather_icon(int code) {
  switch (code) {
    case 0: case 1: return "\U000F0599";            // sunny
    case 2: return "\U000F0595";                    // partly cloudy
    case 3: return "\U000F0590";                    // cloudy
    case 45: case 48: return "\U000F0591";          // fog
    case 51: case 53: case 55: case 56: case 57:
    case 61: case 63: case 80: case 81: return "\U000F0597";  // rain
    case 65: case 82: return "\U000F0596";          // heavy rain
    case 66: case 67: return "\U000F067F";          // freezing rain
    case 71: case 73: case 77: case 85: return "\U000F0598";  // snow
    case 75: case 86: return "\U000F0F36";          // heavy snow
    case 95: case 96: case 99: return "\U000F067E"; // thunderstorm
    default: return "\U000F0590";                  // unknown: cloudy
  }
}

// Icon colour: 0 black, 1 yellow (sun), 2 blue (rain, snow), 3 red (thunder).
inline int weather_icon_color(int code) {
  if (code <= 1)
    return 1;
  if (code >= 95)
    return 3;
  if (code >= 51)
    return 2;
  return 0;
}
