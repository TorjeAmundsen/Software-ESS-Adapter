//bridge_tests.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// 1. Checks that every stick byte we send reaches Dolphin's emulated GC pad as intended
//    (DSU client math from DualShockUDPClient.cpp, MapFloat from ControllerEmu.h and GCPadEmu.cpp).
// 2. Checks the stick path for Wii VC and Ship of Harkinian: ESS range, dead zone, and the speedrun
//    rule that each axis's in-game value follows its input.
// 3. Talks to DsuServer like Dolphin does and checks the pad data that comes back.

#include <winsock2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>
#include <string>
#include <utility>

#include "delayed_sender.hpp"
#include "dsu_server.hpp"
#include "ess.hpp"
#include "ess_range.hpp"
#include "notch.hpp"
#include "soh.hpp"
#include "stick.hpp"

static int failures = 0;
static constexpr Game VC = Game::WiiVC;
static constexpr Game SOH = Game::SoH;
#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                         \
        }                                                       \
    } while (0)

// Dolphin AnalogInput<u8>: (value + offset) / range, inputs clamp at 0.
static double dolphin_axis(uint8_t v) {
    const double plus = std::max(0.0, (v - 128.0) / 127.0);
    const double minus = std::max(0.0, (v - 128.0) / -128.0);
    return plus - minus;
}

// Dolphin's GC stick: MapFloat<u8>(x, 128, 1) in GCPadEmu.cpp, so 1 at -1.0 and 255 at +1.0.
static uint8_t dolphin_map_float(double x) {
    if (x > 0) {
        return static_cast<uint8_t>(std::llround((255 - 128) * x + 128));
    }
    return static_cast<uint8_t>(std::llround((128 - 1) * x + 128));
}

static uint8_t through_dolphin(uint8_t sent) {
    return dolphin_map_float(dolphin_axis(sent));
}

static void test_dolphin_round_trip() {
    // Sent as is, bytes 0 to 64 arrive one too high, e.g. ESS full tilt left (64) as 65.
    CHECK(through_dolphin(64) == 65);
    CHECK(through_dolphin(65) == 65);
    // With the correction every byte from 1 to 255 arrives unchanged. 0 is out of reach.
    bool all_match = true;
    for (int v = 1; v < 256; ++v) {
        all_match = all_match && through_dolphin(to_dolphin_stick(static_cast<uint8_t>(v))) == v;
    }
    CHECK(all_match);
    CHECK(through_dolphin(to_dolphin_stick(0)) == 1);
}

static void test_ess_center() {
    uint8_t c[2] = {128, 128};
    invert_vc_gc(c);
    CHECK(c[0] == 128 && c[1] == 128);
}

static void test_make_calibration() {
    // Controller resting around (130, 125); neutral samples wobble by a unit or two.
    const uint8_t neutral_samples[NOTCH_COUNT][2] = {{130, 125}, {131, 125}, {129, 124}, {130, 126},
                                                     {130, 125}, {131, 124}, {130, 125}, {129, 126}};
    uint8_t samples[CALIBRATION_STEPS][2];
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        samples[2 * i][0] = static_cast<uint8_t>(130 + IDEAL_NOTCHES[i][0] * 9 / 10);
        samples[2 * i][1] = static_cast<uint8_t>(125 + IDEAL_NOTCHES[i][1] * 9 / 10);
        samples[2 * i + 1][0] = neutral_samples[i][0];
        samples[2 * i + 1][1] = neutral_samples[i][1];
    }
    const NotchCalibration cal = make_calibration(samples);
    CHECK(cal.valid);
    CHECK(cal.neutral[0] == 130 && cal.neutral[1] == 125);    // averages 130.0 and 125.0
    CHECK(cal.notch[0][0] == 0 && cal.notch[0][1] == 94);     // Up at 90%: 105 * 9 / 10
    CHECK(cal.notch[5][0] == -67 && cal.notch[5][1] == -67);  // Down-Left

    // Every neutral sample is kept, and anything between them counts as neutral.
    CHECK(cal.neutral_samples[2][0] == 129 && cal.neutral_samples[2][1] == 124);
    CHECK(cal.neutral_min[0] == 129 && cal.neutral_max[0] == 131);
    CHECK(cal.neutral_min[1] == 124 && cal.neutral_max[1] == 126);
    const uint8_t inside[2] = {131, 124}, left_of[2] = {128, 125}, above[2] = {130, 127};
    CHECK(in_neutral_range(cal, inside));
    CHECK(!in_neutral_range(cal, left_of));
    CHECK(!in_neutral_range(cal, above));
    CHECK(!in_neutral_range(NotchCalibration{}, inside));  // without calibrated notches there is no range
}

static void test_notch_sections() {
    // Each notch has to be within 22.5 degrees of its own direction and far enough from neutral.
    CHECK(notch_in_section(0, 0, 105));    // Up
    CHECK(notch_in_section(0, 40, 100));   // Up, 21.8 degrees toward Up-Right
    CHECK(!notch_in_section(0, 45, 100));  // 24.2 degrees: Up-Right's section
    CHECK(!notch_in_section(0, 0, -105));  // Down
    CHECK(!notch_in_section(0, 0, 40));    // too close to neutral
    CHECK(notch_in_section(4, 0, -105));   // Down
    CHECK(notch_in_section(6, -105, -5));  // Left, either side of the wraparound
    CHECK(notch_in_section(6, -105, 5));
    CHECK(notch_in_section(5, -60, -90));  // Down-Left, 11.3 degrees toward Down

    NotchCalibration cal;
    cal.valid = true;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        cal.notch[i][0] = static_cast<int8_t>(IDEAL_NOTCHES[i][0] * 9 / 10);
        cal.notch[i][1] = static_cast<int8_t>(IDEAL_NOTCHES[i][1] * 9 / 10);
    }
    CHECK(misplaced_notch(cal) == -1);
    // Up-Left and Up-Right swapped.
    std::swap(cal.notch[1][0], cal.notch[7][0]);
    CHECK(misplaced_notch(cal) == 1);
}

