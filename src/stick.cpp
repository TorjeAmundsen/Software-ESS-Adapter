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
    std::vector<int> key = {notches.valid, settings.snap_distance, settings.range_start, settings.range_end, settings.dead_zone_distance};
    for (const auto& notch : notches.notch) {
        key.push_back(notch[0]);
        key.push_back(notch[1]);
    }
    return key;
}

// How far an in-game value falls short of its limit. `outward` is the direction the stick moved
// to get here on that axis (1 or -1), and the value may not be behind `limit` in that direction.
// 0 means no limit, for positions on the axis itself.
int shortfall(int value, int outward, int limit) {
    return outward == 0 ? 0 : std::max(0, outward * (limit - value));
}

// The in-game pair closest to `wanted` that is within both axes' limits. If VC can't produce any
// such pair, the one that falls least short.
std::array<int, 2> closest_allowed(const std::array<int, 2>& wanted, const int outward[2], const int limit[2]) {
    const Reachable& r = reachable();
    auto short_by = [&](int x, int y) {
        return shortfall(x, outward[0], limit[0]) + shortfall(y, outward[1], limit[1]);
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
    std::vector<std::array<int, 2>> game(256 * 256);  // the in-game pair chosen for each stick position

    // Each quadrant from center outward, row by row, so the position one step closer to center on
    // each axis is always done first. Moving outward, an axis's in-game value may not move back the
    // other way. Positions on the axes belong to two quadrants and come out the same in both, since
    // they only depend on positions further in on the same axis. Together the quadrants make each
    // axis's in-game value follow its input in the same direction across the whole stick.
    for (const int sx : {1, -1}) {
        for (const int sy : {1, -1}) {
            for (int j = 0; j <= (sy > 0 ? 127 : 128); ++j) {
                for (int i = 0; i <= (sx > 0 ? 127 : 128); ++i) {
                    const int bx = 128 + sx * i, by = 128 + sy * j;
                    const int index = bx * 256 + by;
                    uint8_t stick[2] = {static_cast<uint8_t>(bx), static_cast<uint8_t>(by)};
                    run_steps(stick, notches, settings, nullptr);
                    int baseline[2];
                    vc_in_game(stick[0] - 128, stick[1] - 128, baseline);

                    const int outward[2] = {i > 0 ? sx : 0, j > 0 ? sy : 0};
                    const int limit[2] = {i > 0 ? game[index - sx * 256][0] : 0, j > 0 ? game[index - sy][1] : 0};
                    if (shortfall(baseline[0], outward[0], limit[0]) == 0 && shortfall(baseline[1], outward[1], limit[1]) == 0) {
                        // Neither axis moves back: send the normal result unchanged.
                        game[index] = {baseline[0], baseline[1]};
                        table.sent[2 * index] = stick[0];
                        table.sent[2 * index + 1] = stick[1];
                    } else {
                        game[index] = closest_allowed({baseline[0], baseline[1]}, outward, limit);
                        const int sent = r.sent[game_index(game[index][0], game[index][1])];
                        table.sent[2 * index] = static_cast<uint8_t>(sent >> 8);
                        table.sent[2 * index + 1] = static_cast<uint8_t>(sent & 0xFF);
                    }
                }
            }
        }
    }
    return table;
}

}  // namespace

void process_stick(uint8_t stick[2], const NotchCalibration& notches, const StickSettings& settings, uint8_t calibrated[2]) {
    const uint8_t in[2] = {stick[0], stick[1]};
    run_steps(stick, notches, settings, calibrated);
    if (!settings.ess) {
        return;
    }
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
