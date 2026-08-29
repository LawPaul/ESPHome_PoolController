// Host unit tests for the plumbing arbiter (who owns the diverters + pattern).
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_plumbing_arbiter.cpp -o /tmp/patest && /tmp/patest
#include "../components/pool_control/plumbing_arbiter.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace esphome::pool_control;

// Convenience builder with sane defaults; tweak per case.
static PlumbingInputs mk() {
  PlumbingInputs in{};
  in.base_mode = 1;        // SPA
  in.service = false;
  in.freeze_active = false;
  in.cleaning_active = false;
  in.clean_phase = CLEAN_SCRUB;
  in.sec_of_day = 0;
  in.rotate_s = 300;       // 5 min rotation
  in.normal_flow = 44.0f;
  return in;
}

int main() {
  // --- Priority: SERVICE > FREEZE > CLEANING > SCENE ---------------------
  {
    // Everything asserted at once: SERVICE wins, and it's hands-off.
    PlumbingInputs in = mk();
    in.service = true;
    in.freeze_active = true;
    in.cleaning_active = true;
    PlumbingResolution r = resolve_plumbing(in);
    assert(r.owner == PlumbingOwner::SERVICE);
    assert(!r.actuate);
  }
  {
    // Freeze beats cleaning + scene.
    PlumbingInputs in = mk();
    in.freeze_active = true;
    in.cleaning_active = true;
    PlumbingResolution r = resolve_plumbing(in);
    assert(r.owner == PlumbingOwner::FREEZE);
    assert(r.actuate && r.forced_flow);
  }
  {
    // Cleaning beats scene.
    PlumbingInputs in = mk();
    in.cleaning_active = true;
    PlumbingResolution r = resolve_plumbing(in);
    assert(r.owner == PlumbingOwner::CLEANING);
  }
  {
    // Nothing overriding -> scene.
    PlumbingResolution r = resolve_plumbing(mk());
    assert(r.owner == PlumbingOwner::SCENE);
  }

  // --- FREEZE pattern: rotation from sec_of_day, forced flow, no fountains --
  {
    PlumbingInputs in = mk();
    in.freeze_active = true;
    in.normal_flow = 30.0f;      // below the 48 GPM floor
    // Phase 0 (t=0): pool suction / skimmer.
    in.sec_of_day = 0;
    PlumbingResolution r0 = resolve_plumbing(in);
    assert(r0.owner == PlumbingOwner::FREEZE);
    assert(!r0.valve1_spa && !r0.valve2_drain && !r0.valve3_spa);
    assert(!r0.valve4_fountain && !r0.blower);
    assert(r0.forced_flow && r0.forced_gpm == 48.0f);   // floored up to 48
    // Phase 1 (t=rotate_s): pool suction / main drain.
    in.sec_of_day = in.rotate_s;
    PlumbingResolution r1 = resolve_plumbing(in);
    assert(!r1.valve1_spa && r1.valve2_drain && !r1.valve3_spa);
    // Phase 2 (t=2*rotate_s): spa loop.
    in.sec_of_day = 2 * in.rotate_s;
    PlumbingResolution r2 = resolve_plumbing(in);
    assert(r2.valve1_spa && r2.valve3_spa && !r2.valve4_fountain);
    // Forced flow keeps the higher of 48 and normal_flow.
    in.normal_flow = 60.0f;
    assert(resolve_plumbing(in).forced_gpm == 60.0f);
  }

  // --- CLEANING pattern mirrors cleaning_plumbing for the live phase --------
  {
    PlumbingInputs in = mk();
    in.cleaning_active = true;
    for (int ph : {CLEAN_SPA, CLEAN_SPILLOVER, CLEAN_SCRUB}) {
      in.clean_phase = ph;
      PlumbingResolution r = resolve_plumbing(in);
      ScenePlumbing p = cleaning_plumbing(static_cast<CleanPhase>(ph));
      assert(r.owner == PlumbingOwner::CLEANING);
      assert(!r.forced_flow && r.flow == p.flow);
      assert(r.valve1_spa == p.valve1_spa && r.valve2_drain == p.valve2_drain &&
             r.valve3_spa == p.valve3_spa && r.valve4_fountain == p.valve4_fountain);
      assert(r.blower == p.blower);
    }
  }

  // --- SCENE pattern mirrors scene_plumbing for the base mode ---------------
  {
    for (int m = 0; m <= 6; ++m) {
      PlumbingInputs in = mk();
      in.base_mode = m;
      PlumbingResolution r = resolve_plumbing(in);
      ScenePlumbing p = scene_plumbing(m);
      assert(r.owner == PlumbingOwner::SCENE);
      assert(!r.forced_flow && r.flow == p.flow);
      assert(r.valve1_spa == p.valve1_spa && r.valve2_drain == p.valve2_drain &&
             r.valve3_spa == p.valve3_spa && r.valve4_fountain == p.valve4_fountain);
      assert(r.blower == p.blower);
    }
  }

  std::printf("plumbing_arbiter: all tests passed\n");
  return 0;
}