static void test_notch_warp_and_snap() {
    // Notches that are off: every notch at 90% of ideal distance and rotated 5 degrees.
    NotchCalibration cal;
    const double turn = 5 * 3.14159265358979 / 180;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const double x = IDEAL_NOTCHES[i][0] * 0.9, y = IDEAL_NOTCHES[i][1] * 0.9;
        cal.notch[i][0] = static_cast<int8_t>(std::lround(x * std::cos(turn) - y * std::sin(turn)));
        cal.notch[i][1] = static_cast<int8_t>(std::lround(x * std::sin(turn) + y * std::cos(turn)));
    }
    cal.valid = true;
    auto warp = [&](int x, int y) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        warp_to_ideal_notches(s, cal);
        return std::pair<int, int>(s[0] - 128, s[1] - 128);
    };
    auto close_to = [](std::pair<int, int> a, int x, int y) {
        return std::abs(a.first - x) <= 1 && std::abs(a.second - y) <= 1;
    };

    // Every calibrated notch lands exactly on its ideal notch, and the center stays put.
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        CHECK((warp(cal.notch[i][0], cal.notch[i][1]) == std::pair<int, int>(IDEAL_NOTCHES[i][0], IDEAL_NOTCHES[i][1])));
    }
    CHECK(warp(0, 0) == std::make_pair(0, 0));

    // Halfway to a notch, and halfway between two notches, land halfway between the ideal ones.
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const int j = (i + 1) % NOTCH_COUNT;
        CHECK(close_to(warp(cal.notch[i][0] / 2, cal.notch[i][1] / 2), IDEAL_NOTCHES[i][0] / 2, IDEAL_NOTCHES[i][1] / 2));
        const int mx = (cal.notch[i][0] + cal.notch[j][0]) / 2, my = (cal.notch[i][1] + cal.notch[j][1]) / 2;
        CHECK(close_to(warp(mx, my), (IDEAL_NOTCHES[i][0] + IDEAL_NOTCHES[j][0]) / 2, (IDEAL_NOTCHES[i][1] + IDEAL_NOTCHES[j][1]) / 2));
    }

    // Smooth the whole way: one step on the stick never moves the output by more than a few units.
    int biggest_jump = 0;
    for (int x = -110; x < 110; ++x) {
        for (int y = -110; y < 110; ++y) {
            const auto here = warp(x, y), right = warp(x + 1, y), up = warp(x, y + 1);
            biggest_jump = std::max({biggest_jump, std::abs(right.first - here.first), std::abs(right.second - here.second),
                                     std::abs(up.first - here.first), std::abs(up.second - here.second)});
        }
    }
    CHECK(biggest_jump <= 3);

    // Snapping works on the warped stick, against the ideal notches, and only with calibrated notches.
    auto snap = [&](int x, int y, int distance, bool valid = true) {
        NotchCalibration c = cal;
        c.valid = valid;
        uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        snap_to_notch(s, c, distance);
        return std::pair<int, int>(s[0] - 128, s[1] - 128);
    };
    CHECK(snap(0, 102, 3) == std::make_pair(0, 105));
    CHECK(snap(-73, -74, 3) == std::make_pair(-75, -75));
    CHECK(snap(0, 101, 3) == std::make_pair(0, 101));  // 4 away
    CHECK(snap(0, 102, 0) == std::make_pair(0, 102));  // snapping off
    CHECK(snap(0, 102, 3, false) == std::make_pair(0, 102));
    CHECK(snap(0, 0, 10) == std::make_pair(0, 0));

    // Without a calibration the warp does nothing.
    cal.valid = false;
    CHECK(warp(50, 20) == std::make_pair(50, 20));
}

static void test_notch_offsets() {
    // A target follows the calibrated notch rule: in its own section, and it fits in a stick byte.
    CHECK(notch_offset_allowed(3, 1, -6));
    CHECK(notch_offset_allowed(7, 0, -27));    // Up-Left at -75,48: 12.4 degrees toward Left
    CHECK(notch_offset_allowed(0, 43, 0));     // Up at 43,105: 22.3 degrees
    CHECK(!notch_offset_allowed(0, 44, 0));    // 22.7 degrees: Up-Right's section
    CHECK(!notch_offset_allowed(0, 0, -54));   // 0,51: too close to center
    CHECK(notch_offset_allowed(0, 0, 22));     // 0,127: the edge
    CHECK(!notch_offset_allowed(0, 0, 23));    // past it
    CHECK(!notch_offset_allowed(1, -75, -75));  // the center itself

    // Warp and snap aim at the target instead of the ideal notch.
    NotchCalibration cal;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        cal.notch[i][0] = static_cast<int8_t>(IDEAL_NOTCHES[i][0] * 9 / 10);
        cal.notch[i][1] = static_cast<int8_t>(IDEAL_NOTCHES[i][1] * 9 / 10);
    }
    cal.valid = true;
    cal.offset[3][0] = 1;
    cal.offset[3][1] = -6;
    int target[2];
    notch_target(cal, 3, target);
    CHECK(target[0] == 76 && target[1] == -81);
    uint8_t s[2] = {static_cast<uint8_t>(128 + cal.notch[3][0]), static_cast<uint8_t>(128 + cal.notch[3][1])};
    warp_to_ideal_notches(s, cal);
    CHECK(s[0] - 128 == 76 && s[1] - 128 == -81);
    uint8_t close[2] = {128 + 74, 128 - 80};
    snap_to_notch(close, cal, 3);
    CHECK(close[0] - 128 == 76 && close[1] - 128 == -81);
    uint8_t ideal[2] = {128 + 75, 128 - 75};  // the ideal notch is no longer a snap target
    snap_to_notch(ideal, cal, 3);
    CHECK(ideal[0] - 128 == 75 && ideal[1] - 128 == -75);

    // Holding that notch through the whole path: Down-Right 76,-81 goes in as 56,-60 with ESS on.
    StickSettings settings;
    uint8_t held[2] = {static_cast<uint8_t>(128 + cal.notch[3][0]), static_cast<uint8_t>(128 + cal.notch[3][1])};
    process_stick(held, cal, settings);
    int game[2];
    vc_in_game(held[0] - 128, held[1] - 128, game);
    CHECK(game[0] == 56 && game[1] == -60);
}

