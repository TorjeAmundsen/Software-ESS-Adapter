//ess_range.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "ess_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ess.hpp"
#include "notch.hpp"

static constexpr double FULL_TILT = GATE_STRAIGHT;  // the Gamecube straight range gc_to_n64 assumes

void vc_in_game(int gc_x, int gc_y, int in_game[2]) {
    // VC dead zone on each axis, then it scales the vector down to length 56, then a curve per
    // axis.
    auto dead_zone = [](int c) {
        return c > 15 ? c - 15 : c < -15 ? c + 15 : 0;
    };
    const double coords[2] = {static_cast<double>(dead_zone(gc_x)), static_cast<double>(dead_zone(gc_y))};
    const double length = std::sqrt(coords[0] * coords[0] + coords[1] * coords[1]);
    for (int i = 0; i < 2; ++i) {
        double c = coords[i];
        if (length > 56) {
            c = c * 56 / std::trunc(length);
        }
        c = std::trunc(std::trunc(c) / 56 * 127);
        const double sign = c >= 0 ? 1 : -1;
        in_game[i] = static_cast<int>(std::trunc(sign * 127 * (1 - std::sqrt(1 - std::fabs(c) / 127))));
    }
}

OotZone oot_zone(int x, int y) {
    auto rel = [](int c) {
        const int dz = OOT_DEAD_ZONE, top = OOT_DEAD_ZONE + OOT_REL_MAX;
        return c > dz ? std::min(c, top) - dz : c < -dz ? std::max(c, -top) + dz : 0;
    };
    const int rx = rel(x), ry = rel(y);
    if (rx == 0 && ry == 0) {
        return OotZone::DeadZone;
    }
    return std::sqrt(static_cast<double>(rx * rx + ry * ry)) < OOT_WALK_MAGNITUDE ? OotZone::Ess : OotZone::Walk;
}

int vc_in_game(int gc_offset) {
    int in_game[2];
    vc_in_game(gc_offset, 0, in_game);
    return in_game[0];
}

int stock_in_game(int distance) {
    uint8_t stick[2] = {static_cast<uint8_t>(128 + distance), 128};
    invert_vc_gc(stick);
    return vc_in_game(stick[0] - 128);
}

int physical_start(int in_game) {
    for (int d = 0; d < ESS_MAX_END; ++d) {
        if (stock_in_game(d) >= in_game) {
            return d;
        }
    }
    return ESS_MAX_END;
}

int physical_end(int in_game) {
    int last = 0;
    for (int d = 0; d <= ESS_MAX_END; ++d) {
        if (stock_in_game(d) <= in_game) {
            last = d;
        }
    }
    return last;
}

int max_in_game_end() {
    return stock_in_game(ESS_MAX_END);
}

int physical_dead_zone(int in_game) {
    return in_game > 0 ? physical_start(in_game + 1) : 0;
}

double dead_zone_after_range(int distance, int start) {
    return static_cast<double>(distance) * ESS_DEFAULT_START / start;
}

void apply_dead_zone(uint8_t stick[2], double distance) {
    const int x = stick[0] - 128, y = stick[1] - 128;
    if (std::abs(x) < distance && std::abs(y) < distance) {
        stick[0] = 128;
        stick[1] = 128;
    }
}

void remap_ess_range(uint8_t stick[2], int start, int end) {
    const double x = stick[0] - 128;
    const double y = stick[1] - 128;
    const double r = std::sqrt(x * x + y * y);
    if (r == 0 || r >= FULL_TILT) {
        return;
    }

    // Piecewise linear: [0, start] -> [0, S], [start, end] -> [S, E], [end, 105] -> [E, 105].
    const double S = ESS_DEFAULT_START, E = ESS_DEFAULT_END;
    double mapped;
    if (r < start) {
        // Stay just under the value that rounds up to S, so nothing below start enters the range.
        mapped = std::min(r * S / start, S - 0.501);
    } else if (r <= end) {
        mapped = S + (r - start) * (E - S) / (end - start);
    } else {
        // Stay at or over the value that rounds up past E, so nothing above end enters the range.
        mapped = std::max(E + (r - end) * (FULL_TILT - E) / (FULL_TILT - end), E + 0.5);
    }

    const double scale = mapped / r;
    stick[0] = static_cast<uint8_t>(std::clamp<long>(std::lround(x * scale), -128, 127) + 128);
    stick[1] = static_cast<uint8_t>(std::clamp<long>(std::lround(y * scale), -128, 127) + 128);
}
