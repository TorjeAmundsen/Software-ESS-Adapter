//notch.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Notch calibration, warping, and snapping. Warping and snapping work on centered stick bytes
// (128 = center), before the ESS map.

#pragma once
#include <cstdint>

// Notch order used everywhere: clockwise starting at Up.
constexpr int NOTCH_COUNT = 8;
extern const char* const NOTCH_NAMES[NOTCH_COUNT];

// The Gamecube stick range the ESS map expects: 105 straight, 75 per axis in the corners. The
// perfect notches sit exactly there.
constexpr int GATE_STRAIGHT = 105;
constexpr int GATE_DIAGONAL = 75;
extern const int8_t IDEAL_NOTCHES[NOTCH_COUNT][2];

constexpr int MAX_SNAP_DISTANCE = 5;
constexpr int DEFAULT_SNAP_DISTANCE = 0;

// Calibration alternates notches and neutral: Up, Neutral, Up-Right, Neutral, ... Up-Left,
// Neutral. The 8 neutral samples show where the stick can come to rest. Their average is the
// center the program uses instead of the first report after plugging in, so it survives replugs
// and power cycles, and anything inside their range counts as exactly neutral.
constexpr int CALIBRATION_STEPS = NOTCH_COUNT * 2;
inline bool is_neutral_step(int step) {
    return step % 2 == 1;
}
inline int notch_of_step(int step) {
    return step / 2;
}

struct NotchCalibration {
    bool valid = false;
    uint8_t neutral_samples[NOTCH_COUNT][2] = {};  // raw stick at rest, one per neutral step
    uint8_t neutral[2] = {128, 128};               // average of the samples
    uint8_t neutral_min[2] = {128, 128};           // smallest and largest sample on each axis:
    uint8_t neutral_max[2] = {128, 128};           // the range where the stick can rest
    int8_t notch[NOTCH_COUNT][2] = {};             // measured notch positions, offsets from neutral
    int8_t offset[NOTCH_COUNT][2] = {};            // notch offsets, added to the ideal notches, see notch_target
};

// Where holding a calibrated notch lands (its target): the ideal notch moved by that notch's
// offset, in stick units. A target follows the same rule as a calibrated notch: it has to lie in its notch's
// own section (see notch_in_section), so the warp still can't fold the stick over itself. It also
// has to fit in a stick byte. notch_offset_allowed checks both.
void notch_target(const NotchCalibration& calibration, int notch, int target[2]);
bool notch_offset_allowed(int notch, int dx, int dy);

// A notch only counts if it lies in its own section of the stick: less than 22.5 degrees from its
// direction, so it can't cross into a neighbor's section, and at least NOTCH_MIN_DISTANCE from
// neutral. That keeps the notches in clockwise order, so the warp can't fold the stick over itself.
// x and y are offsets from neutral.
constexpr int NOTCH_MIN_DISTANCE = GATE_STRAIGHT / 2;
bool notch_in_section(int notch, int x, int y);

// The first notch outside its section, or -1 if every notch is in place.
int misplaced_notch(const NotchCalibration& calibration);

// Builds a calibration from raw stick samples, one per step in the order above.
NotchCalibration make_calibration(const uint8_t samples[CALIBRATION_STEPS][2]);

// Stores the neutral samples and derives the average and range from them.
void set_neutral_samples(NotchCalibration& calibration, const uint8_t samples[NOTCH_COUNT][2]);

// True if a raw stick position lies inside the calibrated neutral range.
bool in_neutral_range(const NotchCalibration& calibration, const uint8_t raw_stick[2]);

// Warps the whole stick so the calibrated notches land exactly on their targets. The 8 notches
// split the stick into 8 slices, each between two neighboring notches and the center. Each slice
// is stretched evenly (a linear map that sends its two calibrated notches to their targets), so
// the center stays put and neighboring slices meet without a jump.
void warp_to_ideal_notches(uint8_t stick[2], const NotchCalibration& calibration);

// For an already warped stick: if it is within `distance` units of a notch target, moves it
// exactly onto that target. Picks the nearest notch if several are in range. Only calibrated
// ports snap.
void snap_to_notch(uint8_t stick[2], const NotchCalibration& calibration, int distance);