// The default stick settings for a game: its stock ESS range, which changes nothing.
static StickSettings stock_settings(Game game) {
    StickSettings settings;
    settings.game = game;
    settings.range_start = stock_ess_start(game);
    settings.range_end = stock_ess_end(game);
    return settings;
}

// Physical cardinal distance -> in-game value, through the whole pipeline, all 4 directions.
static int in_game(Game game, int distance, int start, int end, int direction) {
    const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    uint8_t s[2] = {static_cast<uint8_t>(128 + dx[direction] * distance), static_cast<uint8_t>(128 + dy[direction] * distance)};
    StickSettings settings = stock_settings(game);
    settings.range_start = start;
    settings.range_end = end;
    process_stick(s, NotchCalibration{}, settings);
    int game_pair[2];
    game_in_game(game, s[0], s[1], game_pair);
    const int axis = dx[direction] != 0 ? 0 : 1;
    return std::abs(game_pair[axis]);
}

static void test_vc_and_conversion() {
    // From the decomp team: VC's dead zone is GC 0-15, OoT's dead zone 16-22, ESS 23-36.
    CHECK(vc_in_game(15) == 0);
    CHECK(vc_in_game(22) <= 7 && vc_in_game(23) >= 8);
    CHECK(vc_in_game(36) <= 26 && vc_in_game(37) >= 27);
    // VC never produces 8, 13, 17, 22 or 25.
    bool skipped = true;
    for (int gc = 0; gc < 128; ++gc) {
        const int v = vc_in_game(gc);
        skipped = skipped && v != 8 && v != 13 && v != 17 && v != 22 && v != 25;
    }
    CHECK(skipped);

    // The default range, in-game 9 to 26 (8 is unreachable through VC), is the stock map's 11 to 34,
    // so the defaults change nothing.
    CHECK(default_ess_start(VC) == 9);
    CHECK(stock_ess_start(VC) == ESS_DEFAULT_START && stock_ess_end(VC) == ESS_DEFAULT_END);
    CHECK(physical_start(VC, default_ess_start(VC)) == ESS_DEFAULT_START);
    CHECK(physical_end(VC, ESS_IN_GAME_END) == ESS_DEFAULT_END);
    CHECK(physical_start(VC, 9) == physical_start(VC, 8));  // 8 is unreachable, so 8 and 9 start together
    CHECK(max_in_game_end(VC) == 45 && max_in_game_end(SOH) == 45);  // as on ElectroModder's adapter

    // Software dead zone: nothing that would read 2 or less gets through, 3 and up is untouched.
    const int dead_zone = physical_dead_zone(VC, 2);
    CHECK(physical_dead_zone(VC, 0) == 0);
    for (int d = 0; d <= 40; ++d) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + d), 128};
        apply_dead_zone(s, dead_zone);
        if (stock_in_game(VC, d) <= 2) {
            CHECK(s[0] == 128);
        } else {
            CHECK(s[0] == 128 + d);
        }
    }
    uint8_t resting[2] = {129, 129};  // 1, 1
    apply_dead_zone(resting, dead_zone);
    CHECK(resting[0] == 128 && resting[1] == 128);
    // It's a square, like the game's per-axis dead zone: (3, 3) reads (2, 2) and is caught, while
    // (4, 0) reads (3, 0) and gets through.
    uint8_t diagonal[2] = {131, 131};
    apply_dead_zone(diagonal, dead_zone);
    CHECK(diagonal[0] == 128 && diagonal[1] == 128);
    uint8_t past[2] = {132, 128};
    apply_dead_zone(past, dead_zone);
    CHECK(past[0] == 132 && past[1] == 128);

    // OoT zones: dead zone to 7 per axis, ESS while the rest is shorter than 20.
    CHECK(oot_zone(7, 7) == OotZone::DeadZone);
    CHECK(oot_zone(8, 0) == OotZone::Ess);
    CHECK(oot_zone(0, -26) == OotZone::Ess);
    CHECK(oot_zone(27, 0) == OotZone::Walk);
    CHECK(oot_zone(21, 21) == OotZone::Ess);   // 14, 14 after the dead zone: 19.8
    CHECK(oot_zone(22, 22) == OotZone::Walk);  // 15, 15: 21.2
    CHECK(oot_zone(50, 0) == OotZone::Walk);   // 43
    CHECK(oot_zone(51, 0) == OotZone::Run);    // 44: past 43.75
    CHECK(oot_zone(65, 0) == OotZone::Run);    // 58
    CHECK(oot_zone(66, 0) == OotZone::FullSpeed);  // 59: past about 58.6
    CHECK(oot_zone(80, -80) == OotZone::FullSpeed);
    CHECK(oot_magnitude(80, 80) == 60.0f);

    // Two-axis VC matches the one-axis version on the axes, and is symmetric.
    int both[2];
    vc_in_game(30, 0, both);
    CHECK(both[0] == vc_in_game(30) && both[1] == 0);
    vc_in_game(-60, 45, both);
    int mirrored[2];
    vc_in_game(60, -45, mirrored);
    CHECK(both[0] == -mirrored[0] && both[1] == -mirrored[1]);
}

