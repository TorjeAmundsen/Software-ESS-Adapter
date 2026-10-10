//main.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Software ESS Adapter: reads a Gamecube adapter, applies the ESS map from this
// repository, and serves the result to Dolphin as a DSU controller.

#include <winsock2.h>
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsu_server.hpp"
#include "ess.hpp"
#include "ess_range.hpp"
#include "gc_adapter.hpp"
#include "notch.hpp"
#include "resource.h"
#include "stick.hpp"

// Newer Windows SDK values that older mingw-w64 headers don't have.
static constexpr DWORD DWM_USE_IMMERSIVE_DARK_MODE = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

enum ControlId {
    ID_ADAPTER_STATUS = 100,
    ID_DSU_STATUS,
    ID_SNAP_LABEL,
    ID_SNAP_MINUS,
    ID_SNAP_VALUE,
    ID_SNAP_PLUS,
    ID_SNAP_HINT,
    ID_RANGE_LABEL,
    ID_RANGE_START_MINUS,
    ID_RANGE_START_VALUE,
    ID_RANGE_START_PLUS,
    ID_RANGE_END_LABEL,
    ID_RANGE_END_MINUS,
    ID_RANGE_END_VALUE,
    ID_RANGE_END_PLUS,
    ID_RANGE_DEFAULT,
    ID_EDIT,
    ID_MAIN_HINT,
    ID_PROMPT,
    ID_DONE,
    ID_STICK_PLOT_TITLE,
    ID_GAME_PLOT_TITLE,
    ID_DEAD_ZONE_LABEL,
    ID_DEAD_ZONE_MINUS,
    ID_DEAD_ZONE_VALUE,
    ID_DEAD_ZONE_PLUS,
    ID_DEAD_ZONE_HINT,
    ID_ABOUT,
    ID_ABOUT_TITLE,
    ID_ABOUT_CREDITS,
    ID_ABOUT_LEGAL,
    ID_ABOUT_SOURCE,
    ID_ABOUT_LICENSES,
    ID_ABOUT_CLOSE,
    ID_RANGE_HINT,
    ID_OFFSETS_HINT,
    ID_OFFSETS_PROMPT,
    ID_OFFSETS_RESET,
    ID_OFFSETS_DONE,
    ID_ESS_CHECK = 200,
    ID_CALIBRATE = 300,
    ID_PORT_TEXT = 400,
    ID_NOTCH_TEXT = 500,
    ID_MAIN_PORT = 600,
    ID_CLEAR_NOTCHES = 700,
    ID_NOTCH_OFFSETS = 800,  // per port, opens the notch offsets window
    ID_OFFSET_STEP = 900,    // + 4 * notch + 2 * axis, + 1 for plus
    ID_OFFSET_VALUE = 940,   // + 2 * notch + axis
    ID_OFFSET_NAME = 960,    // + notch, then the X and Y headers
    ID_OFFSET_GAME = 970     // + notch
};

static const COLORREF COLOR_BG = RGB(32, 32, 32);
static const COLORREF COLOR_TEXT = RGB(230, 230, 230);
static const COLORREF COLOR_DIM_TEXT = RGB(150, 150, 150);
static const COLORREF COLOR_FACE = RGB(55, 55, 55);
static const COLORREF COLOR_FACE_DOWN = RGB(80, 80, 80);
static const COLORREF COLOR_BORDER = RGB(95, 95, 95);
static const COLORREF COLOR_ACCENT = RGB(0, 120, 215);
static const COLORREF COLOR_PLOT_BG = RGB(24, 24, 24);
static const COLORREF COLOR_PLOT_AXIS = RGB(50, 50, 50);
static const COLORREF COLOR_DEAD_ZONE = RGB(70, 70, 70);
static const COLORREF COLOR_ESS_RANGE = RGB(0, 70, 125);
static const COLORREF COLOR_DOT_IN_RANGE = RGB(80, 200, 255);
static const COLORREF COLOR_FULL_SPEED = RGB(150, 110, 40);
static const COLORREF COLOR_SOFTWARE_DEAD_ZONE = RGB(110, 110, 110);
static const COLORREF COLOR_CALIBRATION_TARGET = RGB(90, 210, 120);
static const COLORREF COLOR_ACTIVE = RGB(60, 200, 90);
static const COLORREF COLOR_PAUSED = RGB(230, 170, 50);

// Status indicator in the main window's top right corner: amber "Paused" while the settings
// window is open (Dolphin only gets a neutral controller), green "Active" while controller data
// is going out to Dolphin, gray "Idle" otherwise.
enum class LinkState { Idle, Active, Paused };
static const int MAIN_RIGHT_X = 350;   // the indicator and the About button
static const int MAIN_BOTTOM_Y = 176;  // the Edit settings / About row
static const RECT ACTIVE_RECT = {MAIN_RIGHT_X, 10, MAIN_RIGHT_X + 70, 28};
static LinkState g_shown_state = LinkState::Idle;

// Plots in the settings window: the physical stick, and what OoT receives.
static const RECT STICK_PLOT = {10, 252, 266, 508};
static const RECT GAME_PLOT = {276, 252, 532, 508};
static constexpr double STICK_PLOT_SCALE = 1.05;                              // pixels per Gamecube stick unit; fits a ring at 105 + 12
static constexpr int N64_STRAIGHT = 80, N64_DIAGONAL = 70;                    // the N64 stick range, drawn on the game plot
static constexpr double GAME_PLOT_SCALE = 1.5;                                // pixels per in-game unit
static constexpr int LEGEND_HEIGHT = 62;                                      // below each plot, so it covers nothing
static const int SETTINGS_BOTTOM_Y = STICK_PLOT.bottom + LEGEND_HEIGHT + 23;  // the prompt line and Done
static const char* const SETTINGS_CLASS = "SoftwareESSAdapterSettings";
static const char* const ABOUT_CLASS = "SoftwareESSAdapterAbout";
static const char* const OFFSETS_CLASS = "SoftwareESSAdapterOffsets";
static const char* const SOURCE_URL = "https://github.com/TorjeAmundsen/Software-ESS-Adapter";

static DsuServer g_dsu;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_ess[PORT_COUNT] = {true, true, true, true};
static std::atomic<int> g_snap_distance{DEFAULT_SNAP_DISTANCE};
// ESS range as the user sets it, in in-game units, and the physical stick distances that
// gives (see ess_range.hpp). The distances are recomputed whenever the setting changes.
static std::atomic<int> g_range_start{ESS_IN_GAME_START};
static std::atomic<int> g_range_end{ESS_IN_GAME_END};
// Start and end distances packed into one atomic, so the bridge thread never reads a new start
// with an old end.
struct RangeDistances {
    int start;
    int end;
};
static std::atomic<uint32_t> g_range_distances{static_cast<uint32_t>(ESS_DEFAULT_START) << 16 | ESS_DEFAULT_END};

static RangeDistances range_distances() {
    const uint32_t packed = g_range_distances;
    return {static_cast<int>(packed >> 16), static_cast<int>(packed & 0xFFFF)};
}

static void update_range_distances() {
    const int start = physical_start(g_range_start);
    const int end = std::max(physical_end(g_range_end), start + 1);
    g_range_distances = static_cast<uint32_t>(start) << 16 | static_cast<uint32_t>(end);
}

// Software dead zone, in in-game units, and the physical distance it covers.
static std::atomic<int> g_dead_zone{DEAD_ZONE_DEFAULT};
static std::atomic<int> g_dead_zone_distance{0};
// Calibration state. Only the bridge thread changes these; the UI asks for a change through
// g_calibration_request (a port to start, or -1 to stop), so a button press and a click can't
// interleave.
static constexpr int NO_REQUEST = -2;
static std::atomic<int> g_calibration_request{NO_REQUEST};
static std::atomic<int> g_calibrating_port{-1};  // -1 when not calibrating
// Speedrun rules forbid learning more about the controller than looking at it would tell you.
// So settings, stick readouts, and plots live in a separate settings window, and while it is
// open Dolphin gets a neutral controller instead of real input.
static std::atomic<bool> g_editing{false};
static HWND g_settings_window = nullptr;
static HWND g_about_window = nullptr;
// Notch offsets window, owned by the settings window so it only exists while editing.
static HWND g_offsets_window = nullptr;
static int g_offsets_port = 0;
static int g_held_notch = -1;  // the notch the stick is on, highlighted in that window, or -1
static std::atomic<int> g_calibration_step{0};  // 0 to CALIBRATION_STEPS - 1, see notch.hpp

static std::mutex g_mutex;  // guards the six below
static AdapterFrame g_raw{};
static AdapterFrame g_centered{};    // raw, centered on the origin
static AdapterFrame g_calibrated{};  // after notch warp and snapping, before the ESS range and dead zone
static AdapterFrame g_out{};
static std::string g_adapter_status = "Starting";
static std::string g_notice;  // shown in the settings window's prompt line for a few seconds
static std::chrono::steady_clock::time_point g_notice_time;
static std::string g_offsets_notice;  // the same for the notch offsets window
static std::chrono::steady_clock::time_point g_offsets_notice_time;
static NotchCalibration g_notches[PORT_COUNT];

