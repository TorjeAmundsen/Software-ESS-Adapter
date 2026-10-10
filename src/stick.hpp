//stick.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// The main stick's path from the centered stick to what goes to Dolphin, in one place so the
// bridge loop and the tests run the same steps in the same order.

#pragma once
#include <cstdint>

#include "ess_range.hpp"
#include "notch.hpp"

struct StickSettings {
    bool ess = true;
    int snap_distance = 0;
    int range_start = ESS_DEFAULT_START;  // physical distances, see ess_range.hpp
    int range_end = ESS_DEFAULT_END;
    int dead_zone_distance = 0;  // physical_dead_zone of the software dead zone
};

// Runs a centered main stick (128 = center) through:
//   1. notch warp and snapping (only with calibrated notches),
//   2. the ESS range (only with ESS on),
//   3. the software dead zone, scaled along with the ESS range so it never reaches into ESS,
//   4. the ESS map (only with ESS on),
//   5. a pass that keeps each axis's in-game value moving the same way as its input.
// OoT speedrun rules say that with one input axis held, moving the other axis's input one way must
// never move that axis's output the other way. In game, after Wii VC, the ESS map can, by a unit
// or two near the diagonals and more with a wider ESS range. So can notch snapping and notch
// offsets, ESS on or off: they move both axes, and VC's length limit then pulls one back. Step 5
// works from center outward: each stick position gets the in-game pair closest to its normal
// result where neither axis is behind the position one step further in on that axis, and sends a
// stick VC turns into that pair. Where VC's length limit leaves no such pair, the positions further
// in get bounded and it starts over. The result for every position is worked out once per
// settings and calibration.
// `calibrated`, if given, gets the stick after step 1: the stick the settings plot shows.
void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2] = nullptr);