static void test_soh() {
    // SoH reads a DSU byte as (byte - 128) / 127 of 85 on each side, clamped, and truncates.
    auto soh = [](int x, int y) {
        int game[2];
        soh_in_game(static_cast<uint8_t>(x), static_cast<uint8_t>(y), game);
        return std::make_pair(game[0], game[1]);
    };
    CHECK(soh(128, 128) == std::make_pair(0, 0));
    CHECK(soh(255, 128) == std::make_pair(85, 0));
    CHECK(soh(0, 128) == std::make_pair(-85, 0));
    CHECK(soh(1, 128) == std::make_pair(-85, 0));
    CHECK(soh(128, 255) == std::make_pair(0, 85));
    CHECK(soh(128 + 63, 128) == std::make_pair(42, 0));  // 63 * 85 / 127 = 42.2
    bool symmetric = true;
    for (int b = 1; b < 256; ++b) {
        symmetric = symmetric && soh(b, 128).first == -soh(256 - b, 128).first && soh(128, b).second == soh(b, 128).first;
    }
    CHECK(symmetric);

    // Every cardinal value up to 84 can be sent safely away from a truncation edge. 85 and the
    // octagon corner 69, 69 only come out exactly on one, and the N64's corner, 70, 70, is outside
    // SoH's octagon.
    for (int v = 0; v <= 84; ++v) {
        bool found = false;
        for (int b = 128; b < 256 && !found; ++b) {
            int game[2];
            double margin;
            soh_in_game(static_cast<uint8_t>(b), 128, game, &margin);
            found = game[0] == v && game[1] == 0 && margin > 0.001;
        }
        if (!found) {
            std::printf("SoH can't safely reach %d\n", v);
        }
        CHECK(found);
    }
    double best_corner_margin = -1;
    bool n64_corner = false;
    for (int x = 128; x < 256; ++x) {
        for (int y = 128; y < 256; ++y) {
            int game[2];
            double margin;
            soh_in_game(static_cast<uint8_t>(x), static_cast<uint8_t>(y), game, &margin);
            if (game[0] == 69 && game[1] == 69) {
                best_corner_margin = std::max(best_corner_margin, margin);
            }
            n64_corner = n64_corner || (game[0] == 70 && game[1] == 70);
        }
    }
    CHECK(best_corner_margin >= 0 && best_corner_margin < 1e-6);
    CHECK(!n64_corner);

    // The map's N64 stage: full tilt straight is 80, the Gamecube corner is the N64 corner 70, 70.
    auto n64 = [](int x, int y) {
        const uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        int game[2];
        n64_in_game(s, game);
        return std::make_pair(game[0], game[1]);
    };
    CHECK(n64(0, 0) == std::make_pair(0, 0));
    CHECK(n64(105, 0) == std::make_pair(80, 0));
    CHECK(n64(0, -105) == std::make_pair(0, -80));
    CHECK(n64(75, 75) == std::make_pair(70, 70));
    CHECK(n64(-75, 75) == std::make_pair(-70, 70));
    CHECK(n64(-40, 20) == std::make_pair(-n64(40, -20).first, -n64(40, -20).second));

    // On SoH, 8 can be reached, so the default range starts there and changes nothing.
    CHECK(default_ess_start(SOH) == OOT_ESS_START);
    CHECK(physical_start(SOH, OOT_ESS_START) == stock_ess_start(SOH));
    CHECK(physical_end(SOH, ESS_IN_GAME_END) == stock_ess_end(SOH));
    CHECK(physical_start(SOH, 9) > physical_start(SOH, 8));

    // Through the whole path with defaults, SoH reads what the map aims for: full tilt straight is
    // 80, and the Gamecube corner reads 68, 68 in every quadrant, just inside SoH's octagon.
    auto through = [](int x, int y) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        process_stick(s, NotchCalibration{}, stock_settings(SOH));
        int game[2];
        soh_in_game(s[0], s[1], game);
        return std::make_pair(game[0], game[1]);
    };
    CHECK(through(0, 0) == std::make_pair(0, 0));
    CHECK(through(105, 0) == std::make_pair(80, 0));
    CHECK(through(-105, 0) == std::make_pair(-80, 0));
    CHECK(through(0, 105) == std::make_pair(0, 80));
    CHECK(through(75, 75) == std::make_pair(68, 68));
    CHECK(through(-75, 75) == std::make_pair(-68, 68));
    CHECK(through(-75, -75) == std::make_pair(-68, -68));
    CHECK(through(75, -75) == std::make_pair(68, -68));
    bool aims = true;  // on the axes, every distance reads exactly the map's value
    for (int d = 0; d <= 105; ++d) {
        aims = aims && through(d, 0) == n64(d, 0) && through(0, -d) == n64(0, -d);
    }
    CHECK(aims);
}

// In-game value of the stock map (no ESS range remap) at a physical distance on a cardinal,
// worked out directly from the ESS map and the game, independent of physical_start/physical_end.
static int stock_reference(Game game, int distance) {
    uint8_t s[2] = {static_cast<uint8_t>(128 + distance), 128};
    int game_pair[2];
    if (game == Game::SoH) {
        n64_in_game(s, game_pair);
        return std::abs(game_pair[0]);
    }
    invert_vc_gc(s);
    return std::abs(vc_in_game(s[0] - 128));
}