static HBRUSH g_bg_brush;
static HFONT g_font;
static HFONT g_mono_font;   // stick readouts
static HFONT g_title_font;  // About window heading

// Settings live next to the exe: Software ESS Adapter settings.ini. The "settings" part keeps it
// apart from the Dolphin profile, dolphin/Software ESS Adapter.ini.
static std::string settings_path() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string exe = path;
    return exe.substr(0, exe.find_last_of('.')) + " settings.ini";
}

static std::string port_section(int port) {
    return "Port" + std::to_string(port + 1);
}

// Reads "a,b,c,..." with exactly `count` integers in [min, max].
static bool read_ints(const std::string& path, const std::string& section, const char* key, int* values, int count, int min, int max) {
    char buffer[256];
    GetPrivateProfileStringA(section.c_str(), key, "", buffer, sizeof(buffer), path.c_str());
    const char* cursor = buffer;
    for (int i = 0; i < count; ++i) {
        char* end;
        const long value = std::strtol(cursor, &end, 10);
        if (end == cursor || value < min || value > max) {
            return false;
        }
        values[i] = static_cast<int>(value);
        cursor = *end == ',' ? end + 1 : end;
    }
    return true;
}

// Settings file keys, named once so loading and saving can't drift apart.
static const char* const SETTINGS_SECTION = "Settings";
static const char* const KEY_SNAP_DISTANCE = "SnapDistance";
static const char* const KEY_RANGE_START = "EssStartInGame";
static const char* const KEY_RANGE_END = "EssEndInGame";
static const char* const KEY_DEAD_ZONE = "DeadZoneInGame";
static const char* const KEY_ESS = "Ess";                 // per port
static const char* const KEY_NEUTRAL = "NeutralSamples";  // per port
static const char* const KEY_NOTCHES = "Notches";         // per port
static const char* const KEY_NOTCH_OFFSETS = "NotchOffsets";  // per port

// The settings rules in one place: keeps every value in its allowed range and updates the
// distances derived from them. Runs after loading and after every change.
static void apply_settings() {
    g_snap_distance = std::clamp<int>(g_snap_distance, 0, MAX_SNAP_DISTANCE);
    g_range_end = std::clamp<int>(g_range_end, 2, max_in_game_end());
    g_range_start = std::clamp<int>(g_range_start, 1, g_range_end - 1);
    g_dead_zone = std::clamp<int>(g_dead_zone, 0, g_range_start - 1);
    update_range_distances();
    g_dead_zone_distance = physical_dead_zone(g_dead_zone);
}

static void save_settings() {
    const std::string path = settings_path();
    auto write = [&](const char* key, int value) {
        WritePrivateProfileStringA(SETTINGS_SECTION, key, std::to_string(value).c_str(), path.c_str());
    };
    write(KEY_SNAP_DISTANCE, g_snap_distance);
    write(KEY_RANGE_START, g_range_start);
    write(KEY_RANGE_END, g_range_end);
    write(KEY_DEAD_ZONE, g_dead_zone);
}

static void load_settings() {
    const std::string path = settings_path();
    auto read = [&](const char* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntA(SETTINGS_SECTION, key, fallback, path.c_str()));
    };
    g_snap_distance = read(KEY_SNAP_DISTANCE, DEFAULT_SNAP_DISTANCE);
    g_range_start = read(KEY_RANGE_START, ESS_IN_GAME_START);
    g_range_end = read(KEY_RANGE_END, ESS_IN_GAME_END);
    g_dead_zone = read(KEY_DEAD_ZONE, DEAD_ZONE_DEFAULT);
    apply_settings();

    for (int p = 0; p < PORT_COUNT; ++p) {
        g_ess[p] = GetPrivateProfileIntA(port_section(p).c_str(), KEY_ESS, 1, path.c_str()) != 0;

        // NeutralSamples = the 8 raw x,y pairs recorded at rest. Notches = x,y pairs in
        // NOTCH_NAMES order, offsets from the average neutral.
        int neutral[NOTCH_COUNT * 2], notches[NOTCH_COUNT * 2];
        NotchCalibration calibration;
        calibration.valid = read_ints(path, port_section(p), KEY_NEUTRAL, neutral, NOTCH_COUNT * 2, 0, 255) &&
                            read_ints(path, port_section(p), KEY_NOTCHES, notches, NOTCH_COUNT * 2, -128, 127);
        if (calibration.valid) {
            uint8_t samples[NOTCH_COUNT][2];
            for (int i = 0; i < NOTCH_COUNT * 2; ++i) {
                samples[i / 2][i % 2] = static_cast<uint8_t>(neutral[i]);
            }
            set_neutral_samples(calibration, samples);
            for (int i = 0; i < NOTCH_COUNT * 2; ++i) {
                calibration.notch[i / 2][i % 2] = static_cast<int8_t>(notches[i]);
            }
            calibration.valid = misplaced_notch(calibration) < 0;  // a hand-edited file could break the warp
        }
        // NotchOffsets = x,y pairs in NOTCH_NAMES order. One a hand-edited file puts out of bounds
        // stays 0, 0.
        int offsets[NOTCH_COUNT * 2];
        if (read_ints(path, port_section(p), KEY_NOTCH_OFFSETS, offsets, NOTCH_COUNT * 2, -128, 127)) {
            for (int i = 0; i < NOTCH_COUNT; ++i) {
                if (notch_offset_allowed(i, offsets[2 * i], offsets[2 * i + 1])) {
                    calibration.offset[i][0] = static_cast<int8_t>(offsets[2 * i]);
                    calibration.offset[i][1] = static_cast<int8_t>(offsets[2 * i + 1]);
                }
            }
        }
        g_notches[p] = calibration;
    }
}

static void save_ess(int port) {
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_ESS, g_ess[port] ? "1" : "0", settings_path().c_str());
}

static void clear_notches(int port) {
    const std::string path = settings_path();
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NEUTRAL, nullptr, path.c_str());
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NOTCHES, nullptr, path.c_str());
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NOTCH_OFFSETS, nullptr, path.c_str());
}

// A -/+ button in the notch offsets window: which notch and axis it steps, and which way.
struct OffsetStep {
    int notch, axis, direction;
};
static OffsetStep offset_step(int id) {
    const int step = id - ID_OFFSET_STEP;
    return {step / 4, step / 2 % 2, step % 2 ? 1 : -1};
}
static bool is_offset_step(int id) {
    return id >= ID_OFFSET_STEP && id < ID_OFFSET_STEP + NOTCH_COUNT * 4;
}

// Why a -/+ button in the settings window can't go further, or nullptr if it can. The buttons look
// dimmed at their limit, and clicking one shows this reason instead.
static const char* limit_reason(int id) {
    if (is_offset_step(id)) {
        const OffsetStep step = offset_step(id);
        int offset[2];
        {
            std::lock_guard lock(g_mutex);
            offset[0] = g_notches[g_offsets_port].offset[step.notch][0];
            offset[1] = g_notches[g_offsets_port].offset[step.notch][1];
        }
        offset[step.axis] += step.direction;
        if (!notch_offset_allowed(step.notch, offset[0], offset[1])) {
            return "A notch offset can't move a notch past halfway to the next notch or too close to center.";
        }
        return nullptr;
    }
    const int snap = g_snap_distance, start = g_range_start, end = g_range_end, dead_zone = g_dead_zone;
    switch (id) {
    case ID_SNAP_MINUS:
        return snap == 0 ? "Notch snapping is already off." : nullptr;
    case ID_SNAP_PLUS:
        if (snap == MAX_SNAP_DISTANCE) {
            static const std::string reason = "The notch snap distance goes up to " + std::to_string(MAX_SNAP_DISTANCE) + ".";
            return reason.c_str();
        }
        return nullptr;
    case ID_RANGE_START_MINUS:
        if (start > dead_zone + 1) {
            return nullptr;
        }
        return dead_zone > 0 ? "The ESS range start must stay above the software dead zone. Lower the dead zone first."
                             : "The ESS range start can't go any lower.";
    case ID_RANGE_START_PLUS:
        return start == end - 1 ? "The ESS range start must stay below the end." : nullptr;
    case ID_RANGE_END_MINUS:
        return end == start + 1 ? "The ESS range end must stay above the start." : nullptr;
    case ID_RANGE_END_PLUS:
        return end == max_in_game_end() ? "The ESS range end can't go any higher." : nullptr;
    case ID_DEAD_ZONE_MINUS:
        return dead_zone == 0 ? "The software dead zone is already off." : nullptr;
    case ID_DEAD_ZONE_PLUS:
        return dead_zone == start - 1 ? "The software dead zone must stay below the ESS range start. Raise the start first." : nullptr;
    }
    return nullptr;
}

