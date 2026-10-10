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
//   4. the ESS map (only with ESS on).
// `calibrated`, if given, gets the stick after step 1: the stick the settings plot shows.
void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2] = nullptr);