static void test_ess_range(Game game) {
    const char* const game_name = game == Game::SoH ? "SoH" : "VC";
    // Defaults change nothing anywhere on the stick.
    bool identity = true;
    for (int x = 0; x < 256; ++x) {
        for (int y = 0; y < 256; ++y) {
            uint8_t s[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y)};
            remap_ess_range(game, s, stock_ess_start(game), stock_ess_end(game));
            identity = identity && s[0] == x && s[1] == y;
        }
    }
    CHECK(identity);

    // A range of in-game a to b means: ESS starts where the stock map first reads a or more and
    // ends where it last reads b or less. In-game 0-7 is dead zone, 8-26 is ESS (OoT libu64/pad.c,
    // z_player.c). The expected distances come straight from the stock map.
    const int ranges[][2] = {{1, 20}, {5, 26}, {8, 26}, {9, 26}, {7, 32}, {8, 35}, {15, 45}, {20, max_in_game_end(game)}};
    for (const auto& range : ranges) {
        int first = -1, last = -1;
        for (int d = 0; d <= ESS_MAX_END; ++d) {
            const int stock = stock_reference(game, d);
            if (first < 0 && stock >= range[0]) {
                first = d;
            }
            if (stock <= range[1]) {
                last = d;
            }
        }
        const int start = physical_start(game, range[0]);
        const int end = physical_end(game, range[1]);
        CHECK(start == first);
        CHECK(end == last);
        for (int direction = 0; direction < 4; ++direction) {
            bool ok = true;
            for (int d = 0; d <= 105; ++d) {
                const int value = in_game(game, d, start, end, direction);
                if (d < first) {
                    ok = ok && value <= 7;
                } else if (d <= last) {
                    ok = ok && value >= 8 && value <= 26;
                } else {
                    ok = ok && value >= 27;
                }
            }
            if (!ok) {
                std::printf("%s range %d-%d direction %d\n", game_name, range[0], range[1], direction);
            }
            CHECK(ok);
        }
    }

    // The dead zone runs after the ESS range and is scaled with it. Two properties for each range
    // start: at its largest (range start - 1) it zeroes everything that would read 1 to 7 in game
    // and none of ESS; and with the default range, N = 2 zeroes everything that reads 2 or less.
    // Range start 3 puts the dead zone's edge 4 stick units from center, where the ESS range's
    // rounding moves a stick in and out of the dead zone as one axis rises. Keeping each axis from
    // dropping (step 5 of process_stick) then lets a few spots at the edge read up to 7 in game,
    // which is still OoT's own dead zone, so it only checks that ESS is untouched there.
    auto pipeline = [game](int x, int y, int start, int end, int dead_zone, int game_pair[2]) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        StickSettings settings = stock_settings(game);
        settings.range_start = start;
        settings.range_end = end;
        settings.dead_zone_distance = dead_zone;
        process_stick(s, NotchCalibration{}, settings);
        game_in_game(game, s[0], s[1], game_pair);
    };
    for (const int range_start : {3, 9, 15, 20}) {
        const int start = physical_start(game, range_start), end = physical_end(game, 26);
        const int largest = physical_dead_zone(game, range_start - 1);
        bool ok = true;
        for (int x = -60; x <= 60; ++x) {
            for (int y = -60; y <= 60; ++y) {
                int game_pair[2];
                pipeline(x, y, start, end, largest, game_pair);
                const bool game_dead_zone = std::abs(game_pair[0]) <= 7 && std::abs(game_pair[1]) <= 7;
                ok = ok && (range_start == 3 || !game_dead_zone || (game_pair[0] == 0 && game_pair[1] == 0));
            }
        }
        int first_ess[2];
        pipeline(start, 0, start, end, largest, first_ess);  // the range start itself is still ESS
        ok = ok && first_ess[0] >= 8;
        if (!ok) {
            std::printf("%s: largest dead zone wrong with range start %d\n", game_name, range_start);
        }
        CHECK(ok);
    }
    // A custom range keeps the stock map's ESS shape: how far ESS reaches on a diagonal compared to
    // a cardinal stays the same as with the stock range, even for extreme ranges, to within two
    // diagonal steps (narrow ranges are only a few stick units wide, and VC skips some values) and
    // 5% (far out, VC's length limit bends the values away from the map's formula). A plain radial
    // remap is much further off: with range 3 to 7, ESS reached 15.6 on the diagonal where the stock
    // shape says 10.
    auto ess_reach = [&](int start, int end, bool diagonal) {
        double last = 0;
        for (int d = 1; d <= 100; ++d) {
            int game_pair[2];
            pipeline(d, diagonal ? d : 0, start, end, 0, game_pair);
            if (oot_zone(game_pair[0], game_pair[1]) == OotZone::Ess) {
                last = diagonal ? d * std::sqrt(2.0) : d;
            }
        }
        return last;
    };
    const double stock_ratio = ess_reach(stock_ess_start(game), stock_ess_end(game), true) /
                               ess_reach(stock_ess_start(game), stock_ess_end(game), false);
    for (const auto& range : {std::make_pair(3, 7), std::make_pair(5, 15), std::make_pair(9, 40), std::make_pair(15, 50)}) {
        const int start = physical_start(game, range.first), end = std::max(physical_end(game, range.second), start + 1);
        const double diagonal = ess_reach(start, end, true), expected = stock_ratio * ess_reach(start, end, false);
        const bool same_shape = std::fabs(diagonal - expected) <= 2 * std::sqrt(2.0) + 0.05 * expected;
        if (!same_shape) {
            std::printf("%s: ESS range %d-%d reaches %.1f on a diagonal, the stock shape says %.1f\n", game_name, range.first,
                        range.second, diagonal, expected);
        }
        CHECK(same_shape);
    }

    bool resting_ok = true;
    for (int x = -60; x <= 60; ++x) {
        for (int y = -60; y <= 60; ++y) {
            int game_pair[2];
            pipeline(x, y, stock_ess_start(game), stock_ess_end(game), physical_dead_zone(game, 2), game_pair);
            const bool small = std::abs(game_pair[0]) <= 2 && std::abs(game_pair[1]) <= 2;
            resting_ok = resting_ok && (!small || (game_pair[0] == 0 && game_pair[1] == 0));
        }
    }
    CHECK(resting_ok);
}

