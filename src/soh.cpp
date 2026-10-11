//soh.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "soh.hpp"

#include <algorithm>
#include <cmath>

#include "ess.hpp"

// OoT's largest stick value per axis, where the VC inversion stops too (OOT_MAX in ess.cpp).
static constexpr int OOT_MAX = 80;

void n64_in_game(const uint8_t stick[2], int in_game[2]) {
    // gc_to_n64 works on 0 <= y <= x <= 127, so fold into that the way invert_vc_gc does, then
    // unfold. It returns double resolution: 2n means n - 0.5 < value <= n and 2n + 1 means
    // n < value <= n + 0.5, so halving rounds to the nearest whole value.
    int sign[2];
    uint8_t coords[2];
    for (int i = 0; i < 2; ++i) {
        sign[i] = stick[i] >= 128 ? 1 : -1;
        coords[i] = static_cast<uint8_t>(stick[i] >= 128 ? stick[i] - 128 : stick[i] == 0 ? 127 : 128 - stick[i]);
    }
    const bool swap = coords[1] > coords[0];
    if (swap) {
        std::swap(coords[0], coords[1]);
    }
    gc_to_n64(coords);
    if (swap) {
        std::swap(coords[0], coords[1]);
    }
    for (int i = 0; i < 2; ++i) {
        in_game[i] = sign[i] * std::min(coords[i] >> 1, OOT_MAX);
    }
}

void fit_soh_octagon(int in_game[2]) {
    // The octagon's sides near each axis: |along| + 16/69 |across| <= 85, as in
    // ControllerStick::Process. On the edge itself SoH's math lands exactly on a truncation edge
    // (69, 69 can come out as 68 or 69 on either axis), so stay strictly inside.
    const double x = std::abs(in_game[0]), y = std::abs(in_game[1]);
    const double reach = std::max(x, y) + 16.0 / 69.0 * std::min(x, y);
    if (reach < 85) {
        return;
    }
    const double scale = 84.999 / reach;
    for (int i = 0; i < 2; ++i) {
        in_game[i] = static_cast<int>(in_game[i] * scale);  // toward center, so it stays inside
    }
}

void soh_in_game(uint8_t x, uint8_t y, int in_game[2], double* margin) {
    // libultraship's names and types, so this reads side by side with the original.
    constexpr float MAX_AXIS_RANGE = 85.0f;
    constexpr double M_PI_ = 3.14159265358979323846;
    constexpr float mSensitivity = 100 / 100.0f;
    constexpr float mDeadzone = MAX_AXIS_RANGE * (0 / 100.0f);

    // DSUAxisDirectionValue times MAX_AXIS_RANGE: one direction of one axis, 0 to 85.
    auto direction_value = [&](uint8_t byte, int direction) {
        const int offset = (byte - 128) * direction;
        return std::clamp(offset / 127.0f, 0.0f, 1.0f) * MAX_AXIS_RANGE;
    };
    double sx = direction_value(x, 1) - direction_value(x, -1);
    double sy = direction_value(y, 1) - direction_value(y, -1);

    double ux = std::fabs(sx) * mSensitivity;
    double uy = std::fabs(sy) * mSensitivity;

    auto len = std::sqrt(ux * ux + uy * uy);
    if (len == 0) {
        // libultraship divides 0 by 0 here and the NaN converts to 0. Skip that.
        in_game[0] = in_game[1] = 0;
        if (margin) {
            *margin = 1;
        }
        return;
    }
    if (len < mDeadzone) {
        len = 0;
    } else if (len > MAX_AXIS_RANGE) {
        len = MAX_AXIS_RANGE / len;
    } else {
        len = (len - mDeadzone) * MAX_AXIS_RANGE / (MAX_AXIS_RANGE - mDeadzone) / len;
    }
    ux *= len;
    uy *= len;

    // The octagon. libultraship takes edgey's sign from the output y, which is still 0 here.
    if (ux != 0.0 && uy != 0.0) {
        auto slope = uy / ux;
        auto edgex = std::copysign(MAX_AXIS_RANGE / (std::fabs(slope) + 16.0 / 69.0), ux);
        auto edgey = std::copysign(std::min(std::fabs(edgex * slope), MAX_AXIS_RANGE / (1.0 / std::fabs(slope) + 16.0 / 69.0)), 0.0);
        edgex = edgey / slope;

        auto scale = std::sqrt(edgex * edgex + edgey * edgey) / MAX_AXIS_RANGE;
        ux *= scale;
        uy *= scale;
    }

    // Notch snapping, off at angle 0, but past 0.9 of the range it still turns the stick into an
    // angle and back.
    const double distance = std::sqrt((ux * ux) + (uy * uy)) / MAX_AXIS_RANGE;
    if (distance >= 0.9) {
        auto angle = std::atan2(uy, ux) + 2 * M_PI_;
        ux = std::cos(angle) * distance * MAX_AXIS_RANGE;
        uy = std::sin(angle) * distance * MAX_AXIS_RANGE;
    }

    // Assigned to int8_t: truncated toward zero.
    in_game[0] = static_cast<int>(std::copysign(ux, sx));
    in_game[1] = static_cast<int>(std::copysign(uy, sy));
    if (margin) {
        // The truncation gives in_game for magnitudes from |in_game| up to |in_game| + 1, and for
        // 0 anything under 1.
        auto axis_margin = [](double magnitude, int value) {
            const double low = std::abs(value), high = low + 1;
            return value == 0 ? high - magnitude : std::min(magnitude - low, high - magnitude);
        };
        *margin = std::min(axis_margin(std::fabs(ux), in_game[0]), axis_margin(std::fabs(uy), in_game[1]));
    }
}
