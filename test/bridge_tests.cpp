//bridge_tests.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// 1. Checks that every stick byte we send reaches Dolphin's emulated GC pad as intended
//    (DSU client math from DualShockUDPClient.cpp, MapFloat from ControllerEmu.h and GCPadEmu.cpp).
// 2. Talks to DsuServer like Dolphin does and checks the pad data that comes back.

#include <winsock2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "dsu_server.hpp"
#include "ess.hpp"
#include "ess_range.hpp"
#include "notch.hpp"
#include "stick.hpp"

static int failures = 0;
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

// Physical cardinal distance -> in-game value, through the whole pipeline, all 4 directions.
static int in_game(int distance, int start, int end, int direction) {
    const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    uint8_t s[2] = {static_cast<uint8_t>(128 + dx[direction] * distance), static_cast<uint8_t>(128 + dy[direction] * distance)};
    StickSettings settings;
    settings.range_start = start;
    settings.range_end = end;
    process_stick(s, NotchCalibration{}, settings);
    // VC treats x and y the same way, so read whichever axis is in use.
    const int axis = dx[direction] != 0 ? 0 : 1;
    return std::abs(vc_in_game(s[axis] - 128));
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
    CHECK(physical_start(ESS_IN_GAME_START) == ESS_DEFAULT_START);
    CHECK(physical_end(ESS_IN_GAME_END) == ESS_DEFAULT_END);
    CHECK(physical_start(9) == physical_start(8));  // 8 is unreachable, so 8 and 9 start together

    // Software dead zone: nothing that would read 2 or less gets through, 3 and up is untouched.
    const int dead_zone = physical_dead_zone(2);
    CHECK(physical_dead_zone(0) == 0);
    for (int d = 0; d <= 40; ++d) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + d), 128};
        apply_dead_zone(s, dead_zone);
        if (stock_in_game(d) <= 2) {
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

    // Two-axis VC matches the one-axis version on the axes, and is symmetric.
    int both[2];
    vc_in_game(30, 0, both);
    CHECK(both[0] == vc_in_game(30) && both[1] == 0);
    vc_in_game(-60, 45, both);
    int mirrored[2];
    vc_in_game(60, -45, mirrored);
    CHECK(both[0] == -mirrored[0] && both[1] == -mirrored[1]);
}

// In-game value of the stock map (no ESS range remap) at a physical distance on a cardinal,
// worked out directly from the ESS map and VC, independent of physical_start/physical_end.
static int stock_reference(int distance) {
    uint8_t s[2] = {static_cast<uint8_t>(128 + distance), 128};
    invert_vc_gc(s);
    return std::abs(vc_in_game(s[0] - 128));
}

