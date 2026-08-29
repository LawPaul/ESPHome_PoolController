#pragma once

#include <cmath>

namespace esphome {
namespace pool_control {

// Pure salt-cell runtime accounting. Framework-free so the output-weighting math
// is host-tested; the YAML interval only reads the live output %, calls the
// step, and accumulates the deltas into restored globals.

struct CellWearStep {
  float gen_delta_h{0};    // wall-clock generating hours accrued this window
  float equiv_delta_h{0};  // output-weighted (equivalent full-output) hours
};

// Accrue runtime for one dt window while the cell is generating. `pct` is the
// cell's output percentage (0..100). equiv is weighted by output so it reflects
// how HARD the cell ran, not merely how long. Returns zero deltas for an idle
// cell (pct <= 0 or NaN) or a non-positive window.
inline CellWearStep cell_wear_step(float pct, float dt_seconds) {
  CellWearStep s;
  if (std::isnan(pct) || pct <= 0.0f || dt_seconds <= 0.0f)
    return s;
  float dt_h = dt_seconds / 3600.0f;
  s.gen_delta_h = dt_h;
  s.equiv_delta_h = dt_h * (pct / 100.0f);
  return s;
}

}  // namespace pool_control
}  // namespace esphome
