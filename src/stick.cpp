//stick.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "stick.hpp"

#include <cstdlib>

#include "ess.hpp"

// Steps 1 to 4 of process_stick.
static void run_steps(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2]) {
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

void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2]) {
    const uint8_t in[2] = {stick[0], stick[1]};
    run_steps(stick, notches, settings, calibrated);
    if (!settings.ess) {
        return;
    }
    // Step 5: each output axis becomes the furthest from center it gets anywhere between its own
    // input at center and where it is now, with the other axis's input held.
    for (int axis = 0; axis < 2; ++axis) {
        const int other = 1 - axis;
        const int direction = in[axis] > 128 ? 1 : -1;
        for (int v = 128; v != in[axis]; v += direction) {
            uint8_t probe[2];
            probe[axis] = static_cast<uint8_t>(v);
            probe[other] = in[other];
            run_steps(probe, notches, settings, nullptr);
            if (std::abs(probe[axis] - 128) > std::abs(stick[axis] - 128)) {
                stick[axis] = probe[axis];
            }
        }
    }
}