static void test_ess_range() {
    // Defaults change nothing anywhere on the stick.
    bool identity = true;
    for (int x = 0; x < 256; ++x) {
        for (int y = 0; y < 256; ++y) {
            uint8_t s[2] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y)};
            remap_ess_range(s, ESS_DEFAULT_START, ESS_DEFAULT_END);
            identity = identity && s[0] == x && s[1] == y;
        }
    }
    CHECK(identity);

    // A range of in-game a to b means: ESS starts where the stock map first reads a or more and
    // ends where it last reads b or less. In-game 0-7 is dead zone, 8-26 is ESS (OoT libu64/pad.c,
    // z_player.c). The expected distances come straight from the stock map.
    const int ranges[][2] = {{1, 20}, {5, 26}, {8, 26}, {9, 26}, {7, 32}, {8, 35}, {15, 45}, {20, max_in_game_end()}};
    for (const auto& range : ranges) {
        int first = -1, last = -1;
        for (int d = 0; d <= ESS_MAX_END; ++d) {
            const int stock = stock_reference(d);
            if (first < 0 && stock >= range[0]) {
                first = d;
            }
            if (stock <= range[1]) {
                last = d;
            }
        }
        const int start = physical_start(range[0]);
        const int end = physical_end(range[1]);
        CHECK(start == first);
        CHECK(end == last);
        for (int direction = 0; direction < 4; ++direction) {
            bool ok = true;
            for (int d = 0; d <= 105; ++d) {
                const int value = in_game(d, start, end, direction);
                if (d < first) {
                    ok = ok && value <= 7;
                } else if (d <= last) {
                    ok = ok && value >= 8 && value <= 26;
                } else {
                    ok = ok && value >= 27;
                }
            }
            if (!ok) {
                std::printf("range %d-%d direction %d\n", range[0], range[1], direction);
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
    auto pipeline = [](int x, int y, int start, int end, int dead_zone, int game[2]) {
        uint8_t s[2] = {static_cast<uint8_t>(128 + x), static_cast<uint8_t>(128 + y)};
        StickSettings settings;
        settings.range_start = start;
        settings.range_end = end;
        settings.dead_zone_distance = dead_zone;
        process_stick(s, NotchCalibration{}, settings);
        vc_in_game(s[0] - 128, s[1] - 128, game);
    };
    for (const int range_start : {3, 9, 15, 20}) {
        const int start = physical_start(range_start), end = physical_end(26);
        const int largest = physical_dead_zone(range_start - 1);
        bool ok = true;
        for (int x = -60; x <= 60; ++x) {
            for (int y = -60; y <= 60; ++y) {
                int game[2];
                pipeline(x, y, start, end, largest, game);
                const bool game_dead_zone = std::abs(game[0]) <= 7 && std::abs(game[1]) <= 7;
                ok = ok && (range_start == 3 || !game_dead_zone || (game[0] == 0 && game[1] == 0));
            }
        }
        int first_ess[2];
        pipeline(start, 0, start, end, largest, first_ess);  // the range start itself is still ESS
        ok = ok && first_ess[0] >= 8;
        if (!ok) {
            std::printf("largest dead zone wrong with range start %d\n", range_start);
        }
        CHECK(ok);
    }
    bool resting_ok = true;
    for (int x = -60; x <= 60; ++x) {
        for (int y = -60; y <= 60; ++y) {
            int game[2];
            pipeline(x, y, ESS_DEFAULT_START, ESS_DEFAULT_END, physical_dead_zone(2), game);
            const bool small = std::abs(game[0]) <= 2 && std::abs(game[1]) <= 2;
            resting_ok = resting_ok && (!small || (game[0] == 0 && game[1] == 0));
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
    server.publish(frame);
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
    server.publish(frame);
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

    closesocket(client);
    server.stop();
}

static void test_axes_monotonic() {
    // Speedrun rule: with one input axis held, moving the other axis's input one way must never move
    // that axis's in-game value the other way. Checked with the defaults, ESS range 9 to 40 and
    // ESS range 3 to 26, which the ESS map alone breaks, and with notches rotated 5 degrees.
    NotchCalibration rotated;
    const double turn = 5 * 3.14159265358979 / 180;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const double x = IDEAL_NOTCHES[i][0] * 0.95, y = IDEAL_NOTCHES[i][1] * 0.95;
        rotated.notch[i][0] = static_cast<int8_t>(std::lround(x * std::cos(turn) - y * std::sin(turn)));
        rotated.notch[i][1] = static_cast<int8_t>(std::lround(x * std::sin(turn) + y * std::cos(turn)));
    }
    rotated.valid = true;
    StickSettings defaults;
    defaults.dead_zone_distance = physical_dead_zone(DEAD_ZONE_DEFAULT);
    StickSettings wide = defaults;
    wide.range_end = physical_end(40);
    StickSettings low_start = defaults;
    low_start.range_start = physical_start(3);
    const std::pair<NotchCalibration, StickSettings> cases[] = {
        {NotchCalibration{}, defaults}, {NotchCalibration{}, wide}, {NotchCalibration{}, low_start}, {rotated, defaults}};
    for (const auto& [cal, settings] : cases) {
        bool ok = true;
        for (int axis = 0; axis < 2 && ok; ++axis) {
            for (int held = -105; held <= 105 && ok; ++held) {
                int previous = -128;
                for (int moved = -105; moved <= 105; ++moved) {
                    uint8_t s[2];
                    s[1 - axis] = static_cast<uint8_t>(128 + held);
                    s[axis] = static_cast<uint8_t>(128 + moved);
                    process_stick(s, cal, settings);
                    int game[2];
                    vc_in_game(s[0] - 128, s[1] - 128, game);
                    if (!cal.valid && game[axis] * moved < 0) {  // never the other way than the stick
                        std::printf("axis %d at %d (other held at %d) points the other way: %d\n", axis, moved, held, game[axis]);
                        ok = false;
                        break;
                    }
                    if (game[axis] < previous) {
                        std::printf("axis %d at %d (other held at %d) went back to %d after %d\n", axis, moved, held, game[axis], previous);
                        ok = false;
                        break;
                    }
                    previous = game[axis];
                }
            }
        }
        CHECK(ok);
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
    test_vc_and_conversion();
    test_ess_range();
    test_axes_monotonic();
    test_dsu_server();
    WSACleanup();
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
