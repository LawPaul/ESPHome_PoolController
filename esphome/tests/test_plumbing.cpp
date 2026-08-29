// Host unit tests for the scene plumbing truth table.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_plumbing.cpp -o /tmp/pltest && /tmp/pltest
#include "../components/pool_control/plumbing.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using esphome::pool_control::scene_plumbing;
using esphome::pool_control::ScenePlumbing;
using esphome::pool_control::FlowSel;
using esphome::pool_control::PoolMode;

static void expect(int mode, bool v1, bool v2, bool v3, bool v4, bool blower,
                   FlowSel flow) {
  ScenePlumbing p = scene_plumbing(mode);
  assert(p.valve1_spa == v1);
  assert(p.valve2_drain == v2);
  assert(p.valve3_spa == v3);
  assert(p.valve4_fountain == v4);
  assert(p.blower == blower);
  assert(p.flow == flow);
}

int main() {
  // --- Each base scene routes exactly as the old scene_* scripts did ---
  // POOL: idle holding, everything at pool, skimmer suction, no blower.
  expect(0, /*v1*/ false, /*v2*/ false, /*v3*/ false, /*v4*/ false,
         /*blower*/ false, FlowSel::HOLDING);
  // SPA: spa suction + spa return, normal flow, no blower.
  expect(1, true, false, true, false, false, FlowSel::NORMAL);
  // SPILLOVER: pool suction, spa return (spills to pool), normal flow.
  expect(2, false, false, true, false, false, FlowSel::NORMAL);
  // FOUNTAINS_LOW: pool loop via fountains, low flow.
  expect(3, false, false, false, true, false, FlowSel::FOUNTAIN_LOW);
  // FOUNTAINS_HIGH: pool loop via fountains, high flow.
  expect(4, false, false, false, true, false, FlowSel::FOUNTAIN_HIGH);
  // SPA_BUBBLES: spa loop, blower ON, bubbles flow.
  expect(5, true, false, true, false, true, FlowSel::BUBBLES);
  // AUTO: automatic program shares POOL's idle holding pattern (differs only in
  // that it permits the scheduled cleaning rotation, which lives elsewhere).
  expect(6, /*v1*/ false, /*v2*/ false, /*v3*/ false, /*v4*/ false,
         /*blower*/ false, FlowSel::HOLDING);

  // --- Only spa scenes route to the spa; only bubbles turns the blower on ---
  assert(scene_plumbing(1).valve1_spa && scene_plumbing(1).valve3_spa);
  assert(scene_plumbing(5).valve1_spa && scene_plumbing(5).valve3_spa);
  assert(scene_plumbing(5 /*bubbles*/).blower);  // bubbles is the only blower scene
  for (int m = 0; m <= 6; ++m)
    if (m != 5) assert(!scene_plumbing(m).blower);  // no other scene blows air

  // --- Only fountain scenes select valve4; no scene uses the main drain ---
  assert(scene_plumbing(3).valve4_fountain && scene_plumbing(4).valve4_fountain);
  for (int m = 0; m <= 6; ++m) {
    if (m != 3 && m != 4) assert(!scene_plumbing(m).valve4_fountain);
    assert(!scene_plumbing(m).valve2_drain);  // drain is a runtime/holding select
  }

  // --- Override / unknown modes fall back to the safe idle POOL scene ---
  ScenePlumbing pool = scene_plumbing(0);
  for (int m : {7 /*CLEANING*/, 8 /*FREEZE*/, 9 /*SERVICE*/, 99, -1}) {
    ScenePlumbing p = scene_plumbing(m);
    assert(p.valve1_spa == pool.valve1_spa && p.valve2_drain == pool.valve2_drain &&
           p.valve3_spa == pool.valve3_spa && p.valve4_fountain == pool.valve4_fountain &&
           p.blower == pool.blower && p.flow == pool.flow);
  }

  printf("test_plumbing: all assertions passed\n");
  return 0;
}
