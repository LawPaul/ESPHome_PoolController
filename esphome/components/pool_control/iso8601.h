#pragma once
// Pure ISO-8601 -> epoch parsing. Framework-free so it can be unit tested on the
// host (see esphome/tests/). No ESPHome headers here on purpose.
//
// WHY THIS EXISTS
// The chlorine auto-tune dedupes readings by Home Assistant's `last_updated`,
// which is the only thing that distinguishes "you re-logged the same FC value"
// from "nothing happened". But `last_updated` is an ISO-8601 STRING
// ("2026-07-26T13:06:58Z"), and ESPHome's `sensor:` / `platform: homeassistant`
// can only import numerics -- it logged
//     'sensor.backyard_fc': Can't convert '2026-07-26T13:06:58Z' to number!
// and published NaN. on_new_reading() then bailed at its first guard
// (SkipNoTimestamp) for EVERY reading, so the integral trim never moved off its
// 50% cold-start seed. The import has to come in as a text_sensor and be parsed
// here instead.
//
// EPOCH CHOICE
// Returns seconds since 2020-01-01T00:00:00Z, not since 1970. The value is
// carried through ESPHome/host code as a float, and a 1970 epoch (~1.8e9) only
// has ~128 s of resolution in a 24-bit mantissa; a 2020 epoch (~2.1e8) gets that
// to ~16 s. In practice dedupe compares two floats parsed from the *same*
// string, so it is exact regardless -- the epoch shift just keeps the number
// honest if it is ever displayed or differenced.

#include <cmath>

namespace esphome {
namespace pool_control {

// Seconds between 1970-01-01 and 2020-01-01, both UTC.
static constexpr double EPOCH_2020 = 1577836800.0;

// Days since 1970-01-01 from a civil (proleptic Gregorian) date.
// Howard Hinnant's days_from_civil; pure integer math, valid for any year.
inline int days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;                                   // [0, 399]
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

// Read exactly n digits starting at s[i], advancing i. Returns false on any
// non-digit (so a malformed field is rejected rather than silently truncated).
inline bool read_uint(const char *s, int &i, int n, int &out) {
  int v = 0;
  for (int k = 0; k < n; k++) {
    const char c = s[i + k];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  i += n;
  out = v;
  return true;
}

// Parse "YYYY-MM-DDTHH:MM:SS" with optional fractional seconds and an optional
// zone suffix ('Z', '+HH:MM' or '-HH:MM'; a bare end is treated as UTC, which is
// what Home Assistant always sends). Returns seconds since 2020-01-01Z, or NaN
// if the string is null/short/malformed or the fields are out of range.
//
// Deliberately strict: a partial parse would produce a plausible-looking
// timestamp that silently corrupts dedupe, which is exactly the failure mode
// this file exists to eliminate.
inline float iso8601_to_epoch2020(const char *s) {
  const float bad = NAN;
  if (s == nullptr) return bad;

  // Shortest legal form is 19 chars: YYYY-MM-DDTHH:MM:SS
  int len = 0;
  while (s[len] != '\0' && len < 64) len++;
  if (len < 19) return bad;

  int i = 0, year = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
  if (!read_uint(s, i, 4, year)) return bad;
  if (s[i++] != '-') return bad;
  if (!read_uint(s, i, 2, mon)) return bad;
  if (s[i++] != '-') return bad;
  if (!read_uint(s, i, 2, day)) return bad;
  if (s[i] != 'T' && s[i] != 't' && s[i] != ' ') return bad;
  i++;
  if (!read_uint(s, i, 2, hh)) return bad;
  if (s[i++] != ':') return bad;
  if (!read_uint(s, i, 2, mm)) return bad;
  if (s[i++] != ':') return bad;
  if (!read_uint(s, i, 2, ss)) return bad;

  if (mon < 1 || mon > 12) return bad;
  if (day < 1 || day > 31) return bad;
  if (hh > 23 || mm > 59 || ss > 60) return bad;  // 60 tolerates a leap second

  // Optional fractional seconds -- accepted and discarded (1 s resolution is
  // far finer than the reading cadence).
  if (s[i] == '.' || s[i] == ',') {
    i++;
    if (s[i] < '0' || s[i] > '9') return bad;  // '.' with no digits is malformed
    while (s[i] >= '0' && s[i] <= '9') i++;
  }

  // Optional zone.
  int offset_s = 0;
  if (s[i] == 'Z' || s[i] == 'z') {
    i++;
  } else if (s[i] == '+' || s[i] == '-') {
    const int sign = (s[i] == '-') ? -1 : 1;
    i++;
    int oh = 0, om = 0;
    if (!read_uint(s, i, 2, oh)) return bad;
    if (s[i] == ':') i++;  // "+HH:MM" and "+HHMM" are both legal
    if (!read_uint(s, i, 2, om)) return bad;
    if (oh > 23 || om > 59) return bad;
    offset_s = sign * (oh * 3600 + om * 60);
  }
  if (s[i] != '\0') return bad;  // trailing junk

  const double days = days_from_civil(year, mon, day);
  const double epoch1970 = days * 86400.0 + hh * 3600.0 + mm * 60.0 + ss;
  return static_cast<float>(epoch1970 - offset_s - EPOCH_2020);
}

}  // namespace pool_control
}  // namespace esphome
