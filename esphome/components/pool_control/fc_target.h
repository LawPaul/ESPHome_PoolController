#pragma once

#include <cmath>
#include <limits>

namespace esphome {
namespace pool_control {

// Pure free-chlorine target from cyanuric acid, using the TFP FC/CYA ratio and
// clamped to a safety band. Framework-free so the chemistry bounds are
// host-tested; the YAML template sensor only reads CYA + ratio and publishes.
//
//   cya        - measured cyanuric acid (ppm); NaN => target unavailable
//   ratio      - FC/CYA ratio (typically ~0.075 for a SWG pool)
//   floor_ppm  - minimum safe FC target
//   ceil_ppm   - maximum FC target (avoid over-chlorination)
//
// Returns NaN when CYA is missing so the caller can hold/skip.
inline float fc_target_from_cya(float cya, float ratio, float floor_ppm,
                                float ceil_ppm) {
  if (std::isnan(cya))
    return std::numeric_limits<float>::quiet_NaN();
  float t = ratio * cya;
  if (t < floor_ppm)
    t = floor_ppm;
  if (t > ceil_ppm)
    t = ceil_ppm;
  return t;
}

}  // namespace pool_control
}  // namespace esphome
