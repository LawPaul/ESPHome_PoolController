// Host unit tests for iso8601.h -- see that header for why it exists.
//   c++ -std=c++17 -Wall -Wextra tests/test_iso8601.cpp -o /tmp/t_iso8601 && /tmp/t_iso8601

#include "../components/pool_control/iso8601.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::days_from_civil;
using esphome::pool_control::iso8601_to_epoch2020;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

// tol defaults to 0.5 s. Around a 2020 epoch (~2.1e8) a float's ulp is 16 s, so
// comparisons against an independently-computed absolute timestamp must allow
// one ulp -- see the EPOCH CHOICE note in iso8601.h.
static void check_eq(float got, float want, const char *what, float tol = 0.5f) {
  if (std::isnan(want) ? !std::isnan(got) : std::fabs(got - want) > tol) {
    std::printf("FAIL: %s (got %.1f, want %.1f)\n", what, got, want);
    failures++;
  }
}

int main() {
  // --- days_from_civil anchors ---
  check(days_from_civil(1970, 1, 1) == 0, "1970-01-01 is day 0");
  check(days_from_civil(2020, 1, 1) == 18262, "2020-01-01 is day 18262");
  check(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) == 2,
        "2000 is a leap year (divisible by 400)");
  check(days_from_civil(1900, 3, 1) - days_from_civil(1900, 2, 28) == 1,
        "1900 is NOT a leap year (divisible by 100, not 400)");

  // --- The epoch itself ---
  check_eq(iso8601_to_epoch2020("2020-01-01T00:00:00Z"), 0.0f, "epoch is zero");
  check_eq(iso8601_to_epoch2020("2020-01-01T00:01:00Z"), 60.0f, "one minute in");
  check_eq(iso8601_to_epoch2020("2020-01-02T00:00:00Z"), 86400.0f, "one day in");

  // --- The exact string that exposed the bug (13:06:58Z = 08:06:58 CDT) ---
  const float real = iso8601_to_epoch2020("2026-07-26T13:06:58Z");
  // Accumulate the expectation in double and round ONCE. Doing this chain in
  // float instead drifts ~2 ulps (each partial sum re-rounds), which is a
  // property of the test arithmetic, not of the parser.
  const double expect =
      static_cast<double>(days_from_civil(2026, 7, 26) - days_from_civil(2020, 1, 1)) * 86400.0 +
      13 * 3600 + 6 * 60 + 58;
  check_eq(real, static_cast<float>(expect), "real captured last_updated parses", 16.0f);
  check(real > 0.0f, "real timestamp is positive");

  // --- Format tolerance ---
  check_eq(iso8601_to_epoch2020("2026-07-26T13:06:58"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "bare time is treated as UTC");
  check_eq(iso8601_to_epoch2020("2026-07-26T13:06:58.123456Z"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "fractional seconds ignored");
  check_eq(iso8601_to_epoch2020("2026-07-26 13:06:58Z"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "space separator accepted");
  check_eq(iso8601_to_epoch2020("2026-07-26t13:06:58z"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "lowercase t/z accepted");

  // --- Zone offsets ---
  check_eq(iso8601_to_epoch2020("2026-07-26T08:06:58-05:00"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "-05:00 (CDT) normalises to UTC");
  check_eq(iso8601_to_epoch2020("2026-07-26T08:06:58-0500"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "colonless offset accepted");
  check_eq(iso8601_to_epoch2020("2026-07-26T14:06:58+01:00"),
           iso8601_to_epoch2020("2026-07-26T13:06:58Z"), "+01:00 normalises to UTC");

  // --- Rejection: must be NaN, never a plausible-looking wrong number ---
  check_eq(iso8601_to_epoch2020(nullptr), NAN, "null rejected");
  check_eq(iso8601_to_epoch2020(""), NAN, "empty rejected");
  check_eq(iso8601_to_epoch2020("unknown"), NAN, "'unknown' rejected");
  check_eq(iso8601_to_epoch2020("unavailable"), NAN, "'unavailable' rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26"), NAN, "date only rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26T13:06"), NAN, "missing seconds rejected");
  check_eq(iso8601_to_epoch2020("2026-13-26T13:06:58Z"), NAN, "month 13 rejected");
  check_eq(iso8601_to_epoch2020("2026-07-32T13:06:58Z"), NAN, "day 32 rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26T24:06:58Z"), NAN, "hour 24 rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26T13:60:58Z"), NAN, "minute 60 rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26T13:06:58Zjunk"), NAN, "trailing junk rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26X13:06:58Z"), NAN, "bad separator rejected");
  check_eq(iso8601_to_epoch2020("20xx-07-26T13:06:58Z"), NAN, "non-digit year rejected");
  check_eq(iso8601_to_epoch2020("2026-07-26T13:06:58."), NAN, "empty fraction rejected");

  // --- Dedupe semantics: the same string must compare EXACTLY equal, and
  //     two different readings must not collide. ---
  check(iso8601_to_epoch2020("2026-07-26T13:06:58Z") ==
            iso8601_to_epoch2020("2026-07-26T13:06:58Z"),
        "identical strings are bitwise-equal (dedupe works)");
  check(iso8601_to_epoch2020("2026-07-26T13:06:58Z") !=
            iso8601_to_epoch2020("2026-07-27T13:06:58Z"),
        "a day-later reading is distinguishable");
  check(iso8601_to_epoch2020("2026-07-26T13:06:58Z") !=
            iso8601_to_epoch2020("2026-07-26T14:06:58Z"),
        "an hour-later reading is distinguishable");

  // A fresh reading must never equal the -1 'never applied' sentinel.
  check(iso8601_to_epoch2020("2026-07-26T13:06:58Z") != -1.0f,
        "parsed ts never collides with the never-applied sentinel");

  if (failures == 0) std::printf("test_iso8601: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
