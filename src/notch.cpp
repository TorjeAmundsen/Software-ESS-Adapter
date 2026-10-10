//notch.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "notch.hpp"

#include <cmath>

const char* const NOTCH_NAMES[NOTCH_COUNT] = {"Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left", "Left", "Up-Left"};

static constexpr int8_t S = GATE_STRAIGHT, D = GATE_DIAGONAL;
const int8_t IDEAL_NOTCHES[NOTCH_COUNT][2] = {{0, S}, {D, D}, {S, 0}, {D, -D}, {0, -S}, {-D, -D}, {-S, 0}, {-D, D}};

void set_neutral_samples(NotchCalibration& calibration, const uint8_t samples[NOTCH_COUNT][2]) {
    for (int axis = 0; axis < 2; ++axis) {
        int sum = 0;
        uint8_t low = 255, high = 0;
        for (int i = 0; i < NOTCH_COUNT; ++i) {
            const uint8_t value = samples[i][axis];
            calibration.neutral_samples[i][axis] = value;
            sum += value;
            low = value < low ? value : low;
            high = value > high ? value : high;
        }
        calibration.neutral[axis] = static_cast<uint8_t>((sum + NOTCH_COUNT / 2) / NOTCH_COUNT);
        calibration.neutral_min[axis] = low;
        calibration.neutral_max[axis] = high;
    }
}

bool in_neutral_range(const NotchCalibration& calibration, const uint8_t raw_stick[2]) {
    if (!calibration.valid) {
        return false;
    }
    for (int axis = 0; axis < 2; ++axis) {
        if (raw_stick[axis] < calibration.neutral_min[axis] || raw_stick[axis] > calibration.neutral_max[axis]) {
            return false;
        }
    }
    return true;
}

bool notch_in_section(int notch, int x, int y) {
    if (x * x + y * y < NOTCH_MIN_DISTANCE * NOTCH_MIN_DISTANCE) {
        return false;
    }
    const double pi = 3.14159265358979323846;
    const double ideal = std::atan2(IDEAL_NOTCHES[notch][1], IDEAL_NOTCHES[notch][0]);
    return std::fabs(std::remainder(std::atan2(y, x) - ideal, 2 * pi)) < pi / NOTCH_COUNT;
}

int misplaced_notch(const NotchCalibration& calibration) {
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        if (!notch_in_section(i, calibration.notch[i][0], calibration.notch[i][1])) {
            return i;
        }
    }
    return -1;
}

NotchCalibration make_calibration(const uint8_t samples[CALIBRATION_STEPS][2]) {
    NotchCalibration calibration;
    uint8_t neutral_samples[NOTCH_COUNT][2];
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        neutral_samples[i][0] = samples[2 * i + 1][0];
        neutral_samples[i][1] = samples[2 * i + 1][1];
    }
    set_neutral_samples(calibration, neutral_samples);
    for (int axis = 0; axis < 2; ++axis) {
        for (int i = 0; i < NOTCH_COUNT; ++i) {
            const int offset = samples[2 * i][axis] - calibration.neutral[axis];
            calibration.notch[i][axis] = static_cast<int8_t>(offset < -128 ? -128 : offset > 127 ? 127 : offset);
        }
    }
    calibration.valid = true;
    return calibration;
}

void warp_to_ideal_notches(uint8_t stick[2], const NotchCalibration& calibration) {
    if (!calibration.valid) {
        return;
    }
    const double x = stick[0] - 128;
    const double y = stick[1] - 128;
    if (x == 0 && y == 0) {
        return;
    }

    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const int j = (i + 1) % NOTCH_COUNT;
        // Write the stick as a * notch_i + b * notch_j. Inside this slice both are >= 0.
        const double ix = calibration.notch[i][0], iy = calibration.notch[i][1];
        const double jx = calibration.notch[j][0], jy = calibration.notch[j][1];
        const double det = ix * jy - jx * iy;
        if (std::fabs(det) < 1e-9) {
            continue;  // two notches in a line with the center: no slice to map
        }
        const double a = (x * jy - jx * y) / det;
        const double b = (ix * y - x * iy) / det;
        if (a < -1e-9 || b < -1e-9) {
            continue;
        }
        const double out_x = a * IDEAL_NOTCHES[i][0] + b * IDEAL_NOTCHES[j][0];
        const double out_y = a * IDEAL_NOTCHES[i][1] + b * IDEAL_NOTCHES[j][1];
        auto to_byte = [](double v) {
            const long r = std::lround(v);
            return static_cast<uint8_t>((r < -128 ? -128 : r > 127 ? 127 : r) + 128);
        };
        stick[0] = to_byte(out_x);
        stick[1] = to_byte(out_y);
        return;
    }
}

void snap_to_notch(uint8_t stick[2], const NotchCalibration& calibration, int distance) {
    if (!calibration.valid || distance <= 0) {
        return;
    }

    const int x = stick[0] - 128;
    const int y = stick[1] - 128;
    int best = -1;
    int best_dist2 = distance * distance;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const int dx = x - IDEAL_NOTCHES[i][0];
        const int dy = y - IDEAL_NOTCHES[i][1];
        const int dist2 = dx * dx + dy * dy;
        if (dist2 <= best_dist2) {
            best = i;
            best_dist2 = dist2;
        }
    }

    if (best >= 0) {
        stick[0] = static_cast<uint8_t>(128 + IDEAL_NOTCHES[best][0]);
        stick[1] = static_cast<uint8_t>(128 + IDEAL_NOTCHES[best][1]);
    }
}
