#include "pool_control.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace pool_control {

static const char *const TAG = "pool_control";

// Persisted demand-observer state. Kept separate from base mode so a firmware
// change to either struct never resets the other.
struct DemandState {
  float demand;
  float anchor_ts;
  float anchor_fc;
  float anchor_equiv_h;
} PACKED;  // NOLINT

void PoolControl::setup() {
  this->mode_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("pool_control_mode"));
  uint8_t bm = 0;
  if (this->mode_pref_.load(&bm)) {
    modes_.set_base_mode(static_cast<PoolMode>(bm));
    ESP_LOGCONFIG(TAG, "Restored base mode: %s", pool_mode_name(modes_.base_mode()));
  }

  this->demand_pref_ = global_preferences->make_preference<DemandState>(fnv1_hash("pool_control_demand"));
  DemandState d{};
  if (this->demand_pref_.load(&d)) {
    demand_.restore(d.demand, d.anchor_ts, d.anchor_fc, d.anchor_equiv_h);
    ESP_LOGCONFIG(TAG, "Restored demand=%.2f ppm/day (anchor fc=%.1f equiv=%.1fh)",
                  d.demand, d.anchor_fc, d.anchor_equiv_h);
  }
}

void PoolControl::save_demand_() {
  DemandState d{demand_.demand(), demand_.anchor_ts(), demand_.anchor_fc(),
                demand_.anchor_equiv_h()};
  this->demand_pref_.save(&d);
}

void PoolControl::observe_demand(float ts, float fc, float equiv_hours,
                                 float ppm_per_equiv_h) {
  auto r = demand_.on_reading(ts / 86400.0f, fc, equiv_hours, ppm_per_equiv_h);
  switch (r.status) {
    case DemandResult::Status::Updated:
      ESP_LOGI(TAG, "Demand: window %.2f ppm/day over %.1fd -> smoothed %.2f",
               r.window_demand, r.window_days, demand_.demand());
      this->save_demand_();
      break;
    case DemandResult::Status::Anchored:
      ESP_LOGI(TAG, "Demand: window opened at FC %.1f", fc);
      this->save_demand_();
      break;
    case DemandResult::Status::Restarted:
      ESP_LOGW(TAG, "Demand: anchor unusable (stale or hours reset); reopened");
      this->save_demand_();
      break;
    case DemandResult::Status::Accumulating:
      ESP_LOGD(TAG, "Demand: window at %.1fd, need %.1fd", r.window_days,
               demand_.min_window_days);
      break;
    case DemandResult::Status::Invalid:
      break;
  }
}

void PoolControl::save_mode_() {
  uint8_t bm = static_cast<uint8_t>(modes_.base_mode());
  this->mode_pref_.save(&bm);
}

void PoolControl::set_base_mode(int m) {
  modes_.set_base_mode(static_cast<PoolMode>(m));
  this->save_mode_();
  ESP_LOGI(TAG, "Base mode -> %s", pool_mode_name(modes_.base_mode()));
}

void PoolControl::dump_config() {
  ESP_LOGCONFIG(TAG, "Pool Control:");
  ESP_LOGCONFIG(TAG, "  demand=%.2f ppm/day", demand_.demand());
  ESP_LOGCONFIG(TAG, "  base mode=%s effective=%s", pool_mode_name(modes_.base_mode()),
                pool_mode_name(modes_.effective_mode()));
}

bool PoolControl::apply_mass_balance(float fc, float target, bool alarm,
                                     float tau_days, float ppm_per_equiv_h,
                                     float generating_hours_per_day,
                                     float floor_pct, float ceil_pct) {
  if (boost_active_ || alarm)
    return false;  // boost or alarm owns the output
  auto c = esphome::pool_control::mass_balance_output(
      demand_.demand(), fc, target, tau_days, ppm_per_equiv_h,
      generating_hours_per_day, floor_pct, ceil_pct);
  if (std::isnan(c.pct)) {
    ESP_LOGD(TAG, "Mass balance: demand not established yet; holding setpoint");
    return false;
  }
  ESP_LOGI(TAG, "Mass balance: demand %.2f + (%.1f->%.1f)/%.1fd = %.2f ppm/day -> %.0f%%",
           demand_.demand(), fc, target, tau_days, c.production_needed, c.pct);
  this->set_output_direct(c.pct);
  return true;
}

void PoolControl::set_output_direct(float value) {
  if (this->output_number_ == nullptr)
    return;
  auto call = this->output_number_->make_call();
  call.set_value(value);
  call.perform();
}

}  // namespace pool_control
}  // namespace esphome
