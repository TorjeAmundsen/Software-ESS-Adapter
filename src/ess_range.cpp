//ess_range.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "ess_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ess.hpp"
#include "notch.hpp"
#include "soh.hpp"

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

float oot_magnitude(int x, int y) {
    auto rel = [](int c) {
        const int dz = OOT_DEAD_ZONE, top = OOT_DEAD_ZONE + OOT_REL_MAX;
        return c > dz ? std::min(c, top) - dz : c < -dz ? std::max(c, -top) + dz : 0;
    };
    const float rx = static_cast<float>(rel(x)), ry = static_cast<float>(rel(y));
    return std::min(std::sqrt(rx * rx + ry * ry), 60.0f);
}

// Running speed at a magnitude on flat ground, as Player_CalcSpeedAndYawFromControlStick works it
// out, before the speed cap. Math_CosS reads a table in steps of 16 angle units, so this rounds
// the angle down the same way.
static double oot_running_speed(float magnitude) {
    const float over = magnitude - 20.0f;
    if (over < 0) {
        return 0;
    }
    const int angle = static_cast<int16_t>(over * 450.0f) & ~15;
    const double cosine = std::cos(angle * 3.14159265358979323846 / 32768);
    const double temp = 1.0 - cosine;
    return (temp * temp * 30.0 + 7.0) * 0.14;
}

OotZone oot_zone(int x, int y) {
    const float magnitude = oot_magnitude(x, y);
    if (magnitude == 0) {
        return OotZone::DeadZone;
    }
    if (magnitude < OOT_WALK_MAGNITUDE) {
        return OotZone::Ess;
    }
    if (magnitude * 0.8f * 0.14f <= 4.9f) {
        return OotZone::Walk;
    }
    constexpr double SPEED_CAP = 600 / 100.0;  // R_RUN_SPEED_LIMIT, normal boots
    return oot_running_speed(magnitude) < SPEED_CAP ? OotZone::Run : OotZone::FullSpeed;
}

int vc_in_game(int gc_offset) {
    int in_game[2];
    vc_in_game(gc_offset, 0, in_game);
    return in_game[0];
}

void game_in_game(Game game, uint8_t x, uint8_t y, int in_game[2]) {
    if (game == Game::SoH) {
        soh_in_game(x, y, in_game);
    } else {
        vc_in_game(x - 128, y - 128, in_game);
    }
}

int stock_in_game(Game game, int distance) {
    uint8_t stick[2] = {static_cast<uint8_t>(128 + distance), 128};
    if (game == Game::SoH) {
        int in_game[2];
        n64_in_game(stick, in_game);
        return in_game[0];
    }
    invert_vc_gc(stick);
    return vc_in_game(stick[0] - 128);
}

int physical_start(Game game, int in_game) {
    for (int d = 0; d < ESS_MAX_END; ++d) {
        if (stock_in_game(game, d) >= in_game) {
            return d;
        }
    }
    return ESS_MAX_END;
}

int physical_end(Game game, int in_game) {
    int last = 0;
    for (int d = 0; d <= ESS_MAX_END; ++d) {
        if (stock_in_game(game, d) <= in_game) {
            last = d;
        }
    }
    return last;
}

int default_ess_start(Game game) {
    return stock_in_game(game, physical_start(game, OOT_ESS_START));
}

int stock_ess_start(Game game) {
    static const int start[2] = {physical_start(Game::WiiVC, OOT_ESS_START), physical_start(Game::SoH, OOT_ESS_START)};
    return start[game == Game::SoH];
}

int stock_ess_end(Game game) {
    static const int end[2] = {physical_end(Game::WiiVC, ESS_IN_GAME_END), physical_end(Game::SoH, ESS_IN_GAME_END)};
    return end[game == Game::SoH];
}

int max_in_game_end(Game game) {
    return stock_in_game(game, ESS_MAX_END);
}

int physical_dead_zone(Game game, int in_game) {
    return in_game > 0 ? physical_start(game, in_game + 1) : 0;
}

double dead_zone_after_range(Game game, int distance, int start) {
    return static_cast<double>(distance) * stock_ess_start(game) / start;
}

void apply_dead_zone(uint8_t stick[2], double distance) {
    const int x = stick[0] - 128, y = stick[1] - 128;
    if (std::abs(x) < distance && std::abs(y) < distance) {
        stick[0] = 128;
        stick[1] = 128;
    }
}

void remap_ess_range(Game game, uint8_t stick[2], int start, int end) {
    const double x = stick[0] - 128;
    const double y = stick[1] - 128;
    const double r = std::sqrt(x * x + y * y);
    if (r == 0 || r >= FULL_TILT) {
        return;
    }

    // Piecewise linear: [0, start] -> [0, S], [start, end] -> [S, E], [end, 105] -> [E, 105].
    const double S = stock_ess_start(game), E = stock_ess_end(game);
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