// The -/+ button pairs in the settings window and the setting each pair steps.
struct Stepper {
    int minus, plus;
    std::atomic<int>* value;
};
static const Stepper STEPPERS[] = {{ID_SNAP_MINUS, ID_SNAP_PLUS, &g_snap_distance},
                                   {ID_RANGE_START_MINUS, ID_RANGE_START_PLUS, &g_range_start},
                                   {ID_RANGE_END_MINUS, ID_RANGE_END_PLUS, &g_range_end},
                                   {ID_DEAD_ZONE_MINUS, ID_DEAD_ZONE_PLUS, &g_dead_zone}};

static void set_notice(const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_notice = text;
    g_notice_time = std::chrono::steady_clock::now();
}

static void set_offsets_notice(const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_offsets_notice = text;
    g_offsets_notice_time = std::chrono::steady_clock::now();
}

// "x,y,x,y,..." for NOTCH_COUNT pairs.
template <typename T>
static std::string join_pairs(const T (&pairs)[NOTCH_COUNT][2]) {
    std::string text;
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        text += (i > 0 ? "," : "") + std::to_string(pairs[i][0]) + "," + std::to_string(pairs[i][1]);
    }
    return text;
}

static void save_notch_offsets(int port) {
    NotchCalibration calibration;
    {
        std::lock_guard lock(g_mutex);
        calibration = g_notches[port];
    }
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NOTCH_OFFSETS, join_pairs(calibration.offset).c_str(),
                               settings_path().c_str());
}

static void save_notches(int port, const NotchCalibration& calibration) {
    const std::string path = settings_path();
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NEUTRAL, join_pairs(calibration.neutral_samples).c_str(), path.c_str());
    WritePrivateProfileStringA(port_section(port).c_str(), KEY_NOTCHES, join_pairs(calibration.notch).c_str(), path.c_str());
}

static void set_adapter_status(const std::string& status) {
    std::lock_guard lock(g_mutex);
    g_adapter_status = status;
}

// The stick settings for a port, as they are right now.
static StickSettings stick_settings(int port) {
    const RangeDistances range = range_distances();
    StickSettings settings;
    settings.ess = g_ess[port];
    settings.snap_distance = g_snap_distance;
    settings.range_start = range.start;
    settings.range_end = range.end;
    settings.dead_zone_distance = g_dead_zone_distance;
    return settings;
}

static GCPortState center(const GCPortState& raw, const GCPortState& origin) {
    // Dolphin's emulated pad always reports a centered origin, so center
    // everything here the way the game would against the real origin.
    GCPortState out = raw;
    uint8_t stick_origin[2] = {origin.stick[0], origin.stick[1]};
    normalize_origin(out.stick, stick_origin);
    uint8_t cstick_origin[2] = {origin.cstick[0], origin.cstick[1]};
    normalize_origin(out.cstick, cstick_origin);
    for (int i = 0; i < 2; ++i) {
        out.trigger[i] = raw.trigger[i] > origin.trigger[i] ? raw.trigger[i] - origin.trigger[i] : 0;
    }
    return out;
}

static void bridge_loop() {
    GCAdapter adapter;
    AdapterFrame raw{}, centered{}, calibrated{}, out{}, sent{}, origin{};
    bool has_origin[PORT_COUNT] = {};
    bool prev_a[PORT_COUNT] = {}, prev_b[PORT_COUNT] = {};
    uint8_t samples[CALIBRATION_STEPS][2] = {};  // raw stick per calibration step

    auto take_calibration_request = [] {
        const int request = g_calibration_request.exchange(NO_REQUEST);
        if (request != NO_REQUEST) {
            g_calibration_step = 0;
            g_calibrating_port = request;
        }
    };
    // Without an adapter, forget every controller: the next one plugged in gets a fresh origin,
    // and the windows stop showing stale controllers and stick positions.
    auto forget_controllers = [&] {
        raw = {};
        std::fill(std::begin(has_origin), std::end(has_origin), false);
        g_dsu.publish(AdapterFrame{});  // reports every port as disconnected
        std::lock_guard lock(g_mutex);
        g_raw = g_centered = g_calibrated = g_out = AdapterFrame{};
    };
    auto wait = [&](int ms) {
        for (int waited = 0; waited < ms && g_running; waited += 100) {
            take_calibration_request();
            Sleep(100);
        }
    };

    while (g_running) {
        take_calibration_request();
        if (!adapter.is_open() && !adapter.open()) {
            set_adapter_status(adapter.last_error());
            forget_controllers();
            wait(1000);
            continue;
        }

        const GCAdapter::ReadResult result = adapter.read(raw);
        if (result == GCAdapter::ReadResult::Timeout) {
            continue;
        }
        if (result == GCAdapter::ReadResult::Error) {
            set_adapter_status(adapter.last_error());
            adapter.close();
            forget_controllers();
            wait(100);  // don't spin if the adapter keeps failing right after opening
            continue;
        }

        NotchCalibration notches[PORT_COUNT];
        {
            std::lock_guard lock(g_mutex);
            std::copy(std::begin(g_notches), std::end(g_notches), notches);
        }

        for (int p = 0; p < PORT_COUNT; ++p) {
            // Like Dolphin and the console: the first report after plugging in is the origin.
            if (raw[p].connected && !has_origin[p]) {
                origin[p] = raw[p];
            }
            has_origin[p] = raw[p].connected;
            // With calibrated notches, the main stick is centered on the saved neutral instead.
            GCPortState port_origin = origin[p];
            if (notches[p].valid) {
                port_origin.stick[0] = notches[p].neutral[0];
                port_origin.stick[1] = notches[p].neutral[1];
            }
            centered[p] = center(raw[p], port_origin);
            // Anywhere the stick was seen resting during calibration counts as exactly neutral.
            if (in_neutral_range(notches[p], raw[p].stick)) {
                centered[p].stick[0] = 128;
                centered[p].stick[1] = 128;
            }
            const bool a = raw[p].buttons1 & 0x01;
            const bool b = raw[p].buttons1 & 0x02;

            if (p == g_calibrating_port) {
                // A records the current notch, B cancels.
                if (b && !prev_b[p]) {
                    g_calibrating_port = -1;
                    set_notice("Port " + std::to_string(p + 1) + " calibration cancelled.");
                } else if (a && !prev_a[p]) {
                    const int step = g_calibration_step;
                    const int notch = notch_of_step(step);
                    auto ask_again = [](int i) {
                        set_notice(std::string("That's not the ") + NOTCH_NAMES[i] + " notch. Hold it and press A again.");
                    };
                    // Neutral isn't known yet, so check against the current center. The finished
                    // calibration is checked again against the real neutral below.
                    const int x = raw[p].stick[0] - port_origin.stick[0];
                    const int y = raw[p].stick[1] - port_origin.stick[1];
                    samples[step][0] = raw[p].stick[0];
                    samples[step][1] = raw[p].stick[1];
                    if (!is_neutral_step(step) && !notch_in_section(notch, x, y)) {
                        ask_again(notch);
                    } else if (step + 1 < CALIBRATION_STEPS) {
                        g_calibration_step = step + 1;
                    } else if (const NotchCalibration calibration = make_calibration(samples); misplaced_notch(calibration) >= 0) {
                        const int misplaced = misplaced_notch(calibration);
                        g_calibration_step = 2 * misplaced;
                        ask_again(misplaced);
                    } else {
                        save_notches(p, calibration);
                        {
                            // Calibrating again keeps the notch offsets.
                            std::lock_guard lock(g_mutex);
                            NotchCalibration with_offsets = calibration;
                            std::copy(&g_notches[p].offset[0][0], &g_notches[p].offset[0][0] + NOTCH_COUNT * 2,
                                      &with_offsets.offset[0][0]);
                            g_notches[p] = with_offsets;
                        }
                        g_calibrating_port = -1;
                        set_notice("Port " + std::to_string(p + 1) + " notches calibrated and saved.");
                    }
                }
            }

            out[p] = centered[p];
            calibrated[p] = centered[p];
            process_stick(out[p].stick, notches[p], stick_settings(p), calibrated[p].stick);

            // Calibration only happens while editing, so this also covers it.
            sent[p] = out[p];
            if (g_editing) {
                sent[p] = GCPortState{};
                sent[p].connected = raw[p].connected;
            }
            prev_a[p] = a;
            prev_b[p] = b;
        }
        g_dsu.publish(sent);

        std::lock_guard lock(g_mutex);
        g_raw = raw;
        g_centered = centered;
        g_calibrated = calibrated;
        g_out = out;
        g_adapter_status = "Adapter connected";
    }
}

static void set_text(HWND hwnd, int id, const std::string& text) {
    char current[256];
    GetDlgItemTextA(hwnd, id, current, sizeof(current));
    if (text != current) {
        SetDlgItemTextA(hwnd, id, text.c_str());
        InvalidateRect(GetDlgItem(hwnd, id), nullptr, TRUE);  // owner-drawn buttons need this
    }
}