static uint32_t reference_crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static void make_request(uint8_t* buf, int size, uint32_t type) {
    std::memset(buf, 0, size);
    std::memcpy(buf, "DSUC", 4);
    const uint16_t version = 1001, length = static_cast<uint16_t>(size - 16);
    std::memcpy(buf + 4, &version, 2);
    std::memcpy(buf + 6, &length, 2);
    std::memcpy(buf + 16, &type, 4);
}

static int receive(SOCKET s, uint8_t* buf, int size) {
    return recv(s, reinterpret_cast<char*>(buf), size, 0);
}

static bool crc_ok(uint8_t* buf, int size) {
    uint32_t crc;
    std::memcpy(&crc, buf + 8, 4);
    std::memset(buf + 8, 0, 4);
    const bool ok = crc == reference_crc32(buf, size);
    std::memcpy(buf + 8, &crc, 4);
    return ok;
}

static void test_dsu_server() {
    const uint16_t port = 26799;
    DsuServer server;
    CHECK(server.start(port));

    SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD timeout_ms = 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    AdapterFrame frame{};
    frame[1].connected = true;
    frame[1].buttons1 = 0x01;  // A
    frame[1].buttons2 = 0x02;  // Z
    frame[1].stick[0] = 200;
    frame[1].stick[1] = 17;
    frame[1].trigger[0] = 99;
    server.publish(frame, VC);
    CHECK(!server.is_transmitting());  // nobody has subscribed yet

    // ListPorts for pads 0 and 1: pad 0 disconnected, pad 1 connected.
    uint8_t req[28];
    make_request(req, sizeof(req), 0x100001);
    const int32_t count = 2;
    std::memcpy(req + 20, &count, 4);
    req[24] = 0;
    req[25] = 1;
    send(client, reinterpret_cast<char*>(req), sizeof(req), 0);

    uint8_t buf[128];
    for (int i = 0; i < 2; ++i) {
        const int n = receive(client, buf, sizeof(buf));
        CHECK(n == 32);
        if (n != 32) {
            continue;
        }
        CHECK(crc_ok(buf, n));
        CHECK(std::memcmp(buf, "DSUS", 4) == 0);
        CHECK(buf[21] == (buf[20] == 1 ? 2 : 0));  // state
    }

    // Subscribe to pad 1 and publish.
    make_request(req, 28, 0x100002);
    req[20] = 1;  // by pad id
    req[21] = 1;
    send(client, reinterpret_cast<char*>(req), 28, 0);
    Sleep(100);  // let the server thread register us
    CHECK(server.client_count() == 1);
    server.publish(frame, VC);
    CHECK(server.is_transmitting());

    const int n = receive(client, buf, sizeof(buf));
    CHECK(n == 100);
    if (n == 100) {
        CHECK(crc_ok(buf, n));
        CHECK(buf[20] == 1);                     // pad id
        CHECK(buf[37] & 0x40);                   // Cross bit (A)
        CHECK(buf[37] & 0x08);                   // R1 bit (Z)
        CHECK(buf[40] == 200 && buf[41] == 16);  // 17 is sent as 16, see to_dolphin_stick
        CHECK(buf[49] == 255);                   // cross analog
        CHECK(buf[52] == 255);                   // r1 analog
        CHECK(buf[55] == 99);                    // l2 analog
    }
    server.publish(frame, SOH);
    if (receive(client, buf, sizeof(buf)) == 100) {
        CHECK(buf[40] == 200 && buf[41] == 17);  // SoH reads bytes as they are
    } else {
        CHECK(!"no pad data for SoH");
    }

    closesocket(client);
    server.stop();
}

