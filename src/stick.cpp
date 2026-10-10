//stick.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "stick.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <limits>
#include <vector>

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

namespace {

// In-game values run from -127 to 127.
constexpr int GAME_SPAN = 255;
constexpr int game_index(int x, int y) {
    return (x + 127) * GAME_SPAN + (y + 127);
}

// Every in-game pair Wii VC can produce, with a stick byte pair that produces it. Byte 0 is left
// out because Dolphin can't receive it (see to_dolphin_stick).
struct Reachable {
    std::vector<int> sent;  // by game_index: sent x << 8 | sent y, or -1 if VC can't produce it
    std::vector<std::array<int, 2>> pairs;

    Reachable() : sent(GAME_SPAN * GAME_SPAN, -1) {
        auto distance = [](int x, int y) {
            return std::abs(x - 128) + std::abs(y - 128);
        };
        for (int x = 1; x <= 255; ++x) {
            for (int y = 1; y <= 255; ++y) {
                int game[2];
                vc_in_game(x - 128, y - 128, game);
                int& slot = sent[game_index(game[0], game[1])];
                if (slot < 0) {
                    pairs.push_back({game[0], game[1]});
                }
                // Of the sticks that give the same pair, keep the one closest to center.
                if (slot < 0 || distance(x, y) < distance(slot >> 8, slot & 0xFF)) {
                    slot = x << 8 | y;
                }
            }
        }
    }
};

const Reachable& reachable() {
    static const Reachable instance;
    return instance;
}

// Step 5's result for every stick position: the two bytes to send, at 2 * (x * 256 + y).
struct Table {
    std::vector<int> key;
    std::vector<uint8_t> sent;
};

std::vector<int> table_key(const NotchCalibration& notches, const StickSettings& settings) {
    // The ESS range only matters with ESS on, so with ESS off changing it doesn't build a new table.
    const int range_start = settings.ess ? settings.range_start : 0, range_end = settings.ess ? settings.range_end : 0;
    std::vector<int> key = {settings.ess, notches.valid, settings.snap_distance, range_start, range_end, settings.dead_zone_distance};
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        key.insert(key.end(), {notches.notch[i][0], notches.notch[i][1], notches.offset[i][0], notches.offset[i][1]});
    }
    return key;
}

// The in-game values a position may take, per axis: lo[axis] to hi[axis].
struct Box {
    int lo[2] = {-127, -127};
    int hi[2] = {127, 127};
};

// How far a value is outside [lo, hi].
int outside(int value, int lo, int hi) {
    return std::max({0, lo - value, value - hi});
}

// The in-game pair closest to `wanted` inside the box. If VC can't produce any such pair, the one
// that is least far outside.
std::array<int, 2> closest_allowed(const std::array<int, 2>& wanted, const Box& box) {
    const Reachable& r = reachable();
    auto short_by = [&](int x, int y) {
        return outside(x, box.lo[0], box.hi[0]) + outside(y, box.lo[1], box.hi[1]);
    };
    std::array<int, 2> best = wanted;
    long best_score = std::numeric_limits<long>::max();
    auto consider = [&](int x, int y) {
        const long dx = x - wanted[0], dy = y - wanted[1];
        const long score = short_by(x, y) * 1000000L + dx * dx + dy * dy;
        if (score < best_score) {
            best_score = score;
            best = {x, y};
        }
    };
    // Nearby first, the whole set only if nothing nearby works.
    constexpr int NEAR = 12;
    for (int x = std::max(-127, wanted[0] - NEAR); x <= std::min(127, wanted[0] + NEAR); ++x) {
        for (int y = std::max(-127, wanted[1] - NEAR); y <= std::min(127, wanted[1] + NEAR); ++y) {
            if (r.sent[game_index(x, y)] >= 0) {
                consider(x, y);
            }
        }
    }
    if (short_by(best[0], best[1]) > 0) {
        for (const auto& pair : r.pairs) {
            consider(pair[0], pair[1]);
        }
    }
    return best;
}