// Main window: connection status only, no controller values.
static void refresh_main(HWND hwnd) {
    AdapterFrame raw;
    std::string status;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        status = g_adapter_status;
    }
    set_text(hwnd, ID_ADAPTER_STATUS, status);
    if (g_dsu.last_error().empty()) {
        set_text(hwnd, ID_DSU_STATUS, g_dsu.client_count() > 0 ? "Dolphin connected." : "Waiting for Dolphin to connect.");
    }
    for (int p = 0; p < PORT_COUNT; ++p) {
        set_text(hwnd, ID_MAIN_PORT + p,
                 "Port " + std::to_string(p + 1) + ": " + (raw[p].connected ? "controller connected" : "no controller"));
    }
    set_text(hwnd, ID_EDIT, g_settings_window ? "Editing settings" : "Edit settings");

    const LinkState state = g_editing ? LinkState::Paused : g_dsu.is_transmitting() ? LinkState::Active : LinkState::Idle;
    if (state != g_shown_state) {
        g_shown_state = state;
        InvalidateRect(hwnd, &ACTIVE_RECT, TRUE);
    }
}

static void paint_active_indicator(HDC dc) {
    HBRUSH background = CreateSolidBrush(COLOR_BG);
    FillRect(dc, &ACTIVE_RECT, background);
    DeleteObject(background);

    const COLORREF color = g_shown_state == LinkState::Active   ? COLOR_ACTIVE
                           : g_shown_state == LinkState::Paused ? COLOR_PAUSED
                                                                : COLOR_DIM_TEXT;
    const char* text = g_shown_state == LinkState::Active ? "Active" : g_shown_state == LinkState::Paused ? "Paused" : "Idle";
    const int dot = 10;
    const int top = (ACTIVE_RECT.top + ACTIVE_RECT.bottom - dot) / 2;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Ellipse(dc, ACTIVE_RECT.left, top, ACTIVE_RECT.left + dot, top + dot);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    RECT label = ACTIVE_RECT;
    label.left += dot + 6;
    HGDIOBJ old_font = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextA(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    SelectObject(dc, old_font);
}

static void refresh_settings(HWND hwnd) {
    AdapterFrame raw, centered, out;
    bool calibrated[PORT_COUNT];
    std::string notice;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        centered = g_centered;
        out = g_out;
        for (int p = 0; p < PORT_COUNT; ++p) {
            calibrated[p] = g_notches[p].valid;
        }
        if (std::chrono::steady_clock::now() - g_notice_time < std::chrono::seconds(4)) {
            notice = g_notice;
        }
    }

    const int calibrating = g_calibrating_port;
    for (int p = 0; p < PORT_COUNT; ++p) {
        std::string text = "No controller";
        if (raw[p].connected) {
            // Stick convention: neutral is 0, 0, right and up are positive. "Game" is what OoT
            // sees after Wii VC. Fixed-width fields in a monospace font so nothing shifts.
            int in_game[2];
            vc_in_game(out[p].stick[0] - 128, out[p].stick[1] - 128, in_game);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "Stick %4d,%4d  Game %3d,%3d", centered[p].stick[0] - 128, centered[p].stick[1] - 128,
                          in_game[0], in_game[1]);
            text = buffer;
        }
        set_text(hwnd, ID_PORT_TEXT + p, text);
        // A calibrated port shows Notch offsets and Clear in place of the text, except while it calibrates.
        set_text(hwnd, ID_NOTCH_TEXT + p, calibrating == p ? "Calibrating" : calibrated[p] ? "" : "Not calibrated");
        const bool show_buttons = calibrated[p] && calibrating != p;
        for (const int id : {ID_NOTCH_OFFSETS + p, ID_CLEAR_NOTCHES + p}) {
            HWND button = GetDlgItem(hwnd, id);
            if ((IsWindowVisible(button) != FALSE) != show_buttons) {
                ShowWindow(button, show_buttons ? SW_SHOW : SW_HIDE);
            }
        }
        set_text(hwnd, ID_CALIBRATE + p, calibrating == p ? "Cancel" : "Calibrate");
    }

    set_text(hwnd, ID_SNAP_VALUE, std::to_string(g_snap_distance));
    set_text(hwnd, ID_RANGE_START_VALUE, std::to_string(g_range_start));
    set_text(hwnd, ID_RANGE_END_VALUE, std::to_string(g_range_end));
    set_text(hwnd, ID_DEAD_ZONE_VALUE, std::to_string(g_dead_zone));
    // Repaint a -/+ button when it reaches or leaves its limit (it looks dimmed there).
    static bool shown_at_limit[std::size(STEPPERS) * 2] = {};
    size_t button = 0;
    for (const Stepper& stepper : STEPPERS) {
        for (const int id : {stepper.minus, stepper.plus}) {
            const bool at_limit = limit_reason(id) != nullptr;
            if (at_limit != shown_at_limit[button]) {
                shown_at_limit[button] = at_limit;
                InvalidateRect(GetDlgItem(hwnd, id), nullptr, TRUE);
            }
            ++button;
        }
    }

    std::string prompt = notice.empty() ? "Dolphin gets a neutral controller until you close this window." : notice;
    if (calibrating >= 0) {
        const std::string port = std::to_string(calibrating + 1);
        const int step = g_calibration_step;
        const std::string progress = " (" + std::to_string(step + 1) + "/" + std::to_string(CALIBRATION_STEPS) + "). B cancels.";
        if (!notice.empty()) {
            prompt = "Port " + port + ": " + notice;
        } else if (!raw[calibrating].connected) {
            prompt = "Port " + port + " calibration: plug in a controller.";
        } else if (is_neutral_step(step)) {
            prompt = "Port " + port + ": let go of the stick, press A" + progress;
        } else {
            prompt = "Port " + port + ": hold the " + NOTCH_NAMES[notch_of_step(step)] + " notch, press A" + progress;
        }
    }
    set_text(hwnd, ID_PROMPT, prompt);
    for (const RECT& plot : {STICK_PLOT, GAME_PLOT}) {
        const RECT with_legend = {plot.left, plot.top, plot.right, plot.bottom + LEGEND_HEIGHT};
        InvalidateRect(hwnd, &with_legend, FALSE);
    }
}