// Speedrun rule: with one input axis held, moving the other axis's input one way must never move
// that axis's in-game value the other way, so that dead zone, ESS, walking and running stay in that
// order. Checks both: each axis, and the zones outward from the middle of every line (full speed
// counts as running). Sweeps every raw stick position (0 to 255 on both
// axes, past the gate too) and returns how many steps break that, printing the first. The center
// has to read 0, 0, and without calibration no axis may point the other way from the stick.
static int monotonic_breaks(const char* name, const NotchCalibration& cal, const StickSettings& settings) {
    auto game_at = [&](int x, int y, int game[2]) {
        uint8_t s[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y)};
        process_stick(s, cal, settings);
        game_in_game(settings.game, s[0], s[1], game);
    };
    int breaks = 0;
    auto report = [&](const char* what, int axis, int moved, int held, int value, int previous) {
        if (breaks++ == 0) {
            std::printf("%s, %s: axis %d at %d (other held at %d) %s %d (before: %d)\n", settings.game == Game::SoH ? "SoH" : "VC", name,
                        axis, moved - 128, held - 128, what, value, previous);
        }
    };
    int center[2];
    game_at(128, 128, center);
    if (center[0] != 0 || center[1] != 0) {
        report("center reads", 0, 128, 128, center[0], center[1]);
    }
    for (int axis = 0; axis < 2; ++axis) {
        for (int held = 0; held < 256; ++held) {
            int previous = -128;
            int zone[256];
            for (int moved = 0; moved < 256; ++moved) {
                int game[2];
                game_at(axis == 0 ? moved : held, axis == 0 ? held : moved, game);
                if (game[axis] < previous) {
                    report("went back to", axis, moved, held, game[axis], previous);
                }
                if (!cal.valid && game[axis] * (moved - 128) < 0) {
                    report("points the other way:", axis, moved, held, game[axis], previous);
                }
                previous = game[axis];
                zone[moved] = std::min(static_cast<int>(oot_zone(game[0], game[1])), static_cast<int>(OotZone::Run));
            }
            for (int moved = 129; moved < 256; ++moved) {
                if (zone[moved] < zone[moved - 1]) {
                    report("fell back to zone", axis, moved, held, zone[moved], zone[moved - 1]);
                }
            }
            for (int moved = 127; moved >= 0; --moved) {
                if (zone[moved] < zone[moved + 1]) {
                    report("fell back to zone", axis, moved, held, zone[moved], zone[moved + 1]);
                }
            }
        }
    }
    return breaks;
}

static NotchCalibration turned_notches(double degrees, double scale) {
    NotchCalibration cal;
    const double turn = degrees * 3.14159265358979 / 180;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const double x = IDEAL_NOTCHES[i][0] * scale, y = IDEAL_NOTCHES[i][1] * scale;
        cal.notch[i][0] = static_cast<int8_t>(std::lround(x * std::cos(turn) - y * std::sin(turn)));
        cal.notch[i][1] = static_cast<int8_t>(std::lround(x * std::sin(turn) + y * std::cos(turn)));
    }
    cal.valid = true;
    return cal;
}

static void test_axes_monotonic(Game game) {
    // Fixed cases: the defaults, ESS range 9 to 40 and 3 to 26, which the ESS map alone breaks on
    // VC, notches turned 5 degrees, and notch offsets with snapping, ESS on and off. With ESS off,
    // snapping alone breaks it through VC's length limit, and big offsets in hundreds of places.
    const NotchCalibration turned = turned_notches(5, 0.95);
    StickSettings defaults = stock_settings(game);
    defaults.dead_zone_distance = physical_dead_zone(game, DEAD_ZONE_DEFAULT);
    StickSettings wide = defaults;
    wide.range_end = physical_end(game, 40);
    StickSettings low_start = defaults;
    low_start.range_start = physical_start(game, 3);
    // A narrow range: past its end the held axis shrinks enough to dip from walking back into ESS.
    StickSettings narrow = defaults;
    narrow.range_start = physical_start(game, 5);
    narrow.range_end = physical_end(game, 13);
    // Targets turned 20 degrees, alternately toward each other, at 60% to 115% of the ideal
    // distance: neighbors end up 5 degrees apart.
    NotchCalibration offsets = turned;
    const NotchCalibration targets[2] = {turned_notches(20, 0.6), turned_notches(-20, 1.15)};
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const NotchCalibration& t = targets[i % 2];
        offsets.offset[i][0] = static_cast<int8_t>(t.notch[i][0] - IDEAL_NOTCHES[i][0]);
        offsets.offset[i][1] = static_cast<int8_t>(t.notch[i][1] - IDEAL_NOTCHES[i][1]);
        CHECK(notch_offset_allowed(i, offsets.offset[i][0], offsets.offset[i][1]));
    }
    StickSettings snapping = defaults;
    snapping.snap_distance = MAX_SNAP_DISTANCE;
    StickSettings ess_off = snapping;
    ess_off.ess = false;
    // Offsets where the table used to get boxed in near VC's length limit: x had to reach -77 and
    // y stay at 41, which VC can't do at once, so Stick -93,38 to -93,39 read y 41 then 40.
    NotchCalibration boxed_in = turned_notches(0, 1);
    const int boxed_in_offsets[NOTCH_COUNT][2] = {{6, -12}, {-2, 2}, {-11, -9}, {-5, -15}, {3, -6}, {14, -14}, {15, 15}, {-8, 2}};
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        boxed_in.offset[i][0] = static_cast<int8_t>(boxed_in_offsets[i][0]);
        boxed_in.offset[i][1] = static_cast<int8_t>(boxed_in_offsets[i][1]);
    }
    StickSettings boxed_in_settings = stock_settings(game);
    boxed_in_settings.snap_distance = 1;
    boxed_in_settings.range_start = physical_start(game, 5);
    boxed_in_settings.range_end = physical_end(game, 35);
    boxed_in_settings.dead_zone_distance = physical_dead_zone(game, 4);

    const struct {
        const char* name;
        NotchCalibration cal;
        StickSettings settings;
    } cases[] = {{"defaults", NotchCalibration{}, defaults},
                 {"ESS range 9 to 40", NotchCalibration{}, wide},
                 {"ESS range 3 to 26", NotchCalibration{}, low_start},
                 {"ESS range 5 to 13", NotchCalibration{}, narrow},
                 {"turned notches", turned, defaults},
                 {"turned notches, ESS off, snap 5", turned, ess_off},
                 {"big offsets, snap 5", offsets, snapping},
                 {"big offsets, ESS off, snap 5", offsets, ess_off},
                 {"boxed in", boxed_in, boxed_in_settings}};
    for (const auto& c : cases) {
        CHECK(monotonic_breaks(c.name, c.cal, c.settings) == 0);
    }

    // Random cases from a fixed seed: calibrations turned, scaled and jittered, targets anywhere in
    // their sections, ESS on and off, any snap distance, ESS range and dead zone.
    unsigned seed = game == Game::SoH ? 54321 : 12345;
    auto random = [&](int low, int high) {
        seed = seed * 1103515245 + 12345;
        return low + static_cast<int>((seed >> 8) % static_cast<unsigned>(high - low + 1));
    };
    for (int n = 0; n < 150; ++n) {
        NotchCalibration cal;
        if (random(0, 5) > 0) {
            cal = turned_notches(random(-15, 15), random(80, 110) / 100.0);
            const int most = random(0, 1) ? 60 : random(0, 30);
            for (int i = 0; i < NOTCH_COUNT; ++i) {
                int x, y;
                do {
                    x = cal.notch[i][0] + random(-6, 6);
                    y = cal.notch[i][1] + random(-6, 6);
                } while (!notch_in_section(i, x, y));
                cal.notch[i][0] = static_cast<int8_t>(x);
                cal.notch[i][1] = static_cast<int8_t>(y);
                int dx, dy;
                do {
                    dx = random(-most, most);
                    dy = random(-most, most);
                } while (!notch_offset_allowed(i, dx, dy));
                cal.offset[i][0] = static_cast<int8_t>(dx);
                cal.offset[i][1] = static_cast<int8_t>(dy);
            }
        }
        StickSettings settings = stock_settings(game);
        settings.ess = random(0, 3) > 0;
        settings.snap_distance = random(0, MAX_SNAP_DISTANCE);
        const int start = random(1, 15), end = random(start + 1, std::min(start + 45, max_in_game_end(game)));
        settings.range_start = physical_start(game, start);
        settings.range_end = std::max(physical_end(game, end), settings.range_start + 1);
        settings.dead_zone_distance = physical_dead_zone(game, random(0, start - 1));
        const std::string name = "random case " + std::to_string(n);
        CHECK(monotonic_breaks(name.c_str(), cal, settings) == 0);
    }

    // With ESS off and no calibration on VC, the pass changes nothing: only the dead zone applies.
    // (SoH's octagon can pull an axis back, so there the pass can change things.)
    if (game == Game::SoH) {
        return;
    }
    for (const int dead_zone : {0, 2, 6}) {
        StickSettings plain;
        plain.ess = false;
        plain.dead_zone_distance = physical_dead_zone(VC, dead_zone);
        bool same = true;
        for (int x = 0; x < 256; ++x) {
            for (int y = 0; y < 256; ++y) {
                uint8_t s[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y)}, expected[2] = {s[0], s[1]};
                process_stick(s, NotchCalibration{}, plain);
                apply_dead_zone(expected, plain.dead_zone_distance);
                same = same && s[0] == expected[0] && s[1] == expected[1];
            }
        }
        CHECK(same);
    }
}