Table build_table(const NotchCalibration& notches, const StickSettings& settings) {
    Table table;
    table.key = table_key(notches, settings);
    table.sent.resize(256 * 256 * 2);
    const Reachable& r = reachable();

    // Steps 1 to 4 for every stick position: the bytes they send and what VC turns those into.
    std::vector<std::array<uint8_t, 2>> steps(256 * 256);
    std::vector<std::array<int, 2>> baseline(256 * 256);
    for (int index = 0; index < 256 * 256; ++index) {
        uint8_t stick[2] = {static_cast<uint8_t>(index >> 8), static_cast<uint8_t>(index & 0xFF)};
        run_steps(stick, notches, settings, nullptr);
        steps[index] = {stick[0], stick[1]};
        vc_in_game(stick[0] - 128, stick[1] - 128, baseline[index].data());
    }

    std::vector<std::array<int, 2>> game(256 * 256);  // the in-game pair chosen for each stick position
    std::vector<Box> bounds(256 * 256);               // extra limits, see below

    // Each quadrant from center outward, row by row, so the position one step closer to center on
    // each axis is always done first. Moving outward, an axis's in-game value may not move back the
    // other way. Positions on the axes belong to two quadrants and come out the same in both, since
    // they only depend on positions further in on the same axis and on their own bounds. Together
    // the quadrants make each axis's in-game value follow its input in the same direction across
    // the whole stick.
    //
    // A position can be boxed in: x has to reach as far out as its neighbor further in on x, y as
    // far out as its neighbor further in on y, and VC's length limit can't do both at once. Then
    // the positions further in on the axis that fell short get bounded to the value it reached,
    // and the table is worked out again, until nothing falls short.
    constexpr int MAX_PASSES = 64;
    for (int pass = 0; pass < MAX_PASSES; ++pass) {
        bool boxed_in = false;
        for (const int sx : {1, -1}) {
            for (const int sy : {1, -1}) {
                for (int j = 0; j <= (sy > 0 ? 127 : 128); ++j) {
                    for (int i = 0; i <= (sx > 0 ? 127 : 128); ++i) {
                        const int bx = 128 + sx * i, by = 128 + sy * j;
                        const int index = bx * 256 + by;
                        const int outward[2] = {sx, sy}, inner[2] = {i, j};
                        const int step[2] = {sx * 256, sy};  // one position outward, in index units

                        Box box = bounds[index];
                        for (int axis = 0; axis < 2; ++axis) {
                            if (inner[axis] > 0) {
                                const int limit = game[index - step[axis]][axis];
                                if (outward[axis] > 0) {
                                    box.lo[axis] = std::max(box.lo[axis], limit);
                                } else {
                                    box.hi[axis] = std::min(box.hi[axis], limit);
                                }
                            }
                        }
                        const std::array<int, 2>& normal = baseline[index];
                        if (outside(normal[0], box.lo[0], box.hi[0]) == 0 && outside(normal[1], box.lo[1], box.hi[1]) == 0) {
                            // Nothing moves back: send the normal result unchanged.
                            game[index] = normal;
                            table.sent[2 * index] = steps[index][0];
                            table.sent[2 * index + 1] = steps[index][1];
                            continue;
                        }
                        game[index] = closest_allowed(normal, box);
                        const int sent = r.sent[game_index(game[index][0], game[index][1])];
                        table.sent[2 * index] = static_cast<uint8_t>(sent >> 8);
                        table.sent[2 * index + 1] = static_cast<uint8_t>(sent & 0xFF);
                        for (int axis = 0; axis < 2; ++axis) {
                            const int value = game[index][axis];
                            if (outside(value, box.lo[axis], box.hi[axis]) == 0 || inner[axis] == 0) {
                                continue;
                            }
                            boxed_in = true;
                            for (int k = 1; k <= inner[axis]; ++k) {
                                Box& b = bounds[index - k * step[axis]];
                                if (outward[axis] > 0) {
                                    b.hi[axis] = std::min(b.hi[axis], value);
                                } else {
                                    b.lo[axis] = std::max(b.lo[axis], value);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!boxed_in) {
            break;
        }
    }
    return table;
}

}  // namespace

void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2]) {
    const uint8_t in[2] = {stick[0], stick[1]};
    run_steps(stick, notches, settings, calibrated);
    // One table per settings and calibration in use, built on first use. Building takes a moment,
    // so changing a setting or calibrating costs that once.
    thread_local std::deque<Table> tables;
    const std::vector<int> key = table_key(notches, settings);
    const Table* table = nullptr;
    for (const Table& t : tables) {
        if (t.key == key) {
            table = &t;
        }
    }
    if (!table) {
        if (tables.size() >= 8) {
            tables.pop_front();
        }
        tables.push_back(build_table(notches, settings));
        table = &tables.back();
    }
    const int index = in[0] * 256 + in[1];
    stick[0] = table->sent[2 * index];
    stick[1] = table->sent[2 * index + 1];
}