static void fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void frame(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

// Draws the owner-drawn push buttons and the ESS checkboxes in dark colors.
static void draw_item(const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    RECT rect = item.rcItem;
    char text[64];
    GetWindowTextA(item.hwndItem, text, sizeof(text));
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COLOR_TEXT);
    FillRect(dc, &rect, g_bg_brush);

    const int id = static_cast<int>(item.CtlID);
    if (id >= ID_ESS_CHECK && id < ID_ESS_CHECK + PORT_COUNT) {
        const int size = 14;
        const int top = (rect.top + rect.bottom - size) / 2;
        const RECT box = {rect.left, top, rect.left + size, top + size};
        const bool checked = g_ess[id - ID_ESS_CHECK];
        fill(dc, box, checked ? COLOR_ACCENT : COLOR_FACE);
        frame(dc, box, checked ? COLOR_ACCENT : COLOR_BORDER);
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ old_pen = SelectObject(dc, pen);
            const POINT tick[3] = {{box.left + 3, top + 7}, {box.left + 6, top + 10}, {box.left + 11, top + 4}};
            Polyline(dc, tick, 3);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        RECT label = rect;
        label.left += size + 6;
        DrawTextA(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    } else {
        fill(dc, rect, (item.itemState & ODS_SELECTED) ? COLOR_FACE_DOWN : COLOR_FACE);
        frame(dc, rect, COLOR_BORDER);
        if (limit_reason(id)) {
            SetTextColor(dc, COLOR_DIM_TEXT);  // a -/+ button that can't go further
        }
        DrawTextA(dc, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    // Keyboard focus (Tab), shown only once the keyboard is used, like standard Windows buttons.
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        frame(dc, rect, COLOR_ACCENT);
    }
}

// Off-screen canvas for one plot and its legend underneath, so the 60 Hz redraw doesn't
// flicker. w and h are the plot square; the legend area follows below it.
struct Plot {
    HDC dc;
    int w, h;
    double scale;  // pixels per stick unit
    int px(double x) const {
        return w / 2 + static_cast<int>(std::lround(x * scale));
    }
    int py(double y) const {
        return h / 2 - static_cast<int>(std::lround(y * scale));
    }
};

template <typename Draw>
static void draw_plot(HDC target, const RECT& rect, double scale, Draw draw) {
    Plot plot{CreateCompatibleDC(target), rect.right - rect.left, rect.bottom - rect.top, scale};
    HBITMAP bitmap = CreateCompatibleBitmap(target, plot.w, plot.h + LEGEND_HEIGHT);
    HGDIOBJ old_bitmap = SelectObject(plot.dc, bitmap);
    HGDIOBJ old_pen = SelectObject(plot.dc, GetStockObject(DC_PEN));
    HGDIOBJ old_brush = SelectObject(plot.dc, GetStockObject(DC_BRUSH));
    HGDIOBJ old_font = SelectObject(plot.dc, g_font);
    SetBkMode(plot.dc, TRANSPARENT);
    const RECT area = {0, 0, plot.w, plot.h};
    fill(plot.dc, area, COLOR_PLOT_BG);
    fill(plot.dc, {0, plot.h, plot.w, plot.h + LEGEND_HEIGHT}, COLOR_BG);

    draw(plot);

    frame(plot.dc, area, COLOR_BORDER);
    BitBlt(target, rect.left, rect.top, plot.w, plot.h + LEGEND_HEIGHT, plot.dc, 0, 0, SRCCOPY);
    SelectObject(plot.dc, old_font);
    SelectObject(plot.dc, old_brush);
    SelectObject(plot.dc, old_pen);
    SelectObject(plot.dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(plot.dc);
}

static void draw_axes(const Plot& plot) {
    SetDCPenColor(plot.dc, COLOR_PLOT_AXIS);
    MoveToEx(plot.dc, 0, plot.h / 2, nullptr);
    LineTo(plot.dc, plot.w, plot.h / 2);
    MoveToEx(plot.dc, plot.w / 2, 0, nullptr);
    LineTo(plot.dc, plot.w / 2, plot.h);
}

// Octagon through (0, straight) and (corner, corner) in each direction.
static void draw_gate(const Plot& plot, double straight, double corner, COLORREF color) {
    const double points[NOTCH_COUNT][2] = {{0, straight},  {corner, corner},   {straight, 0},  {corner, -corner},
                                           {0, -straight}, {-corner, -corner}, {-straight, 0}, {-corner, corner}};
    POINT gate[NOTCH_COUNT + 1];
    for (int i = 0; i <= NOTCH_COUNT; ++i) {
        gate[i] = {plot.px(points[i % NOTCH_COUNT][0]), plot.py(points[i % NOTCH_COUNT][1])};
    }
    SetDCPenColor(plot.dc, color);
    Polyline(plot.dc, gate, NOTCH_COUNT + 1);
}

static void draw_dot(const Plot& plot, int port, double x, double y, bool highlight) {
    SetDCPenColor(plot.dc, COLOR_PLOT_BG);
    SetDCBrushColor(plot.dc, highlight ? COLOR_DOT_IN_RANGE : COLOR_TEXT);
    const int cx = plot.px(x), cy = plot.py(y);
    Ellipse(plot.dc, cx - 4, cy - 4, cx + 5, cy + 5);
    const std::string label = std::to_string(port + 1);
    SetTextColor(plot.dc, COLOR_TEXT);
    TextOutA(plot.dc, cx + 6, cy - 16, label.c_str(), static_cast<int>(label.size()));
}

// Legend under the plot: one colored swatch (or line) and label per row.
struct LegendRow {
    COLORREF color;
    bool line;
    std::string text;
};

static void draw_legend(const Plot& plot, const std::vector<LegendRow>& rows) {
    SetTextColor(plot.dc, COLOR_TEXT);
    for (size_t i = 0; i < rows.size(); ++i) {
        const int y = plot.h + 6 + 18 * static_cast<int>(i);
        if (rows[i].line) {
            fill(plot.dc, {2, y + 7, 12, y + 9}, rows[i].color);
        } else {
            fill(plot.dc, {2, y + 3, 12, y + 13}, rows[i].color);
        }
        TextOutA(plot.dc, 18, y, rows[i].text.c_str(), static_cast<int>(rows[i].text.size()));
    }
}

static void paint_plots(HDC target) {
    AdapterFrame raw, centered, calibrated, out;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        centered = g_centered;
        calibrated = g_calibrated;
        out = g_out;
    }
    const RangeDistances range = range_distances();
    const int start = range.start, end = range.end;

    // Physical stick: the dead zone and ESS range to scale, inside the Gamecube gate.
    draw_plot(target, STICK_PLOT, STICK_PLOT_SCALE, [&](const Plot& plot) {
        auto disk = [&](double r, COLORREF color) {
            SetDCPenColor(plot.dc, color);
            SetDCBrushColor(plot.dc, color);
            Ellipse(plot.dc, plot.px(-r), plot.py(r), plot.px(r) + 1, plot.py(-r) + 1);
        };
        disk(end, COLOR_ESS_RANGE);
        disk(start, COLOR_DEAD_ZONE);
        if (g_dead_zone_distance > 0) {
            // This plot is before the ESS range, where the dead zone has its unscaled size.
            const double half = g_dead_zone_distance;
            SetDCPenColor(plot.dc, COLOR_SOFTWARE_DEAD_ZONE);
            SetDCBrushColor(plot.dc, COLOR_SOFTWARE_DEAD_ZONE);
            Rectangle(plot.dc, plot.px(-half), plot.py(half), plot.px(half) + 1, plot.py(-half) + 1);
        }
        draw_axes(plot);
        draw_gate(plot, GATE_STRAIGHT, GATE_DIAGONAL, COLOR_BORDER);

        // While calibrating, a ring around where the stick should go for the current step:
        // the ideal notch, or the center for a neutral step.
        if (g_calibrating_port >= 0) {
            const int step = g_calibration_step;
            const int8_t* target = is_neutral_step(step) ? nullptr : IDEAL_NOTCHES[notch_of_step(step)];
            const double tx = target ? target[0] : 0, ty = target ? target[1] : 0, r = 12;
            HPEN pen = CreatePen(PS_SOLID, 2, COLOR_CALIBRATION_TARGET);
            HGDIOBJ old_pen = SelectObject(plot.dc, pen);
            HGDIOBJ old_brush = SelectObject(plot.dc, GetStockObject(NULL_BRUSH));
            Ellipse(plot.dc, plot.px(tx - r), plot.py(ty + r), plot.px(tx + r) + 1, plot.py(ty - r) + 1);
            SelectObject(plot.dc, old_brush);
            SelectObject(plot.dc, old_pen);
            DeleteObject(pen);
        }

        for (int p = 0; p < PORT_COUNT; ++p) {
            if (!raw[p].connected) {
                continue;
            }
            // The stick after notch calibration, so it lines up with the ESS range. A port that is
            // calibrating shows its raw stick instead, to match the green target ring.
            const GCPortState& shown = p == g_calibrating_port ? centered[p] : calibrated[p];
            const int x = shown.stick[0] - 128, y = shown.stick[1] - 128;
            const double r = std::sqrt(static_cast<double>(x * x + y * y));
            draw_dot(plot, p, x, y, r >= start && r <= end);
        }
        draw_legend(plot, {{COLOR_ESS_RANGE, false,
                            "ESS range " + std::to_string(g_range_start) + " to " + std::to_string(g_range_end) + " in game"},
                           {COLOR_DEAD_ZONE, false, "Game dead zone"},
                           {COLOR_SOFTWARE_DEAD_ZONE, false, "Software dead zone (reads 0, 0)"}});
    });

    // In game: OoT's own zones in the values it receives from VC. ESS is everything within 20
    // of the dead zone square, so a rounded square; full speed is the same shape at 60.
    draw_plot(target, GAME_PLOT, GAME_PLOT_SCALE, [&](const Plot& plot) {
        auto rounded_square = [&](double half, double radius, COLORREF color, bool filled) {
            SetDCPenColor(plot.dc, color);
            SetDCBrushColor(plot.dc, color);
            SelectObject(plot.dc, GetStockObject(filled ? DC_BRUSH : NULL_BRUSH));
            const int diameter = static_cast<int>(std::lround(2 * radius * plot.scale));
            RoundRect(plot.dc, plot.px(-half), plot.py(half), plot.px(half) + 1, plot.py(-half) + 1, diameter, diameter);
            SelectObject(plot.dc, GetStockObject(DC_BRUSH));
        };
        // Half a unit past the last value in each zone, so whole values fall inside.
        const double ess_radius = OOT_WALK_MAGNITUDE - 0.5;
        rounded_square(OOT_DEAD_ZONE + ess_radius, ess_radius, COLOR_ESS_RANGE, true);
        rounded_square(OOT_DEAD_ZONE + 0.5, 0, COLOR_DEAD_ZONE, true);
        draw_axes(plot);
        rounded_square(OOT_DEAD_ZONE + OOT_REL_MAX, OOT_REL_MAX, COLOR_FULL_SPEED, false);
        draw_gate(plot, N64_STRAIGHT, N64_DIAGONAL, COLOR_BORDER);
        for (int p = 0; p < PORT_COUNT; ++p) {
            if (!raw[p].connected) {
                continue;
            }
            int in_game[2];
            vc_in_game(out[p].stick[0] - 128, out[p].stick[1] - 128, in_game);
            draw_dot(plot, p, in_game[0], in_game[1], oot_zone(in_game[0], in_game[1]) == OotZone::Ess);
        }
        draw_legend(plot, {{COLOR_ESS_RANGE, false, "ESS (turn in place)"},
                           {COLOR_DEAD_ZONE, false, "Game dead zone"},
                           {COLOR_FULL_SPEED, true, "Full speed"}});
    });
}

static HWND add_control(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) {
    const DWORD tab_stop = std::string(cls) == "BUTTON" ? WS_TABSTOP : 0;
    HWND control = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | tab_stop | style, x, y, w, h, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

// Hint text drawn in the dim color (see handle_dark_controls).
static std::vector<HWND> g_dim_text;

static HWND add_dim_text(HWND parent, const char* text, int x, int y, int w, int h, int id) {
    HWND control = add_control(parent, "STATIC", text, 0, x, y, w, h, id);
    g_dim_text.push_back(control);
    return control;
}

// Owner-drawn buttons report fast second clicks as double clicks, so both count.
static bool is_click(WPARAM wparam) {
    const int code = HIWORD(wparam);
    return code == BN_CLICKED || code == BN_DOUBLECLICKED;
}

// A window that can't be resized, with an inside (client area) of width x height.
static HWND create_fixed_window(const char* cls, const char* title, DWORD style, int width, int height, HWND owner) {
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);
    return CreateWindowA(cls, title, style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, owner, nullptr,
                         GetModuleHandleA(nullptr), nullptr);
}

static void open_offsets(HWND owner, int port);

static void on_settings_command(HWND hwnd, int id) {
    if (id == ID_DONE || id == IDCANCEL) {  // IDCANCEL: Escape
        DestroyWindow(hwnd);
        return;
    }
    if (const char* reason = limit_reason(id)) {
        set_notice(reason);
        refresh_settings(hwnd);
        return;
    }
    for (const Stepper& stepper : STEPPERS) {
        if (id == stepper.minus || id == stepper.plus) {
            *stepper.value += id == stepper.plus ? 1 : -1;
            apply_settings();
            save_settings();
            refresh_settings(hwnd);
            return;
        }
    }
    if (id >= ID_ESS_CHECK && id < ID_ESS_CHECK + PORT_COUNT) {
        const int port = id - ID_ESS_CHECK;
        g_ess[port] = !g_ess[port];
        save_ess(port);
        InvalidateRect(GetDlgItem(hwnd, id), nullptr, TRUE);
    } else if (id >= ID_CALIBRATE && id < ID_CALIBRATE + PORT_COUNT) {
        const int port = id - ID_CALIBRATE;
        if (g_calibrating_port == port) {
            g_calibration_request = -1;
            set_notice("Port " + std::to_string(port + 1) + " calibration cancelled.");
        } else {
            g_calibration_request = port;
            set_notice("");  // an old notice would hide the first step's prompt
        }
    } else if (id >= ID_CLEAR_NOTCHES && id < ID_CLEAR_NOTCHES + PORT_COUNT) {
        const int port = id - ID_CLEAR_NOTCHES;
        const std::string question = "Clear the notch calibration and notch offsets for port " + std::to_string(port + 1) + "?";
        if (MessageBoxA(hwnd, question.c_str(), "Software ESS Adapter", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            if (g_offsets_window && g_offsets_port == port) {
                DestroyWindow(g_offsets_window);
            }
            {
                std::lock_guard lock(g_mutex);
                g_notches[port] = NotchCalibration{};
            }
            clear_notches(port);
            set_notice("Port " + std::to_string(port + 1) + " notch calibration and notch offsets cleared.");
        }
    } else if (id >= ID_NOTCH_OFFSETS && id < ID_NOTCH_OFFSETS + PORT_COUNT) {
        open_offsets(hwnd, id - ID_NOTCH_OFFSETS);
    } else if (id == ID_RANGE_DEFAULT) {
        g_range_start = ESS_IN_GAME_START;
        g_range_end = ESS_IN_GAME_END;
        g_dead_zone = DEAD_ZONE_DEFAULT;
        apply_settings();
        save_settings();
    }
    refresh_settings(hwnd);
}

// Messages both windows handle the same way: dark static text and owner-drawn buttons.
static bool handle_dark_controls(UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (msg == WM_CTLCOLORSTATIC) {
        HDC dc = reinterpret_cast<HDC>(wparam);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
        const bool dim = std::find(g_dim_text.begin(), g_dim_text.end(), reinterpret_cast<HWND>(lparam)) != g_dim_text.end();
        // The held notch's name and Game value in the notch offsets window.
        const bool held = g_held_notch >= 0 && (id == ID_OFFSET_NAME + g_held_notch || id == ID_OFFSET_GAME + g_held_notch);
        SetTextColor(dc, held ? COLOR_DOT_IN_RANGE : dim ? COLOR_DIM_TEXT : COLOR_TEXT);
        SetBkColor(dc, COLOR_BG);
        result = reinterpret_cast<LRESULT>(g_bg_brush);
        return true;
    }
    if (msg == WM_DRAWITEM) {
        draw_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        result = TRUE;
        return true;
    }
    return false;
}

static void use_dark_title_bar(HWND hwnd) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWM_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result;
    if (handle_dark_controls(msg, wparam, lparam, result)) {
        return result;
    }

    switch (msg) {
    case WM_CREATE:
        use_dark_title_bar(hwnd);
        for (int p = 0; p < PORT_COUNT; ++p) {
            const int y = 10 + p * 30;
            const std::string label = "Port " + std::to_string(p + 1) + " ESS";
            add_control(hwnd, "BUTTON", label.c_str(), BS_OWNERDRAW, 10, y, 85, 22, ID_ESS_CHECK + p);
            add_control(hwnd, "BUTTON", "Calibrate", BS_OWNERDRAW, 100, y, 75, 22, ID_CALIBRATE + p);
            HWND readout = add_control(hwnd, "STATIC", "", 0, 185, y + 3, 212, 18, ID_PORT_TEXT + p);
            SendMessageA(readout, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono_font), TRUE);
            add_control(hwnd, "STATIC", "", 0, 404, y + 3, 134, 18, ID_NOTCH_TEXT + p);
            ShowWindow(add_control(hwnd, "BUTTON", "Notch offsets", BS_OWNERDRAW, 404, y, 86, 22, ID_NOTCH_OFFSETS + p), SW_HIDE);
            ShowWindow(add_control(hwnd, "BUTTON", "Clear", BS_OWNERDRAW, 494, y, 44, 22, ID_CLEAR_NOTCHES + p), SW_HIDE);
        }
        {
            // Settings as a table: label, -/+ controls, short hint. Every row's controls start at
            // the same x, and every hint too.
            const int label_x = 10, control_x = 145, hint_x = 420;
            auto stepper = [&](int x, int y, int minus, int value, int plus) {
                add_control(hwnd, "BUTTON", "-", BS_OWNERDRAW, x, y, 24, 22, minus);
                add_control(hwnd, "STATIC", "", SS_CENTER, x + 26, y + 3, 30, 18, value);
                add_control(hwnd, "BUTTON", "+", BS_OWNERDRAW, x + 58, y, 24, 22, plus);
            };
            add_control(hwnd, "STATIC", "Notch snapping", 0, label_x, 137, 130, 18, ID_SNAP_LABEL);
            stepper(control_x, 134, ID_SNAP_MINUS, ID_SNAP_VALUE, ID_SNAP_PLUS);
            add_dim_text(hwnd, "stick units, 0 = off", hint_x, 137, 115, 18, ID_SNAP_HINT);

            add_control(hwnd, "STATIC", "ESS range", 0, label_x, 167, 130, 18, ID_RANGE_LABEL);
            stepper(control_x, 164, ID_RANGE_START_MINUS, ID_RANGE_START_VALUE, ID_RANGE_START_PLUS);
            add_control(hwnd, "STATIC", "to", SS_CENTER, control_x + 86, 167, 22, 18, ID_RANGE_END_LABEL);
            stepper(control_x + 112, 164, ID_RANGE_END_MINUS, ID_RANGE_END_VALUE, ID_RANGE_END_PLUS);
            add_control(hwnd, "BUTTON", "Default", BS_OWNERDRAW, control_x + 204, 164, 62, 22, ID_RANGE_DEFAULT);
            add_dim_text(hwnd, "in game", hint_x, 167, 115, 18, ID_RANGE_HINT);

            add_control(hwnd, "STATIC", "Software dead zone", 0, label_x, 197, 130, 18, ID_DEAD_ZONE_LABEL);
            stepper(control_x, 194, ID_DEAD_ZONE_MINUS, ID_DEAD_ZONE_VALUE, ID_DEAD_ZONE_PLUS);
            add_dim_text(hwnd, "in game, 0 = off", hint_x, 197, 115, 18, ID_DEAD_ZONE_HINT);
        }
        add_control(hwnd, "STATIC", "Your stick", 0, STICK_PLOT.left, STICK_PLOT.top - 20, 200, 18, ID_STICK_PLOT_TITLE);
        add_control(hwnd, "STATIC", "In game (OoT on Wii VC)", 0, GAME_PLOT.left, GAME_PLOT.top - 20, 200, 18, ID_GAME_PLOT_TITLE);
        add_control(hwnd, "STATIC", "", 0, 10, SETTINGS_BOTTOM_Y + 4, 410, 18, ID_PROMPT);
        add_control(hwnd, "BUTTON", "Done", BS_OWNERDRAW, GAME_PLOT.right - 100, SETTINGS_BOTTOM_Y, 100, 24, ID_DONE);
        refresh_settings(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_plots(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (is_click(wparam)) {
            on_settings_command(hwnd, LOWORD(wparam));
        }
        return 0;
    case WM_DESTROY:
        // Closing the settings window ends editing: real input goes back to Dolphin.
        g_calibration_request = -1;
        g_editing = false;
        g_settings_window = nullptr;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

// Notch offsets window: an offset per notch, each axis with -/+ buttons, and where holding
// that notch lands in game with everything else applied.
static const int OFFSETS_ROW_Y = 70, OFFSETS_ROW_HEIGHT = 28;
static const int OFFSETS_BOTTOM_Y = OFFSETS_ROW_Y + NOTCH_COUNT * OFFSETS_ROW_HEIGHT + 4;  // the prompt line, then the buttons

// Highlight a notch while the stick, after calibration and snapping, is this close to its target
// (or within the snap distance, if that's further).
static constexpr int HELD_DISTANCE = 6;

static void refresh_offsets(HWND hwnd) {
    const int port = g_offsets_port;
    NotchCalibration calibration;
    std::string notice;
    GCPortState stick;
    {
        std::lock_guard lock(g_mutex);
        calibration = g_notches[port];
        stick = g_calibrated[port];
        if (std::chrono::steady_clock::now() - g_offsets_notice_time < std::chrono::seconds(4)) {
            notice = g_offsets_notice;
        }
    }
    const StickSettings settings = stick_settings(port);

    int held = -1;
    if (stick.connected && calibration.valid) {
        const int reach = std::max(HELD_DISTANCE, settings.snap_distance);
        int best = reach * reach;
        for (int i = 0; i < NOTCH_COUNT; ++i) {
            int target[2];
            notch_target(calibration, i, target);
            const int dx = stick.stick[0] - 128 - target[0], dy = stick.stick[1] - 128 - target[1];
            if (dx * dx + dy * dy <= best) {
                best = dx * dx + dy * dy;
                held = i;
            }
        }
    }
    if (held != g_held_notch) {
        for (const int notch : {g_held_notch, held}) {
            if (notch >= 0) {
                InvalidateRect(GetDlgItem(hwnd, ID_OFFSET_NAME + notch), nullptr, TRUE);
                InvalidateRect(GetDlgItem(hwnd, ID_OFFSET_GAME + notch), nullptr, TRUE);
            }
        }
        g_held_notch = held;
    }
    // With ESS on, the ESS map gives the same in-game value for neighboring sticks, so a click
    // doesn't always change Game.
    if (notice.empty() && settings.ess) {
        notice = "With ESS on, some steps don't change Game.";
    }
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
            set_text(hwnd, ID_OFFSET_VALUE + 2 * i + axis, std::to_string(calibration.offset[i][axis]));
        }
        // The calibrated notch, centered on neutral as the bridge loop does it, through the
        // whole stick path.
        uint8_t stick[2] = {static_cast<uint8_t>(128 + calibration.notch[i][0]), static_cast<uint8_t>(128 + calibration.notch[i][1])};
        process_stick(stick, calibration, settings);
        int in_game[2];
        vc_in_game(stick[0] - 128, stick[1] - 128, in_game);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "Game %3d,%3d", in_game[0], in_game[1]);
        set_text(hwnd, ID_OFFSET_GAME + i, buffer);
    }
    // Repaint a -/+ button when it reaches or leaves its limit (it looks dimmed there).
    static bool shown_at_limit[NOTCH_COUNT * 4] = {};
    for (int step = 0; step < NOTCH_COUNT * 4; ++step) {
        const bool at_limit = limit_reason(ID_OFFSET_STEP + step) != nullptr;
        if (at_limit != shown_at_limit[step]) {
            shown_at_limit[step] = at_limit;
            InvalidateRect(GetDlgItem(hwnd, ID_OFFSET_STEP + step), nullptr, TRUE);
        }
    }
    set_text(hwnd, ID_OFFSETS_PROMPT, notice);
}

static void on_offsets_command(HWND hwnd, int id) {
    if (id == ID_OFFSETS_DONE || id == IDCANCEL) {  // IDCANCEL: Escape
        DestroyWindow(hwnd);
        return;
    }
    if (const char* reason = limit_reason(id)) {
        set_offsets_notice(reason);
        refresh_offsets(hwnd);
        return;
    }
    const int port = g_offsets_port;
    if (is_offset_step(id)) {
        const OffsetStep step = offset_step(id);
        std::lock_guard lock(g_mutex);
        g_notches[port].offset[step.notch][step.axis] += step.direction;
    } else if (id == ID_OFFSETS_RESET) {
        // Ask first, like Clear.
        bool any = false;
        {
            std::lock_guard lock(g_mutex);
            for (const auto& offset : g_notches[port].offset) {
                any = any || offset[0] != 0 || offset[1] != 0;
            }
        }
        if (!any) {
            set_offsets_notice("Every notch offset is already 0.");
            refresh_offsets(hwnd);
            return;
        }
        const std::string question = "Reset every notch offset for port " + std::to_string(port + 1) + " to 0?";
        if (MessageBoxA(hwnd, question.c_str(), "Software ESS Adapter", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        std::lock_guard lock(g_mutex);
        std::fill(&g_notches[port].offset[0][0], &g_notches[port].offset[0][0] + NOTCH_COUNT * 2, 0);
    } else {
        return;
    }
    save_notch_offsets(port);
    refresh_offsets(hwnd);
}

static LRESULT CALLBACK offsets_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result;
    if (handle_dark_controls(msg, wparam, lparam, result)) {
        return result;
    }

    switch (msg) {
    case WM_CREATE: {
        use_dark_title_bar(hwnd);
        add_dim_text(hwnd, "Move where each calibrated notch lands, in stick units. Game shows what OoT reads when you hold it.", 10,
                     10, 380, 34, ID_OFFSETS_HINT);
        const int x_column = 90, y_column = 186, game_column = 284;
        add_control(hwnd, "STATIC", "X", SS_CENTER, x_column, OFFSETS_ROW_Y - 20, 82, 18, ID_OFFSET_NAME + NOTCH_COUNT);
        add_control(hwnd, "STATIC", "Y", SS_CENTER, y_column, OFFSETS_ROW_Y - 20, 82, 18, ID_OFFSET_NAME + NOTCH_COUNT + 1);
        for (int i = 0; i < NOTCH_COUNT; ++i) {
            const int y = OFFSETS_ROW_Y + i * OFFSETS_ROW_HEIGHT;
            add_control(hwnd, "STATIC", NOTCH_NAMES[i], 0, 10, y + 3, 75, 18, ID_OFFSET_NAME + i);
            for (int axis = 0; axis < 2; ++axis) {
                const int x = axis == 0 ? x_column : y_column;
                const int step = ID_OFFSET_STEP + 4 * i + 2 * axis;
                add_control(hwnd, "BUTTON", "-", BS_OWNERDRAW, x, y, 24, 22, step);
                add_control(hwnd, "STATIC", "", SS_CENTER, x + 26, y + 3, 30, 18, ID_OFFSET_VALUE + 2 * i + axis);
                add_control(hwnd, "BUTTON", "+", BS_OWNERDRAW, x + 58, y, 24, 22, step + 1);
            }
            HWND game = add_control(hwnd, "STATIC", "", 0, game_column, y + 3, 106, 18, ID_OFFSET_GAME + i);
            SendMessageA(game, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono_font), TRUE);
        }
        add_control(hwnd, "STATIC", "", 0, 10, OFFSETS_BOTTOM_Y, 380, 18, ID_OFFSETS_PROMPT);
        add_control(hwnd, "BUTTON", "Reset all", BS_OWNERDRAW, 10, OFFSETS_BOTTOM_Y + 24, 90, 24, ID_OFFSETS_RESET);
        add_control(hwnd, "BUTTON", "Done", BS_OWNERDRAW, 290, OFFSETS_BOTTOM_Y + 24, 100, 24, ID_OFFSETS_DONE);
        refresh_offsets(hwnd);
        return 0;
    }
    case WM_COMMAND:
        if (is_click(wparam)) {
            on_offsets_command(hwnd, LOWORD(wparam));
        }
        return 0;
    case WM_DESTROY:
        g_offsets_window = nullptr;
        g_held_notch = -1;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void open_offsets(HWND owner, int port) {
    if (g_offsets_window && g_offsets_port == port) {
        SetForegroundWindow(g_offsets_window);
        return;
    }
    if (g_offsets_window) {
        DestroyWindow(g_offsets_window);
    }
    g_offsets_port = port;
    const std::string title = "Port " + std::to_string(port + 1) + " notch offsets";
    g_offsets_window =
        create_fixed_window(OFFSETS_CLASS, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 400, OFFSETS_BOTTOM_Y + 58, owner);
    // Beside the settings window, so its plots stay visible: to the right if it fits on that
    // screen, else to the left, else where Windows put it.
    RECT settings, own;
    GetWindowRect(owner, &settings);
    GetWindowRect(g_offsets_window, &own);
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoA(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& screen = monitor.rcWork;
    const int width = own.right - own.left, height = own.bottom - own.top;
    const int top = std::clamp<int>(settings.top, screen.top, std::max<int>(screen.top, screen.bottom - height));
    if (settings.right + width <= screen.right) {
        SetWindowPos(g_offsets_window, nullptr, settings.right, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (settings.left - width >= screen.left) {
        SetWindowPos(g_offsets_window, nullptr, settings.left - width, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(g_offsets_window, SW_SHOW);
}

static void open_settings(HWND owner) {
    if (g_settings_window) {
        SetForegroundWindow(g_settings_window);
        return;
    }
    g_editing = true;
    g_settings_window = create_fixed_window(SETTINGS_CLASS, "Software ESS Adapter settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                            GAME_PLOT.right + 10, SETTINGS_BOTTOM_Y + 34, owner);
    ShowWindow(g_settings_window, SW_SHOW);
}

// A text resource from assets/resources.rc, with Windows line endings.
static std::string load_text_resource(int id) {
    HRSRC resource = FindResourceA(nullptr, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10));  // RT_RCDATA
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const char* data = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (!data) {
        return "";
    }
    const std::string text(data, SizeofResource(nullptr, resource));
    std::string crlf;
    for (char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        if (c != '\r') {
            crlf += c;
        }
    }
    return crlf;
}

// Writes the license texts built into the exe to a temporary text file and opens it in the
// default text editor.
static void open_licenses(HWND hwnd) {
    char folder[MAX_PATH];
    GetTempPathA(MAX_PATH, folder);
    const std::string path = std::string(folder) + "Software ESS Adapter licenses.txt";
    const std::string text = load_text_resource(GPL_TEXT) + "\r\n\r\n" + load_text_resource(THIRD_PARTY_TEXT);
    if (FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
        ShellExecuteA(hwnd, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// About window: name and version, credits, the GPL's no-warranty notice, the source link, and a
// way to read the licenses. GPLv3 asks interactive programs to show this, and it keeps the release
// zip free of separate license files.
static LRESULT CALLBACK about_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result;
    if (handle_dark_controls(msg, wparam, lparam, result)) {
        return result;
    }

    switch (msg) {
    case WM_CREATE: {
        use_dark_title_bar(hwnd);
        HWND title = add_control(hwnd, "STATIC", "Software ESS Adapter v" APP_VERSION, 0, 14, 14, 330, 24, ID_ABOUT_TITLE);
        SendMessageA(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_title_font), TRUE);
        add_control(hwnd, "STATIC", "Created by Torje Amundsen\r\nESS map from ESS-Adapter by Skuzee", 0, 14, 44, 330, 34,
                    ID_ABOUT_CREDITS);
        add_dim_text(hwnd, "Free software under the GNU GPL v3, with no warranty.", 14, 88, 330, 18, ID_ABOUT_LEGAL);
        add_control(hwnd, "BUTTON", "Source code", BS_OWNERDRAW, 14, 122, 100, 24, ID_ABOUT_SOURCE);
        add_control(hwnd, "BUTTON", "Licenses", BS_OWNERDRAW, 122, 122, 90, 24, ID_ABOUT_LICENSES);
        add_control(hwnd, "BUTTON", "Close", BS_OWNERDRAW, 254, 122, 90, 24, ID_ABOUT_CLOSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!is_click(wparam)) {
            return 0;
        }
        switch (LOWORD(wparam)) {
        case ID_ABOUT_SOURCE:
            ShellExecuteA(hwnd, "open", SOURCE_URL, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_ABOUT_LICENSES:
            open_licenses(hwnd);
            break;
        case ID_ABOUT_CLOSE:
        case IDCANCEL:  // Escape
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    }
    case WM_DESTROY:
        g_about_window = nullptr;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void open_about(HWND owner) {
    if (g_about_window) {
        SetForegroundWindow(g_about_window);
        return;
    }
    g_about_window =
        create_fixed_window(ABOUT_CLASS, "About Software ESS Adapter", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 358, 160, owner);
    ShowWindow(g_about_window, SW_SHOW);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result;
    if (handle_dark_controls(msg, wparam, lparam, result)) {
        return result;
    }

    switch (msg) {
    case WM_CREATE:
        use_dark_title_bar(hwnd);
        // The status lines wrap to two lines so error messages fit.
        add_control(hwnd, "STATIC", "", 0, 10, 10, 335, 34, ID_ADAPTER_STATUS);
        add_control(hwnd, "STATIC", g_dsu.last_error().c_str(), 0, 10, 46, 335, 34, ID_DSU_STATUS);
        for (int p = 0; p < PORT_COUNT; ++p) {
            add_control(hwnd, "STATIC", "", 0, 10, 86 + p * 20, 340, 18, ID_MAIN_PORT + p);
        }
        add_control(hwnd, "BUTTON", "Edit settings", BS_OWNERDRAW, 10, MAIN_BOTTOM_Y, 120, 24, ID_EDIT);
        add_dim_text(hwnd, "Stick values only show while editing.", 140, MAIN_BOTTOM_Y + 4, 205, 18, ID_MAIN_HINT);
        add_control(hwnd, "BUTTON", "About", BS_OWNERDRAW, MAIN_RIGHT_X, MAIN_BOTTOM_Y, 70, 24, ID_ABOUT);
        refresh_main(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_active_indicator(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (is_click(wparam) && LOWORD(wparam) == ID_EDIT) {
            open_settings(hwnd);
        } else if (is_click(wparam) && LOWORD(wparam) == ID_ABOUT) {
            open_about(hwnd);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    // Only one copy can own the adapter and the DSU port, so a second start just shows the first.
    CreateMutexA(nullptr, FALSE, "SoftwareESSAdapter.Running");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowA("SoftwareESSAdapter", nullptr)) {
            ShowWindow(running, SW_RESTORE);
            SetForegroundWindow(running);
        }
        return 0;
    }

    load_settings();
    g_dsu.start(DsuServer::DEFAULT_PORT);
    std::thread bridge(bridge_loop);

    g_bg_brush = CreateSolidBrush(COLOR_BG);
    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_mono_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    g_title_font = CreateFontA(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    // App icon from assets/resources.rc, at the system's large and small icon sizes.
    auto load_icon = [&](int size_metric) {
        const int size = GetSystemMetrics(size_metric);
        return static_cast<HICON>(LoadImageA(instance, MAKEINTRESOURCEA(APP_ICON), IMAGE_ICON, size, size, 0));
    };

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hIcon = load_icon(SM_CXICON);
    wc.hIconSm = load_icon(SM_CXSMICON);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = "SoftwareESSAdapter";
    RegisterClassExA(&wc);
    wc.lpfnWndProc = settings_proc;
    wc.lpszClassName = SETTINGS_CLASS;
    RegisterClassExA(&wc);
    wc.lpfnWndProc = about_proc;
    wc.lpszClassName = ABOUT_CLASS;
    RegisterClassExA(&wc);
    wc.lpfnWndProc = offsets_proc;
    wc.lpszClassName = OFFSETS_CLASS;
    RegisterClassExA(&wc);

    HWND hwnd = create_fixed_window("SoftwareESSAdapter", "Software ESS Adapter", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                    MAIN_RIGHT_X + 80, MAIN_BOTTOM_Y + 34, nullptr);
    ShowWindow(hwnd, show);

    // Refresh the windows at 60 Hz. SetTimer only ticks every 15.6 ms or so, which lands nowhere
    // near 60, so wait on a high-resolution waitable timer scheduled against the clock instead.
    // Without high-resolution support (before Windows 10 1803) this falls back to a normal one.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const LONGLONG period = frequency.QuadPart / 60;
    LONGLONG next_frame = now.QuadPart + period;
    bool quit = false;
    while (!quit) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart - next_frame > period) {
            next_frame = now.QuadPart;  // fell behind, don't try to catch up
        }
        LARGE_INTEGER due;  // negative = relative, in 100 ns units
        due.QuadPart = -std::max<LONGLONG>(0, (next_frame - now.QuadPart) * 10000000 / frequency.QuadPart);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);

        if (MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0) {
            refresh_main(hwnd);
            if (g_settings_window) {
                refresh_settings(g_settings_window);
            }
            if (g_offsets_window) {
                refresh_offsets(g_offsets_window);
            }
            next_frame += period;
        }

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            // Tab moves between buttons, Space presses one, Escape closes Settings and About.
            HWND active = GetActiveWindow();
            if (active && IsDialogMessageA(active, &msg)) {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    CloseHandle(timer);

    g_running = false;
    bridge.join();
    g_dsu.stop();
    DeleteObject(g_font);
    DeleteObject(g_mono_font);
    DeleteObject(g_title_font);
    DeleteObject(g_bg_brush);
    return 0;
}
