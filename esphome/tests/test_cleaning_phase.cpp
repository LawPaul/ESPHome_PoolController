// Host unit tests for the cleaning-cycle deadline math.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_cleaning_phase.cpp -o /tmp/ctest && /tmp/ctest
#include "../components/pool_control/cleaning_phase.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace esphome::pool_control;

// spa 10min, spillover 5min, scrub 20min, prime 60s.
static CleanDurations dur() {
  return CleanDurations{10u * 60000u, 5u * 60000u, 20u * 60000u, 60000u};
}

int main() {
  const CleanDurations d = dur();
  const uint32_t end_spa = d.spa_ms;                       // 600000
  const uint32_t end_spillover = end_spa + d.spillover_ms; // 900000
  const uint32_t end_scrub = end_spillover + d.scrub_ms;   // 2100000

  // Phase boundaries (half-open: [start, end)).
  assert(cleaning_tick(0, d, false).phase == CLEAN_SPA);
  assert(cleaning_tick(end_spa - 1, d, false).phase == CLEAN_SPA);
  assert(cleaning_tick(end_spa, d, false).phase == CLEAN_SPILLOVER);
  assert(cleaning_tick(end_spillover - 1, d, false).phase == CLEAN_SPILLOVER);
  assert(cleaning_tick(end_spillover, d, false).phase == CLEAN_SCRUB);
  assert(cleaning_tick(end_scrub - 1, d, false).phase == CLEAN_SCRUB);
  assert(cleaning_tick(end_scrub, d, false).phase == CLEAN_DONE);
  assert(cleaning_tick(end_scrub + 999999u, d, false).phase == CLEAN_DONE);

  // Booster is only permitted during scrub, and only after the prime window.
  assert(!cleaning_tick(0, d, false).booster_permit);                       // spa
  assert(!cleaning_tick(end_spa, d, false).booster_permit);                 // spillover
  assert(!cleaning_tick(end_spillover, d, false).booster_permit);           // scrub, t=0 into scrub
  assert(!cleaning_tick(end_spillover + d.prime_ms - 1, d, false).booster_permit); // still priming
  assert(cleaning_tick(end_spillover + d.prime_ms, d, false).booster_permit);      // prime done
  assert(cleaning_tick(end_scrub - 1, d, false).booster_permit);            // late scrub
  assert(!cleaning_tick(end_scrub, d, false).booster_permit);               // done: booster off
  assert(!cleaning_tick(end_scrub + 1, d, false).booster_permit);           // done

  // Freeze suppresses the booster even in the middle of the scrub phase.
  assert(!cleaning_tick(end_spillover + d.prime_ms + 1000, d, true).booster_permit);
  // ...but the phase itself is unaffected by freeze.
  assert(cleaning_tick(end_spillover + d.prime_ms + 1000, d, true).phase == CLEAN_SCRUB);

  // Zero-length phases collapse cleanly (e.g. spillover disabled).
  {
    CleanDurations z{5000u, 0u, 5000u, 0u};
    assert(cleaning_tick(0, z, false).phase == CLEAN_SPA);
    assert(cleaning_tick(5000, z, false).phase == CLEAN_SCRUB);   // spillover skipped
    assert(cleaning_tick(5000, z, false).booster_permit);          // prime=0 => immediate
    assert(cleaning_tick(10000, z, false).phase == CLEAN_DONE);    // scrub done
  }

  // --- cleaning_plumbing: each phase's absolute diverter/flow pattern ---
  {
    // Phase 1 SPA: spa suction + spa return, clean flow, no blower.
    ScenePlumbing p1 = cleaning_plumbing(CLEAN_SPA);
    assert(p1.valve1_spa && !p1.valve2_drain && p1.valve3_spa && !p1.valve4_fountain);
    assert(!p1.blower && p1.flow == FlowSel::CLEAN);
    // Phase 2 SPILLOVER: pool suction (skimmer) + spa return -> spills to pool.
    ScenePlumbing p2 = cleaning_plumbing(CLEAN_SPILLOVER);
    assert(!p2.valve1_spa && !p2.valve2_drain && p2.valve3_spa && !p2.valve4_fountain);
    assert(p2.flow == FlowSel::CLEAN);
    // Phase 3 SCRUB: pool suction on main drain + pool return, booster feed flow.
    ScenePlumbing p3 = cleaning_plumbing(CLEAN_SCRUB);
    assert(!p3.valve1_spa && p3.valve2_drain && !p3.valve3_spa && !p3.valve4_fountain);
    assert(p3.flow == FlowSel::BOOSTER);
    // Only phase 3 pulls the main drain; no cleaning phase ever runs fountains
    // or the blower.
    for (CleanPhase ph : {CLEAN_SPA, CLEAN_SPILLOVER, CLEAN_SCRUB}) {
      assert(!cleaning_plumbing(ph).valve4_fountain);
      assert(!cleaning_plumbing(ph).blower);
    }
    assert(cleaning_plumbing(CLEAN_SCRUB).valve2_drain);
    assert(!cleaning_plumbing(CLEAN_SPA).valve2_drain);
    // IDLE / DONE fall back to the safe idle pattern.
    for (CleanPhase ph : {CLEAN_IDLE, CLEAN_DONE}) {
      ScenePlumbing p = cleaning_plumbing(ph);
      assert(!p.valve1_spa && !p.valve2_drain && !p.valve3_spa && !p.valve4_fountain);
      assert(!p.blower && p.flow == FlowSel::HOLDING);
    }
  }

  std::printf("cleaning_phase: all tests passed\n");
  return 0;
}
