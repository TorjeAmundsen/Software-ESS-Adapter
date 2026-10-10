//stick.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "stick.hpp"

#include "ess.hpp"

void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2]) {
    warp_to_ideal_notches(stick, notches);
    snap_to_notch(stick, notches, settings.snap_distance);
    if (calibrated) {
        calibrated[0] = stick[0];
        calibrated[1] = stick[1];
    }
    if (settings.ess) {
        remap_ess_range(stick, settings.range_start, settings.range_end);
    }
    // The dead zone goes after everything that moves the stick, scaled along with the ESS range.
    const double dead_zone =
        settings.ess ? dead_zone_after_range(settings.dead_zone_distance, settings.range_start) : settings.dead_zone_distance;
    apply_dead_zone(stick, dead_zone);
    if (settings.ess) {
        invert_vc_gc(stick);
    }
}
