#pragma once

#include <cmath>

namespace esphome {
namespace pool_control {

// Pure mass-balance chlorine demand estimator. Framework-free so the window
// arithmetic is host-tested; the YAML only feeds it a reading and reads back
// the estimate.
//
// WHY THIS EXISTS
// Cell output % sets a RATE (ppm/day), not a level, so there is no output that
// "gives" a target FC -- output only sets a slope. The physically meaningful
// question is what production matches consumption, and over any window that is
// just bookkeeping:
//
//     demand (ppm/day) = (ppm produced - ppm gained) / days
//
// Production is taken from the cell's OUTPUT-WEIGHTED generating hours
// (cell_wear.h), so it is MEASURED rather than inferred from the commanded
// setpoint: pump downtime, comms loss, cold-water cutout and boost cycles are
// all already folded into that counter. An error integral on FC cannot make
// those distinctions -- it only ever sees "FC is low" and cannot tell a rise in
// demand from an hour the cell never ran.
//
// WHY A WINDOW, NOT A READING
// Only the two ENDPOINT FC readings carry test error, so accuracy improves with
// window length: +-0.5 ppm per FAS-DPD drop is +-0.7 ppm/day across one day but
// +-0.1 ppm/day across seven. Reacting to a single reading eats the full test
// error every time, forever.

struct DemandResult {
  enum class Status {
    Invalid,       // missing input, or the reading is a duplicate / out of order
    Anchored,      // first reading: window opened, nothing to estimate yet
    Accumulating,  // window still shorter than min_window_days
    Restarted,     // anchor too old, or the hour counter went backwards
    Updated,       // estimate advanced
  };
  Status status{Status::Invalid};
  float window_demand{NAN};  // this window's raw estimate, ppm/day
  float window_days{NAN};
};

class DemandObserver {
 public:
  // Demand moves on two timescales: season (months) and bathers/weather
  // (hours). Only the slow one is learnable, and the split is comfortable --
  // at these settings a seasonal ramp is tracked to within a few percent of
  // output while per-window test noise is cut to well under a percent.
  float alpha = 0.3f;             // EMA weight per accepted window
  float min_window_days = 3.0f;   // shorter windows are mostly test noise
  float max_window_days = 21.0f;  // an older anchor no longer describes the pool

  // `ts_days` epoch-days, `fc` ppm, `equiv_hours` the cell's LIFETIME
  // output-weighted generating hours, `ppm_per_equiv_h` from
  // ppm_per_equiv_hour() below.
  DemandResult on_reading(float ts_days, float fc, float equiv_hours,
                          float ppm_per_equiv_h) {
    DemandResult r;
    if (std::isnan(ts_days) || std::isnan(fc) || std::isnan(equiv_hours) ||
        !(ppm_per_equiv_h > 0.0f))
      return r;

    if (std::isnan(anchor_ts_)) {
      this->anchor_(ts_days, fc, equiv_hours);
      r.status = DemandResult::Status::Anchored;
      return r;
    }

    const float days = ts_days - anchor_ts_;
    if (days <= 0.0f) return r;  // re-logged or out-of-order reading

    // A lifetime counter that went DOWN means the persisted hours were lost
    // (reflash, failed restore), so this window's production is unknowable.
    if (days > max_window_days || equiv_hours < anchor_equiv_h_) {
      this->anchor_(ts_days, fc, equiv_hours);
      r.status = DemandResult::Status::Restarted;
      return r;
    }

    if (days < min_window_days) {
      r.status = DemandResult::Status::Accumulating;
      r.window_days = days;
      return r;
    }

    const float produced = ppm_per_equiv_h * (equiv_hours - anchor_equiv_h_);
    float d = (produced - (fc - anchor_fc_)) / days;
    if (d < 0.0f) d = 0.0f;  // a pool never creates chlorine; that is test error

    demand_ = std::isnan(demand_) ? d : demand_ + alpha * (d - demand_);
    this->anchor_(ts_days, fc, equiv_hours);

    r.status = DemandResult::Status::Updated;
    r.window_demand = d;
    r.window_days = days;
    return r;
  }

  float demand() const { return demand_; }
  bool has_anchor() const { return !std::isnan(anchor_ts_); }

  // Restore across reboot. A restored anchor whose equiv_hours no longer match
  // the live counter is caught by the backwards-counter check above.
  void restore(float demand, float anchor_ts, float anchor_fc, float anchor_equiv_h) {
    demand_ = demand;
    anchor_ts_ = anchor_ts;
    anchor_fc_ = anchor_fc;
    anchor_equiv_h_ = anchor_equiv_h;
  }
  float anchor_ts() const { return anchor_ts_; }
  float anchor_fc() const { return anchor_fc_; }
  float anchor_equiv_h() const { return anchor_equiv_h_; }

 private:
  void anchor_(float ts_days, float fc, float equiv_hours) {
    anchor_ts_ = ts_days;
    anchor_fc_ = fc;
    anchor_equiv_h_ = equiv_hours;
  }

  float demand_{NAN};
  float anchor_ts_{NAN};
  float anchor_fc_{NAN};
  float anchor_equiv_h_{NAN};
};

// Production constant: ppm added per equivalent full-output cell hour.
// 1 lb of chlorine in 10,000 gal is ~12 ppm (10,000 gal weighs ~83,450 lb).
inline float ppm_per_equiv_hour(float cell_lb_per_day, float pool_gallons) {
  if (!(pool_gallons > 0.0f) || !(cell_lb_per_day > 0.0f)) return NAN;
  return (cell_lb_per_day / 24.0f) * 12.0f * (10000.0f / pool_gallons);
}

// Output % whose production matches `demand`, given how many hours a day the
// cell actually gets to generate.
inline float output_for_demand(float demand, float ppm_per_equiv_h,
                               float generating_hours_per_day) {
  if (std::isnan(demand) || !(ppm_per_equiv_h > 0.0f) ||
      !(generating_hours_per_day > 0.0f))
    return NAN;
  float pct = 100.0f * demand / (ppm_per_equiv_h * generating_hours_per_day);
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

struct OutputCommand {
  float pct{NAN};
  float production_needed{NAN};  // ppm/day the cell has to make
};

// Cover demand, plus close whatever gap remains over `tau_days`.
//
// tau is the EXPECTED TEST INTERVAL, not a tuning gain. The commanded output
// stands until the next reading, so FC lands on target exactly when
// tau == the time until you next look; asking to close the gap faster than you
// will act overshoots by precisely that ratio.
//
// Unlike an error integral this is ABSOLUTE -- each reading recomputes the
// whole answer, so there is nothing to wind up and no need for a slew limit.
// It can move 20 points on one reading when the data warrants and 2 when it
// does not.
inline OutputCommand mass_balance_output(float demand, float fc, float target,
                                         float tau_days, float ppm_per_equiv_h,
                                         float generating_hours_per_day,
                                         float floor_pct, float ceil_pct) {
  OutputCommand c;
  if (std::isnan(demand) || std::isnan(fc) || std::isnan(target) ||
      !(tau_days > 0.0f) || !(ppm_per_equiv_h > 0.0f) ||
      !(generating_hours_per_day > 0.0f))
    return c;

  float need = demand + (target - fc) / tau_days;
  if (need < 0.0f) need = 0.0f;  // FC above target: stop, never un-chlorinate
  c.production_needed = need;

  float pct = 100.0f * need / (ppm_per_equiv_h * generating_hours_per_day);
  if (pct < floor_pct) pct = floor_pct;
  if (pct > ceil_pct) pct = ceil_pct;
  c.pct = pct;
  return c;
}

}  // namespace pool_control
}  // namespace esphome
