// Host unit tests for the mass-balance chlorine demand observer.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_chlorine_demand.cpp -o /tmp/cdtest && /tmp/cdtest
#include "../components/pool_control/chlorine_demand.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::DemandObserver;
using esphome::pool_control::DemandResult;
using esphome::pool_control::mass_balance_output;
using esphome::pool_control::output_for_demand;
using esphome::pool_control::ppm_per_equiv_hour;

using Status = DemandResult::Status;

static bool near(float a, float b, float tol = 1e-3f) {
  return std::fabs(a - b) < tol;
}

int main() {
  // This pool: 27,000 gal, 2.0 lb/day cell.
  const float K = ppm_per_equiv_hour(2.0f, 27000.0f);
  assert(near(K, 0.3704f));            // ppm per equivalent full-output hour
  assert(near(K * 24.0f, 8.889f));     // ppm/day at 100% with a 24/7 pump

  // Bad geometry -> no constant rather than a divide-by-zero.
  assert(std::isnan(ppm_per_equiv_hour(2.0f, 0.0f)));
  assert(std::isnan(ppm_per_equiv_hour(0.0f, 27000.0f)));

  // --- Steady state: FC flat means production IS demand -------------------
  {
    DemandObserver o;
    // 30% output, 24 h/day -> 7.2 equivalent hours per day.
    const float equiv_per_day = 24.0f * 0.30f;
    auto r0 = o.on_reading(0.0f, 7.0f, 0.0f, K);
    assert(r0.status == Status::Anchored);
    assert(std::isnan(o.demand()));

    auto r1 = o.on_reading(4.0f, 7.0f, 4.0f * equiv_per_day, K);
    assert(r1.status == Status::Updated);
    assert(near(r1.window_demand, K * equiv_per_day));  // 2.67 ppm/day
    assert(near(o.demand(), K * equiv_per_day));        // first window seeds
  }

  // --- FC rising means production exceeded demand -------------------------
  {
    DemandObserver o;
    const float equiv_per_day = 24.0f * 0.30f;
    o.on_reading(0.0f, 7.0f, 0.0f, K);
    // Produced 2.67 ppm/day; FC gained 1.0 ppm/day -> demand 1.67 ppm/day.
    auto r = o.on_reading(4.0f, 11.0f, 4.0f * equiv_per_day, K);
    assert(r.status == Status::Updated);
    assert(near(r.window_demand, K * equiv_per_day - 1.0f));
  }

  // --- Windows shorter than the minimum keep the anchor -------------------
  {
    DemandObserver o;
    o.on_reading(0.0f, 9.0f, 0.0f, K);
    auto a = o.on_reading(1.0f, 10.0f, 24.0f * 0.30f, K);
    assert(a.status == Status::Accumulating);
    assert(near(a.window_days, 1.0f));
    assert(std::isnan(o.demand()));
    // Anchor untouched, so the eventual window still spans the full 3 days.
    auto b = o.on_reading(3.0f, 9.5f, 3.0f * 24.0f * 0.30f, K);
    assert(b.status == Status::Updated);
    assert(near(b.window_days, 3.0f));
    // produced 8.0 ppm, FC gained 0.5 -> 2.5 ppm/day.
    assert(near(b.window_demand, (K * 3.0f * 7.2f - 0.5f) / 3.0f));
  }

  // --- EMA: a step in demand is approached, not jumped to -----------------
  {
    DemandObserver o;
    o.alpha = 0.3f;
    const float equiv_per_day = 24.0f * 0.30f;
    o.on_reading(0.0f, 7.0f, 0.0f, K);
    o.on_reading(3.0f, 7.0f, 3.0f * equiv_per_day, K);  // seeds at 2.67
    const float seed = o.demand();
    // Next window: FC falls 1 ppm/day -> raw demand 3.67, one third of the way.
    o.on_reading(6.0f, 4.0f, 6.0f * equiv_per_day, K);
    assert(o.demand() > seed && o.demand() < seed + 1.0f);
    assert(near(o.demand(), seed + 0.3f * 1.0f));
  }

  // --- Duplicate / out-of-order readings change nothing -------------------
  {
    DemandObserver o;
    o.on_reading(0.0f, 7.0f, 0.0f, K);
    o.on_reading(4.0f, 7.0f, 4.0f * 7.2f, K);
    const float d = o.demand();
    assert(o.on_reading(4.0f, 7.0f, 4.0f * 7.2f, K).status == Status::Invalid);
    assert(o.on_reading(3.0f, 7.0f, 4.0f * 7.2f, K).status == Status::Invalid);
    assert(near(o.demand(), d));
  }

  // --- Missing inputs are rejected, not folded in as zero -----------------
  {
    DemandObserver o;
    const float nan = std::nanf("");
    assert(o.on_reading(nan, 7.0f, 0.0f, K).status == Status::Invalid);
    assert(o.on_reading(0.0f, nan, 0.0f, K).status == Status::Invalid);
    assert(o.on_reading(0.0f, 7.0f, nan, K).status == Status::Invalid);
    assert(o.on_reading(0.0f, 7.0f, 0.0f, nan).status == Status::Invalid);
    assert(!o.has_anchor());
  }

  // --- A stale anchor re-anchors rather than averaging over a dead month --
  {
    DemandObserver o;
    o.on_reading(0.0f, 7.0f, 0.0f, K);
    auto r = o.on_reading(30.0f, 7.0f, 30.0f * 7.2f, K);
    assert(r.status == Status::Restarted);
    assert(std::isnan(o.demand()));
    assert(near(o.anchor_ts(), 30.0f));
  }

  // --- Lifetime hours going backwards means the counter was lost ----------
  {
    DemandObserver o;
    o.on_reading(0.0f, 7.0f, 500.0f, K);
    auto r = o.on_reading(4.0f, 7.0f, 3.0f, K);  // reflashed: counter restarted
    assert(r.status == Status::Restarted);
    assert(std::isnan(o.demand()));
  }

  // --- Test error cannot drive demand negative ----------------------------
  {
    DemandObserver o;
    o.on_reading(0.0f, 5.0f, 0.0f, K);
    // FC gained far more than the cell could have made -> clamp at zero.
    auto r = o.on_reading(4.0f, 12.0f, 4.0f * 24.0f * 0.05f, K);
    assert(r.status == Status::Updated);
    assert(near(r.window_demand, 0.0f));
    assert(near(o.demand(), 0.0f));
  }

  // --- restore() survives a reboot mid-window -----------------------------
  {
    DemandObserver o;
    o.restore(2.5f, 10.0f, 8.0f, 100.0f);
    assert(o.has_anchor() && near(o.demand(), 2.5f));
    auto r = o.on_reading(14.0f, 8.0f, 100.0f + 4.0f * 7.2f, K);
    assert(r.status == Status::Updated);
    assert(near(r.window_demand, K * 7.2f));
  }

  // --- output_for_demand: the inverse of the plant ------------------------
  {
    // 2.5 ppm/day of demand on a 24/7 pump needs ~28%.
    assert(near(output_for_demand(2.5f, K, 24.0f), 28.125f));
    // Same demand with the pump running half the day needs twice the output.
    assert(near(output_for_demand(2.5f, K, 12.0f), 56.25f));
    // Beyond the cell's capacity, clamp rather than command >100%.
    assert(near(output_for_demand(20.0f, K, 24.0f), 100.0f));
    assert(near(output_for_demand(0.0f, K, 24.0f), 0.0f));
    assert(std::isnan(output_for_demand(std::nanf(""), K, 24.0f)));
    assert(std::isnan(output_for_demand(2.5f, K, 0.0f)));
  }

  // --- mass_balance_output: FC on target is pure demand cover -------------
  {
    auto c = mass_balance_output(2.5f, 7.0f, 7.0f, 2.5f, K, 24.0f, 0.0f, 100.0f);
    assert(near(c.production_needed, 2.5f));
    assert(near(c.pct, 28.125f));
  }

  // --- Replay of 2026-07-26..29, demand 2.5 ppm/day, tau 2.5 d ------------
  // The trim controller produced 40 / 30 / 20 / 10 over these same readings,
  // ratcheting down by its slew limit even on the day FC had already turned.
  {
    const float d = 2.5f, tau = 2.5f, target = 7.0f;
    auto at = [&](float fc) {
      return mass_balance_output(d, fc, target, tau, K, 24.0f, 0.0f, 100.0f).pct;
    };
    assert(std::lround(at(10.0f)) == 15);
    assert(std::lround(at(11.5f)) == 8);
    assert(std::lround(at(9.5f)) == 17);
    // Absolute, not incremental: the 9.5 command does not depend on having
    // passed through 11.5 first, so there is no ratchet to unwind.
    assert(near(at(9.5f), at(9.5f)));
  }

  // --- FC far above target: stop, never command negative production -------
  {
    auto c = mass_balance_output(2.5f, 20.0f, 7.0f, 2.5f, K, 24.0f, 0.0f, 100.0f);
    assert(near(c.production_needed, 0.0f));
    assert(near(c.pct, 0.0f));
  }

  // --- FC far below target saturates at the ceiling ----------------------
  {
    auto c = mass_balance_output(2.5f, 0.0f, 7.0f, 0.5f, K, 24.0f, 0.0f, 100.0f);
    assert(near(c.pct, 100.0f));
  }

  // --- Floor and ceiling are honoured ------------------------------------
  {
    assert(near(mass_balance_output(2.5f, 20.0f, 7.0f, 2.5f, K, 24.0f, 10.0f, 100.0f).pct,
                10.0f));
    assert(near(mass_balance_output(2.5f, 0.0f, 7.0f, 0.5f, K, 24.0f, 0.0f, 60.0f).pct,
                60.0f));
  }

  // --- A shorter horizon corrects harder ---------------------------------
  {
    const float slow = mass_balance_output(2.5f, 5.0f, 7.0f, 5.0f, K, 24.0f, 0.0f, 100.0f).pct;
    const float fast = mass_balance_output(2.5f, 5.0f, 7.0f, 1.0f, K, 24.0f, 0.0f, 100.0f).pct;
    assert(fast > slow);
  }

  // --- Nothing is commanded before demand is known -----------------------
  {
    const float nan = std::nanf("");
    assert(std::isnan(mass_balance_output(nan, 7.0f, 7.0f, 2.5f, K, 24.0f, 0.0f, 100.0f).pct));
    assert(std::isnan(mass_balance_output(2.5f, nan, 7.0f, 2.5f, K, 24.0f, 0.0f, 100.0f).pct));
    assert(std::isnan(mass_balance_output(2.5f, 7.0f, nan, 2.5f, K, 24.0f, 0.0f, 100.0f).pct));
    assert(std::isnan(mass_balance_output(2.5f, 7.0f, 7.0f, 0.0f, K, 24.0f, 0.0f, 100.0f).pct));
    assert(std::isnan(mass_balance_output(2.5f, 7.0f, 7.0f, 2.5f, K, 0.0f, 0.0f, 100.0f).pct));
  }

  // --- Observer and command close the loop on a steady pool --------------
  // Learn demand from a flat window, then command from it: the output that
  // produced the flat FC is the output that comes back out.
  {
    DemandObserver o;
    const float equiv_per_day = 24.0f * 0.28125f;  // the 2.5 ppm/day output
    o.on_reading(0.0f, 7.0f, 0.0f, K);
    o.on_reading(4.0f, 7.0f, 4.0f * equiv_per_day, K);
    auto c = mass_balance_output(o.demand(), 7.0f, 7.0f, 2.5f, K, 24.0f, 0.0f, 100.0f);
    assert(near(c.pct, 28.125f, 0.01f));
  }

  std::printf("chlorine_demand: all tests passed\n");
  return 0;
}