static void test_delayed_sender() {
    using namespace std::chrono;
    struct Sent {
        steady_clock::time_point at;
        int id;
        Game game;
    };
    std::mutex mutex;
    std::vector<Sent> sent;
    auto frame_with_id = [](int id) {
        AdapterFrame frame{};
        frame[0].stick[0] = static_cast<uint8_t>(id);
        return frame;
    };
    {
        DelayedSender sender([&](const AdapterFrame& frame, Game game) {
            std::lock_guard lock(mutex);
            sent.push_back({steady_clock::now(), frame[0].stick[0], game});
        });

        // No lag: sent right away, on the calling thread.
        sender.push(frame_with_id(1), VC, microseconds(0));
        CHECK(sent.size() == 1 && sent[0].id == 1 && sent[0].game == VC);

        // 30 ms: every frame goes out 30 ms after it came in, in order. A few milliseconds late is
        // fine (the thread has to wake up), early never.
        std::vector<steady_clock::time_point> pushed;
        for (int id = 2; id < 12; ++id) {
            pushed.push_back(steady_clock::now());
            sender.push(frame_with_id(id), SOH, milliseconds(30));
            Sleep(4);
        }
        Sleep(80);
        {
            std::lock_guard lock(mutex);
            CHECK(sent.size() == 11);
            long long worst_late_us = 0;
            bool on_time = sent.size() == 11;
            for (size_t i = 1; i < sent.size() && on_time; ++i) {
                const auto late = duration_cast<microseconds>(sent[i].at - (pushed[i - 1] + milliseconds(30))).count();
                on_time = sent[i].id == static_cast<int>(i) + 1 && sent[i].game == SOH && late >= 0;
                worst_late_us = std::max(worst_late_us, late);
            }
            std::printf("input lag: worst %lld us late\n", worst_late_us);
            CHECK(on_time);
            CHECK(worst_late_us < 5000);
        }

        // Changing the lag drops what is still waiting, so nothing goes out of order: frame 12
        // waits 50 ms, then frame 13 with no lag goes out at once and 12 never does.
        sender.push(frame_with_id(12), SOH, milliseconds(50));
        sender.push(frame_with_id(13), SOH, microseconds(0));
        Sleep(80);
        std::lock_guard lock(mutex);
        CHECK(sent.size() == 12 && sent.back().id == 13);
    }
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    test_dolphin_round_trip();
    test_ess_center();
    test_make_calibration();
    test_notch_sections();
    test_notch_warp_and_snap();
    test_notch_offsets();
    test_vc_and_conversion();
    test_soh();
    for (const Game game : {VC, SOH}) {
        test_ess_range(game);
        test_axes_monotonic(game);
    }
    test_dsu_server();
    test_delayed_sender();
    WSACleanup();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
