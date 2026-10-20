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

int stock_full_speed_start(Game game) {
    auto first_full_speed = [](Game g) {
        for (int d = 0; d < FULL_TILT; ++d) {
            if (oot_zone(stock_in_game(g, d), 0) == OotZone::FullSpeed) {
                return d;
            }
        }
        return static_cast<int>(FULL_TILT);
    };
    static const int start[2] = {first_full_speed(Game::WiiVC), first_full_speed(Game::SoH)};
    return start[game == Game::SoH];
}

double auto_full_speed_start(Game game, int end) {
    // Where the even spread past the end puts the stock map's full speed edge (halfway between its
    // last running and first full speed distance), plus that half.
    const double stock_end = stock_ess_end(game), stock_edge = stock_full_speed_start(game) - 0.5;
    return end + (stock_edge - stock_end) * (FULL_TILT - end) / (FULL_TILT - stock_end) + 0.5;
}

int min_full_speed_start(int end) {
    return end + MIN_FULL_SPEED_GAP;
}

int max_in_game_end(Game game) {
    return std::min(stock_in_game(game, ESS_MAX_END), ESS_IN_GAME_MAX_END);
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

// The ESS map's N64 stage as the formula gc_to_n64 implements, before rounding, for
// 0 <= y <= x: the in-game values the stock map aims for, on a continuous stick.
static void n64_exact(double x, double y, double out[2]) {
    const double s = 5 * x + 2 * y;
    const double distance = std::min(s / 525, 1.0);
    const double closeness = s > 0 ? 7 * y / s : 0;
    const double scale = distance * distance * distance * closeness * (70.0 / 75 - 80.0 / 105) + 80.0 / 105;
    out[0] = x * scale;
    out[1] = y * scale;
}

// How far out the stock map's ESS starts and ends in a direction (a unit vector), in stick
// units: where the in-game stick leaves OoT's square dead zone (7.5, halfway between 7 and 8),
// and where its magnitude past that reaches 19.5 (halfway between 19 and 20, so 26.5 on a
// cardinal). Smooth in the direction, unlike the rounded values.
struct EssEdges {
    double start, end;
};
static EssEdges stock_ess_edges(double dx, double dy) {
    double a = std::fabs(dx), b = std::fabs(dy);
    if (b > a) {
        std::swap(a, b);
    }
    auto at = [&](double distance, double game[2]) {
        n64_exact(a * distance, b * distance, game);
    };
    // Both grow with distance, so halve the interval until it's tiny.
    auto solve = [&](auto past_edge) {
        double low = 0, high = FULL_TILT;
        for (int i = 0; i < 50; ++i) {
            const double mid = (low + high) / 2;
            double game[2];
            at(mid, game);
            (past_edge(game) ? high : low) = mid;
        }
        return (low + high) / 2;
    };
    EssEdges edges;
    edges.start = solve([](const double game[2]) {
        return game[0] >= OOT_DEAD_ZONE + 0.5;  // x is the larger axis
    });
    edges.end = solve([](const double game[2]) {
        const double rx = std::max(game[0] - OOT_DEAD_ZONE, 0.0), ry = std::max(game[1] - OOT_DEAD_ZONE, 0.0);
        return std::sqrt(rx * rx + ry * ry) >= OOT_WALK_MAGNITUDE - 0.5;
    });
    return edges;
}

// The zone the stock map puts a stick position in (origin-centered, whole units): what the game
// makes of the bytes the ESS map sends for it.
static OotZone stock_zone(Game game, int x, int y) {
    uint8_t stick[2] = {static_cast<uint8_t>(std::clamp(x + 128, 0, 255)), static_cast<uint8_t>(std::clamp(y + 128, 0, 255))};
    int in_game[2];
    if (game == Game::SoH) {
        n64_in_game(stick, in_game);
        fit_soh_octagon(in_game);
    } else {
        invert_vc_gc(stick);
        vc_in_game(stick[0] - 128, stick[1] - 128, in_game);
    }
    return oot_zone(in_game[0], in_game[1]);
}

// How far out the stock map's running and full speed start in a direction, in stick units,
// measured on the stock map itself: what the game makes of it through the Wii VC (or SoH) isn't
// what the N64 formula aims for, least of all on the diagonals. Along each ray, the first
// distance whose closest whole position runs, and the first that goes full speed, so on a
// cardinal running starts at 66.5, half a unit past the last walking distance. Rounding makes
// those jump back and forth by a unit or so from ray to ray, which stretching the stock map
// would make worse, so each is averaged over the rays within 8 degrees: the stock map's shape
// without its rounding. Every quarter degree of a quadrant, measured once per game; in between,
// the two neighbors are blended so the edges are smooth in the direction.
struct RunEdges {
    double run, full;
};
static RunEdges stock_run_edges(Game game, double x, double y) {
    constexpr int STEPS = 360;  // per quadrant
    constexpr int AVERAGE = 32;  // rays on each side, 8 degrees
    constexpr double HALF_PI = 1.57079632679489661923;
    struct Table {
        RunEdges edges[STEPS + 1];
        explicit Table(Game game) {
            RunEdges rays[STEPS + 1];
            for (int i = 0; i <= STEPS; ++i) {
                const double dx = std::cos(i * HALF_PI / STEPS), dy = std::sin(i * HALF_PI / STEPS);
                RunEdges& e = rays[i];
                e = {FULL_TILT, FULL_TILT};
                for (double r = 0; r < FULL_TILT; r += 0.05) {
                    const OotZone zone = stock_zone(game, static_cast<int>(std::lround(dx * r)), static_cast<int>(std::lround(dy * r)));
                    if (zone >= OotZone::Run && e.run == FULL_TILT) {
                        e.run = r;
                    }
                    if (zone == OotZone::FullSpeed) {
                        e.full = r;
                        break;
                    }
                }
            }
            // The stock map is the same mirrored across the axes, so rays past them mirror back.
            for (int i = 0; i <= STEPS; ++i) {
                RunEdges sum = {0, 0};
                for (int j = i - AVERAGE; j <= i + AVERAGE; ++j) {
                    const RunEdges& e = rays[j < 0 ? -j : j > STEPS ? 2 * STEPS - j : j];
                    sum.run += e.run;
                    sum.full += e.full;
                }
                constexpr int RAYS = 2 * AVERAGE + 1;
                edges[i] = {sum.run / RAYS, sum.full / RAYS};
            }
            // The average keeps the shape, but on the cardinals the edges sit exactly half a unit
            // past the last whole distance before them, as measured.
            auto half = [](double r) {
                return std::round(r * 2) / 2;
            };
            const double run_scale = half(rays[0].run) / edges[0].run, full_scale = half(rays[0].full) / edges[0].full;
            for (RunEdges& e : edges) {
                e.run *= run_scale;
                e.full *= full_scale;
            }
        }
    };
    static const Table tables[2] = {Table(Game::WiiVC), Table(Game::SoH)};
    const Table& table = tables[game == Game::SoH];
    const double at = std::atan2(std::fabs(y), std::fabs(x)) / HALF_PI * STEPS;
    const int i = std::min(static_cast<int>(at), STEPS - 1);
    const double t = at - i;
    const RunEdges& a = table.edges[i];
    const RunEdges& b = table.edges[i + 1];
    return {a.run + (b.run - a.run) * t, a.full + (b.full - a.full) * t};
}

void remap_ess_range(Game game, uint8_t stick[2], int start, int end, int full_speed) {
    const double x = stick[0] - 128;
    const double y = stick[1] - 128;
    const double r = std::sqrt(x * x + y * y);
    if (full_speed != AUTO_FULL_SPEED && full_speed == std::lround(auto_full_speed_start(game, end))) {
        // The automatic distance, rounded: the same as automatic.
        full_speed = AUTO_FULL_SPEED;
    }
    if (r == 0 || r >= FULL_TILT || (start == stock_ess_start(game) && end == stock_ess_end(game) && full_speed == AUTO_FULL_SPEED)) {
        return;
    }

    // Start, end and the stock anchors are cardinal distances. OoT's ESS edge isn't round (the
    // game's dead zone is square), so in other directions the stock map's ESS starts and ends
    // further out. Each of them scales with the stock map's edge in this direction, so the custom
    // range has the stock map's ESS shape everywhere, not only on the cardinals.
    static const EssEdges cardinal = stock_ess_edges(1, 0);
    const EssEdges here = stock_ess_edges(x / r, y / r);
    const double start_scale = here.start / cardinal.start, end_scale = here.end / cardinal.end;
    const double S = stock_ess_start(game) * start_scale, E = stock_ess_end(game) * end_scale;
    const double range_start = start * start_scale;
    const double range_end = std::min(end * end_scale, FULL_TILT - 1);

    // Piecewise linear: [0, start] -> [0, S], [start, end] -> [S, E], [end, 105] -> [E, 105].
    double out[2];
    if (r < range_start) {
        // The same scale in every direction, so the software dead zone (a square) keeps its shape.
        // Each axis stays just under the cardinal anchor, the edge of the game's dead zone (also a
        // square), so nothing below start rounds up into the range.
        const double edge = stock_ess_start(game) - 0.501;
        for (int i = 0; i < 2; ++i) {
            const double value = (i == 0 ? x : y) * S / range_start;
            out[i] = std::clamp(value, -edge, edge);
        }
    } else {
        double mapped;
        // With a set full speed distance, the stock map's full speed edge is one more anchor:
        // [end, full speed edge] walks then runs, split the way the stock map splits it, and past
        // the edge goes full speed. The edge sits half a unit before the first full speed distance,
        // like the stock edge it lands on, and scales with it in other directions.
        const RunEdges stock = stock_run_edges(game, x, y);
        const double R = stock.run, F = stock.full;
        const double full_edge = full_speed == AUTO_FULL_SPEED
                                     ? FULL_TILT
                                     : std::clamp((full_speed - 0.5) * F / stock_run_edges(game, 1, 0).full, range_end + 2, FULL_TILT - 1);
        const double run_edge = range_end + (R - E) * (full_edge - range_end) / (F - E);
        if (r <= range_end) {
            mapped = S + (r - range_start) * (E - S) / (range_end - range_start);
        } else if (full_speed == AUTO_FULL_SPEED) {
            // Stay at or over the value that rounds up past E, so nothing above end enters the range.
            mapped = std::max(E + (r - range_end) * (FULL_TILT - E) / (FULL_TILT - range_end), E + 0.5);
        } else if (r <= full_edge) {
            mapped = std::max(E + (r - range_end) * (F - E) / (full_edge - range_end), E + 0.5);
        } else {
            mapped = F + (r - full_edge) * (FULL_TILT - F) / (FULL_TILT - full_edge);
        }
        out[0] = x * mapped / r;
        out[1] = y * mapped / r;

        if (full_speed != AUTO_FULL_SPEED && r > range_end) {
            // A set full speed distance stretches or squeezes the stock map past the end, so plain rounding
            // would scatter the stock map's rounded running and full speed edges into
            // noise. Instead, the zone here is the stock map's own, moved along the ray by as much
            // as this map moves that edge: one stick unit here is one there, so the edges keep the
            // stock map's stepped shape, as with the ESS range. Then the closest whole position the
            // stock map puts in that zone. Walking positions that round into ESS keep that: the ESS
            // end is drawn as before.
            auto stock_zone_moved = [&](double by) {
                const double scale = (r + by) / r;
                return stock_zone(game, static_cast<int>(std::lround(x * scale)), static_cast<int>(std::lround(y * scale)));
            };
            const OotZone wanted = r <= (run_edge + full_edge) / 2
                                       ? std::clamp(stock_zone_moved(R - run_edge), OotZone::Walk, OotZone::Run)
                                       : std::clamp(stock_zone_moved(F - full_edge), OotZone::Run, OotZone::FullSpeed);
            long chosen[2] = {std::lround(out[0]), std::lround(out[1])};
            const OotZone rounded = stock_zone(game, chosen[0], chosen[1]);
            if (rounded != wanted && !(wanted == OotZone::Walk && rounded < OotZone::Walk)) {
                double best = 2.0 * 2.0;  // look no further than this, squared
                const long fx = static_cast<long>(std::floor(out[0])), fy = static_cast<long>(std::floor(out[1]));
                for (long cx = fx - 1; cx <= fx + 2; ++cx) {
                    for (long cy = fy - 1; cy <= fy + 2; ++cy) {
                        const double dx = cx - out[0], dy = cy - out[1], d = dx * dx + dy * dy;
                        if (d < best && stock_zone(game, cx, cy) == wanted) {
                            best = d;
                            chosen[0] = cx;
                            chosen[1] = cy;
                        }
                    }
                }
            }
            out[0] = static_cast<double>(chosen[0]);
            out[1] = static_cast<double>(chosen[1]);
        }
    }
    stick[0] = static_cast<uint8_t>(std::clamp<long>(std::lround(out[0]), -128, 127) + 128);
    stick[1] = static_cast<uint8_t>(std::clamp<long>(std::lround(out[1]), -128, 127) + 128);
}
