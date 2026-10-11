//main.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Software ESS Adapter: reads a Gamecube adapter, applies the ESS map from this
// repository, and serves the result to Dolphin or Ship of Harkinian as a DSU controller.

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/filename.H>
#include <FL/platform.H>

#ifdef _WIN32
#include <dwmapi.h>
#include <shellapi.h>
#else
#include <FL/Fl_PNG_Image.H>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "delayed_sender.hpp"
#include "dsu_server.hpp"
#include "embedded.hpp"
#include "ess.hpp"
#include "ess_range.hpp"
#include "gc_adapter.hpp"
#include "ini.hpp"
#include "notch.hpp"
#include "resource.h"
#include "stick.hpp"

#ifdef _WIN32
// Newer Windows SDK values that older mingw-w64 headers don't have.
static constexpr DWORD DWM_USE_IMMERSIVE_DARK_MODE = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

// Ship of Harkinian output and input lag are only in dev builds (.uild.ps1 --dev) until SoH's DSU
// input is in an official libultraship release. Without them the program always plays on Wii VC.
#ifdef SOH_DEV
static constexpr bool SOH_FEATURES = true;
#else
static constexpr bool SOH_FEATURES = false;
#endif

enum ControlId {
    ID_CANCEL = 2,  // Escape, or the window's close button
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
    ID_GAME_LABEL,
    ID_GAME_VC,   // "Playing on" choices, drawn as a pair of toggle buttons
    ID_GAME_SOH,
    ID_LAG_LABEL,
    ID_LAG_MINUS,
    ID_LAG_VALUE,
    ID_LAG_PLUS,
    ID_LAG_HINT,
    ID_ACTIVE,  // the main window's status indicator
    ID_PLOTS,   // the settings window's plots and zone legend
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

static const Fl_Color COLOR_BG = fl_rgb_color(32, 32, 32);
static const Fl_Color COLOR_TEXT = fl_rgb_color(230, 230, 230);
static const Fl_Color COLOR_DIM_TEXT = fl_rgb_color(150, 150, 150);
static const Fl_Color COLOR_FACE = fl_rgb_color(55, 55, 55);
static const Fl_Color COLOR_FACE_DOWN = fl_rgb_color(80, 80, 80);
static const Fl_Color COLOR_BORDER = fl_rgb_color(95, 95, 95);
static const Fl_Color COLOR_ACCENT = fl_rgb_color(0, 120, 215);
static const Fl_Color COLOR_PLOT_AXIS = fl_rgb_color(138, 138, 138);
static const Fl_Color COLOR_DOT_IN_RANGE = fl_rgb_color(80, 200, 255);
// The notch to aim for while calibrating: the blue of the held notch in the notch offsets window.
static const Fl_Color COLOR_CALIBRATION_TARGET = COLOR_DOT_IN_RANGE;
// The plots color everything by what OoT does with it, in OotZone order: only ESS in color, the
// movement zones in grays that get darker as Link speeds up.
static const Fl_Color ZONE_COLORS[] = {fl_rgb_color(0x1A, 0x1A, 0x1A), fl_rgb_color(0x3F, 0xBF, 0x6F), fl_rgb_color(0x5C, 0x5C, 0x5C),
                                       fl_rgb_color(0x46, 0x46, 0x46), fl_rgb_color(0x32, 0x32, 0x32)};
static const char* const ZONE_NAMES[] = {"Dead zone", "ESS (turn in place)", "Walk", "Run", "Full speed"};
static const Fl_Color COLOR_ACTIVE = fl_rgb_color(60, 200, 90);
static const Fl_Color COLOR_PAUSED = fl_rgb_color(230, 170, 50);

// The built-in fonts (see load_fonts): Selawik in place of Segoe UI, Cascadia Mono SemiLight in
// place of Consolas, at the sizes the windows were laid out for.
static constexpr Fl_Font FONT_UI = FL_FREE_FONT, FONT_MONO = FL_FREE_FONT + 1, FONT_TITLE = FL_FREE_FONT + 2;
static constexpr Fl_Fontsize FONT_SIZE = 12, TITLE_SIZE = 17;

struct Rect {
    int left, top, right, bottom;
};

// Status indicator in the main window's top right corner: amber "Paused" while the settings
// window is open (the game only gets a neutral controller), green "Active" while controller data
// is going out to the game, gray "Idle" otherwise.
enum class LinkState { Idle, Active, Paused };
static const int MAIN_RIGHT_X = 350;   // the indicator and the About button
static const int MAIN_BOTTOM_Y = 176;  // the Edit settings / About row
static const Rect ACTIVE_RECT = {MAIN_RIGHT_X, 10, MAIN_RIGHT_X + 70, 28};
static LinkState g_shown_state = LinkState::Idle;

// Plots in the settings window: the physical stick, and what OoT receives, both at the same
// scale, and the zone legend beside the game plot.
static constexpr int PLOT_SCALE = 1;                         // pixels per unit, stick or in-game
static constexpr int STICK_PLOT_RANGE = 118;                 // stick units from center to edge; fits a ring at 105 + 12
static constexpr int GAME_PLOT_RANGE = 90;                   // in-game units from center to edge; fits SoH's 85
static constexpr int N64_STRAIGHT = 80, N64_DIAGONAL = 70;   // the N64 stick range, drawn on the game plot
static constexpr int LEGEND_WIDTH = 150;                     // beside the game plot: a row per zone
static constexpr int STICK_PLOT_SIZE = 2 * PLOT_SCALE * STICK_PLOT_RANGE, GAME_PLOT_SIZE = 2 * PLOT_SCALE * GAME_PLOT_RANGE;
static constexpr int PLOTS_TOP = SOH_FEATURES ? 312 : 252;  // under the settings rows (two more in dev builds)
static const Rect STICK_PLOT = {10, PLOTS_TOP, 10 + STICK_PLOT_SIZE, PLOTS_TOP + STICK_PLOT_SIZE};
static const Rect GAME_PLOT = {STICK_PLOT.right + 10, PLOTS_TOP, STICK_PLOT.right + 10 + GAME_PLOT_SIZE, PLOTS_TOP + GAME_PLOT_SIZE};
static const int SETTINGS_BOTTOM_Y = STICK_PLOT.bottom + 12;  // the prompt line and Done
static const int SETTINGS_WIDTH = std::max<int>(548, GAME_PLOT.right + LEGEND_WIDTH + 10);  // fits the settings rows too
// The window class on Windows (which lets a second start find the first) and WM_CLASS on Linux.
static const char* const WINDOW_CLASS = "SoftwareESSAdapter";
static const char* const SOURCE_URL = "https://github.com/TorjeAmundsen/Software-ESS-Adapter";

static DsuServer g_dsu;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_ess[PORT_COUNT] = {true, true, true, true};
static std::atomic<Game> g_game{Game::WiiVC};
// Input lag added on Ship of Harkinian, in milliseconds (see delayed_sender.hpp).
static constexpr int MAX_INPUT_LAG_MS = 100;
static std::atomic<int> g_input_lag{0};
static std::atomic<int> g_snap_distance{DEFAULT_SNAP_DISTANCE};
// ESS range as the user sets it, in in-game units, and the physical stick distances that
// gives (see ess_range.hpp). The distances are recomputed whenever the setting changes.
static std::atomic<int> g_range_start{default_ess_start(Game::WiiVC)};
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
    const int start = physical_start(g_game, g_range_start);
    const int end = std::max(physical_end(g_game, g_range_end), start + 1);
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
// open the game gets a neutral controller instead of real input.
static std::atomic<bool> g_editing{false};
class DarkWindow;
static DarkWindow* g_main_window = nullptr;
static DarkWindow* g_settings_window = nullptr;
static DarkWindow* g_about_window = nullptr;
// Notch offsets window, owned by the settings window so it only exists while editing.
static DarkWindow* g_offsets_window = nullptr;
static int g_offsets_port = 0;
static int g_held_notch = -1;  // the notch the stick is on, highlighted in that window, or -1
static std::atomic<int> g_calibration_step{0};  // 0 to CALIBRATION_STEPS - 1, see notch.hpp

static std::mutex g_mutex;  // guards the seven below
static AdapterFrame g_raw{};
static AdapterFrame g_centered{};    // raw, centered on the origin
static AdapterFrame g_calibrated{};  // after notch warp and snapping, before the ESS range and dead zone
static AdapterFrame g_out{};
// Recent g_out frames with when they were made, so the "In game" plot can lag like the game does.
struct TimedFrame {
    std::chrono::steady_clock::time_point at;
    AdapterFrame frame;
};
static std::deque<TimedFrame> g_out_history;
static std::string g_adapter_status = "Starting";
static std::string g_notice;  // shown in the settings window's prompt line for a few seconds
static std::chrono::steady_clock::time_point g_notice_time;
static std::string g_offsets_notice;  // the same for the notch offsets window
static std::chrono::steady_clock::time_point g_offsets_notice_time;
static NotchCalibration g_notches[PORT_COUNT];

// Settings live next to the program: Software ESS Adapter settings.ini on Windows,
// software-ess-adapter-settings.ini on Linux. The "settings" part keeps it apart from the Dolphin
// profile, dolphin/Software ESS Adapter.ini.
static std::string settings_path() {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string exe = path;
    return exe.substr(0, exe.find_last_of('.')) + " settings.ini";
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).string() + "-settings.ini";
#endif
}

#ifndef _WIN32
// A folder only this user can read, for files the program writes out: XDG_RUNTIME_DIR, or one in
// /tmp named after the user.
static std::filesystem::path user_runtime_dir() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::filesystem::path dir = runtime && *runtime ? std::filesystem::path(runtime) / "software-ess-adapter"
                                                          : std::filesystem::path("/tmp/software-ess-adapter-" + std::to_string(getuid()));
    mkdir(dir.c_str(), 0700);
    return dir;
}
#endif

static std::string port_section(int port) {
    return "Port" + std::to_string(port + 1);
}

// Reads "a,b,c,..." with exactly `count` integers in [min, max].
static bool read_ints(const std::string& path, const std::string& section, const char* key, int* values, int count, int min, int max) {
    const std::string text = ini_read(path, section, key).value_or("");
    const char* cursor = text.c_str();
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
static const char* const KEY_GAME = "Game";  // 0 = Wii VC, 1 = Ship of Harkinian
static const char* const KEY_INPUT_LAG = "InputLagMs";
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
    g_input_lag = std::clamp<int>(g_input_lag, 0, MAX_INPUT_LAG_MS);
    g_range_end = std::clamp<int>(g_range_end, 2, max_in_game_end(g_game));
    g_range_start = std::clamp<int>(g_range_start, 1, g_range_end - 1);
    g_dead_zone = std::clamp<int>(g_dead_zone, 0, g_range_start - 1);
    update_range_distances();
    g_dead_zone_distance = physical_dead_zone(g_game, g_dead_zone);
}

static void save_settings() {
    const std::string path = settings_path();
    auto write = [&](const char* key, int value) {
        ini_write(path, SETTINGS_SECTION, key, std::to_string(value));
    };
    if (SOH_FEATURES) {
        write(KEY_GAME, g_game == Game::SoH ? 1 : 0);
        write(KEY_INPUT_LAG, g_input_lag);
    }
    write(KEY_SNAP_DISTANCE, g_snap_distance);
    write(KEY_RANGE_START, g_range_start);
    write(KEY_RANGE_END, g_range_end);
    write(KEY_DEAD_ZONE, g_dead_zone);
}

static void load_settings() {
    const std::string path = settings_path();
    auto read = [&](const char* key, int fallback) {
        return ini_read_int(path, SETTINGS_SECTION, key, fallback);
    };
    // A settings file from a dev build may still say SoH; other builds leave it alone and play on VC.
    g_game = SOH_FEATURES && read(KEY_GAME, 0) == 1 ? Game::SoH : Game::WiiVC;
    g_input_lag = SOH_FEATURES ? read(KEY_INPUT_LAG, 0) : 0;
    g_snap_distance = read(KEY_SNAP_DISTANCE, DEFAULT_SNAP_DISTANCE);
    g_range_start = read(KEY_RANGE_START, default_ess_start(g_game));
    g_range_end = read(KEY_RANGE_END, ESS_IN_GAME_END);
    g_dead_zone = read(KEY_DEAD_ZONE, DEAD_ZONE_DEFAULT);
    apply_settings();

    for (int p = 0; p < PORT_COUNT; ++p) {
        g_ess[p] = ini_read_int(path, port_section(p), KEY_ESS, 1) != 0;

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
    ini_write(settings_path(), port_section(port), KEY_ESS, g_ess[port] ? "1" : "0");
}

static void clear_notches(int port) {
    const std::string path = settings_path();
    ini_write(path, port_section(port), KEY_NEUTRAL, std::nullopt);
    ini_write(path, port_section(port), KEY_NOTCHES, std::nullopt);
    ini_write(path, port_section(port), KEY_NOTCH_OFFSETS, std::nullopt);
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
    const int snap = g_snap_distance, start = g_range_start, end = g_range_end, dead_zone = g_dead_zone, lag = g_input_lag;
    switch (id) {
    case ID_LAG_MINUS:
    case ID_LAG_PLUS:
        if (g_game != Game::SoH) {
            return "Input lag only applies to Ship of Harkinian, to match how Dolphin feels.";
        }
        if (id == ID_LAG_MINUS) {
            return lag == 0 ? "Input lag is already off." : nullptr;
        }
        if (lag == MAX_INPUT_LAG_MS) {
            static const std::string reason = "Input lag goes up to " + std::to_string(MAX_INPUT_LAG_MS) + " ms.";
            return reason.c_str();
        }
        return nullptr;
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
        if (end == max_in_game_end(g_game)) {
            static const std::string reason =
                "The ESS range end goes up to " + std::to_string(ESS_IN_GAME_MAX_END) + ", as on ElectroModder's adapter.";
            return reason.c_str();
        }
        return nullptr;
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
                                   {ID_DEAD_ZONE_MINUS, ID_DEAD_ZONE_PLUS, &g_dead_zone},
                                   {ID_LAG_MINUS, ID_LAG_PLUS, &g_input_lag}};

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
    ini_write(settings_path(), port_section(port), KEY_NOTCH_OFFSETS, join_pairs(calibration.offset));
}

static void save_notches(int port, const NotchCalibration& calibration) {
    const std::string path = settings_path();
    ini_write(path, port_section(port), KEY_NEUTRAL, join_pairs(calibration.neutral_samples));
    ini_write(path, port_section(port), KEY_NOTCHES, join_pairs(calibration.notch));
}

static void set_adapter_status(const std::string& status) {
    std::lock_guard lock(g_mutex);
    g_adapter_status = status;
}

// The program the controller goes to, for status lines and prompts.
static const char* client_name() {
    return g_game == Game::SoH ? "Ship of Harkinian" : "Dolphin";
}

// The input lag to add right now: only on Ship of Harkinian.
static std::chrono::microseconds input_lag() {
    return std::chrono::milliseconds(g_game == Game::SoH ? g_input_lag.load() : 0);
}

// What the game has right now: g_out from the input lag ago, or the oldest kept if the lag is
// longer than the history so far. Call with g_mutex held.
static AdapterFrame game_has_now() {
    const auto lag = input_lag();
    if (lag.count() == 0 || g_out_history.empty()) {
        return g_out;
    }
    const auto due = std::chrono::steady_clock::now() - lag;
    for (auto it = g_out_history.rbegin(); it != g_out_history.rend(); ++it) {
        if (it->at <= due) {
            return it->frame;
        }
    }
    return g_out_history.front().frame;
}

// The stick settings for a port, as they are right now.
static StickSettings stick_settings(int port) {
    const RangeDistances range = range_distances();
    StickSettings settings;
    settings.game = g_game;
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
    // Everything goes out through here, so input lag delays it all, disconnects included.
    DelayedSender sender([](const AdapterFrame& frame, Game game) { g_dsu.publish(frame, game); });

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
        sender.push(AdapterFrame{}, g_game, input_lag());  // reports every port as disconnected
        std::lock_guard lock(g_mutex);
        g_raw = g_centered = g_calibrated = g_out = AdapterFrame{};
        g_out_history.clear();
    };
    auto wait = [&](int ms) {
        for (int waited = 0; waited < ms && g_running; waited += 100) {
            take_calibration_request();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        sender.push(sent, g_game, input_lag());

        std::lock_guard lock(g_mutex);
        g_raw = raw;
        g_centered = centered;
        g_calibrated = calibrated;
        g_out = out;
        const auto now = std::chrono::steady_clock::now();
        g_out_history.push_back({now, out});
        while (g_out_history.front().at < now - std::chrono::milliseconds(MAX_INPUT_LAG_MS + 50)) {
            g_out_history.pop_front();
        }
        g_adapter_status = "Adapter connected";
    }
}

// Set once Tab or an arrow key moves between buttons. Until then, like standard Windows buttons,
// no button shows keyboard focus or takes Space.
static bool g_keyboard_used = false;

static void fill(const Rect& rect, Fl_Color color) {
    fl_rectf(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, color);
}

static void frame(const Rect& rect, Fl_Color color) {
    fl_rect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, color);
}

// Lines `width` pixels wide with round ends, as GDI pens draw them. Reset with fl_line_style(0).
static void pen(Fl_Color color, int width) {
    fl_color(color);
    fl_line_style(width > 1 ? FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND : FL_SOLID, width > 1 ? width : 0);
}

// The windows were laid out for Segoe UI and Consolas. Text lines up with the built-in fonts
// using those fonts' measurements (as Windows reports them): how far the baseline sits below the
// top of a line, and how far apart lines are.
struct FontLayout {
    int ascent, line_height;
};

static FontLayout font_layout(Fl_Font font) {
    if (font == FONT_MONO) {
        return {11, 14};  // Consolas, 12 px
    }
    if (font == FONT_TITLE) {
        return {18, 23};  // Segoe UI Semibold, 17 px
    }
    return {12, 15};  // Segoe UI, 12 px
}

// One line of text in the current font, with its top left corner at x, y.
static void draw_text_at(const std::string& text, int x, int y) {
    fl_draw(text.c_str(), x, y + font_layout(fl_font()).ascent);
}

// One line of text in the current font, centered vertically in the rect (and horizontally too if
// `centered`), as Windows centers a single line.
static void draw_text_in(const std::string& text, const Rect& rect, bool centered) {
    const int width = static_cast<int>(fl_width(text.c_str()));
    const int x = centered ? rect.left + (rect.right - rect.left - width) / 2 : rect.left;
    draw_text_at(text, x, rect.top + (rect.bottom - rect.top - font_layout(fl_font()).line_height) / 2);
}

// Breaks text into lines no wider than `width` in the current font, at spaces and newlines, as
// Windows wraps static text.
static std::vector<std::string> wrap(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find_first_of(" \n", start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string word = text.substr(start, end - start);
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && fl_width(longer.c_str()) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = longer;
        }
        if (end == text.size() || text[end] == '\n') {
            lines.push_back(line);
            line.clear();
        }
        start = end + 1;
    }
    return lines;
}

// Every window: a fixed size, the dark background, and controls found by their ControlId.
class DarkWindow : public Fl_Double_Window {
public:
    using Command = void (*)(DarkWindow*, int);

    // `destroyed` runs when the window closes, to forget it.
    DarkWindow(int width, int height, const std::string& title, Command command, void (*destroyed)())
        : Fl_Double_Window(width, height), command_(command), destroyed_(destroyed) {
        copy_label(title.c_str());
        box(FL_FLAT_BOX);
        color(COLOR_BG);
        resizable(nullptr);
        end();
        // Escape and the close button.
        callback([](Fl_Widget* window, void*) {
            static_cast<DarkWindow*>(window)->command(ID_CANCEL);
        });
    }

    void command(int id) {
        command_(this, id);
    }

    // The control with this id, or nullptr if there is none (like one only dev builds have).
    Fl_Widget* control(int id) const {
        for (int i = 0; i < children(); ++i) {
            if (child(i)->argument() == id) {
                return child(i);
            }
        }
        return nullptr;
    }

    void close() {
        if (auto destroyed = std::exchange(destroyed_, nullptr)) {
            destroyed();
        }
        hide();
        Fl::delete_widget(this);
    }

private:
    Command command_;
    void (*destroyed_)();
};

static void redraw_control(DarkWindow* window, int id) {
    if (Fl_Widget* control = window->control(id)) {
        control->redraw();
    }
}

static void set_text(DarkWindow* window, int id, const std::string& text) {
    Fl_Widget* control = window->control(id);
    if (!control) {
        return;  // a control only dev builds have
    }
    if (text != (control->label() ? control->label() : "")) {
        control->copy_label(text.c_str());
        control->redraw();
    }
}

// Static text, wrapped to its box. Hints use the dim color, and the held notch's name and Game
// value in the notch offsets window light up.
class Text : public Fl_Widget {
public:
    Text(int x, int y, int w, int h, const char* text, int id, bool centered, bool dim)
        : Fl_Widget(x, y, w, h), centered_(centered), dim_(dim) {
        copy_label(text);
        argument(id);
        labelfont(FONT_UI);
        labelsize(FONT_SIZE);
    }

    void draw() override {
        const int id = static_cast<int>(argument());
        const bool held = g_held_notch >= 0 && (id == ID_OFFSET_NAME + g_held_notch || id == ID_OFFSET_GAME + g_held_notch);
        fl_push_clip(x(), y(), w(), h());
        fl_rectf(x(), y(), w(), h(), COLOR_BG);
        fl_font(labelfont(), labelsize());
        fl_color(held ? COLOR_DOT_IN_RANGE : dim_ ? COLOR_DIM_TEXT : COLOR_TEXT);
        int top = y();
        for (const std::string& line : wrap(label(), w())) {
            const int left = centered_ ? x() + (w() - static_cast<int>(fl_width(line.c_str()))) / 2 : x();
            draw_text_at(line, left, top);
            top += font_layout(labelfont()).line_height;
        }
        fl_pop_clip();
    }

private:
    bool centered_, dim_;
};

// The push buttons, the ESS checkboxes, and the "Playing on" toggles, all in dark colors.
class Button : public Fl_Widget {
public:
    Button(int x, int y, int w, int h, const char* text, int id) : Fl_Widget(x, y, w, h) {
        copy_label(text);
        argument(id);
        labelfont(FONT_UI);
        labelsize(FONT_SIZE);
        callback([](Fl_Widget* button, void*) {
            static_cast<DarkWindow*>(button->window())->command(static_cast<int>(button->argument()));
        });
    }

    int handle(int event) override {
        switch (event) {
        case FL_PUSH:
            if (Fl::event_button() != FL_LEFT_MOUSE) {
                return 0;
            }
            pressed_ = true;
            take_focus();
            redraw();
            return 1;
        case FL_DRAG:
            if ((Fl::event_inside(this) != 0) != pressed_) {
                pressed_ = !pressed_;
                redraw();
            }
            return 1;
        case FL_RELEASE:
            if (pressed_) {
                pressed_ = false;
                redraw();
                do_callback();  // last: the command may close the window
            }
            return 1;
        case FL_FOCUS:
            if (!g_keyboard_used) {
                return 0;
            }
            redraw();
            return 1;
        case FL_UNFOCUS:
            redraw();
            return 1;
        case FL_KEYBOARD:
            if (Fl::event_key() == ' ' && g_keyboard_used) {
                do_callback();
                return 1;
            }
            return 0;
        }
        return 0;
    }

    void draw() override {
        const Rect rect = {x(), y(), x() + w(), y() + h()};
        const int id = static_cast<int>(argument());
        fl_font(labelfont(), labelsize());
        fill(rect, COLOR_BG);
        Fl_Color text_color = COLOR_TEXT;
        bool centered = true;
        Rect text_rect = rect;

        if (id >= ID_ESS_CHECK && id < ID_ESS_CHECK + PORT_COUNT) {
            const int size = 14;
            const int top = (rect.top + rect.bottom - size) / 2;
            const Rect box = {rect.left, top, rect.left + size, top + size};
            const bool checked = g_ess[id - ID_ESS_CHECK];
            fill(box, checked ? COLOR_ACCENT : COLOR_FACE);
            frame(box, checked ? COLOR_ACCENT : COLOR_BORDER);
            if (checked) {
                pen(fl_rgb_color(255, 255, 255), 2);
                fl_line(box.left + 3, top + 7, box.left + 6, top + 10, box.left + 11, top + 4);
                fl_line_style(0);
            }
            text_rect.left += size + 6;
            centered = false;
        } else if (id == ID_GAME_VC || id == ID_GAME_SOH) {
            // The chosen game is filled in, like a checked ESS box.
            const bool chosen = (id == ID_GAME_SOH) == (g_game == Game::SoH);
            fill(rect, chosen ? COLOR_ACCENT : pressed_ ? COLOR_FACE_DOWN : COLOR_FACE);
            frame(rect, chosen ? COLOR_ACCENT : COLOR_BORDER);
        } else {
            fill(rect, pressed_ ? COLOR_FACE_DOWN : COLOR_FACE);
            frame(rect, COLOR_BORDER);
            if (limit_reason(id)) {
                text_color = COLOR_DIM_TEXT;  // a -/+ button that can't go further
            }
        }
        fl_color(text_color);
        if (std::strcmp(label(), "-") == 0) {
            // Selawik's hyphen lands between two pixel rows and looks dimmed, like a button at its
            // limit, so draw the minus as Segoe UI did: 5 pixels wide, 1 high.
            fl_xyline(rect.left + w() / 2 - 3, rect.top + h() / 2, rect.left + w() / 2 + 1);
        } else {
            draw_text_in(label(), text_rect, centered);
        }
        // Keyboard focus (Tab), shown only once the keyboard is used, like standard Windows buttons.
        if (Fl::focus() == this && g_keyboard_used) {
            frame(rect, COLOR_ACCENT);
        }
    }

private:
    bool pressed_ = false;
};

// A widget that draws with a function, for the status indicator and the plots.
class Canvas : public Fl_Widget {
public:
    Canvas(const Rect& rect, int id, void (*paint)()) : Fl_Widget(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top), paint_(paint) {
        argument(id);
    }

    void draw() override {
        fl_push_clip(x(), y(), w(), h());
        paint_();
        fl_pop_clip();
    }

private:
    void (*paint_)();
};

static Text* add_text(DarkWindow* window, const char* text, int x, int y, int w, int h, int id, bool centered = false) {
    auto* control = new Text(x, y, w, h, text, id, centered, false);
    window->add(control);
    return control;
}

// Hint text, in the dim color.
static Text* add_dim_text(DarkWindow* window, const char* text, int x, int y, int w, int h, int id) {
    auto* control = new Text(x, y, w, h, text, id, false, true);
    window->add(control);
    return control;
}

static Button* add_button(DarkWindow* window, const char* text, int x, int y, int w, int h, int id) {
    auto* control = new Button(x, y, w, h, text, id);
    window->add(control);
    return control;
}

// Main window: connection status only, no controller values.
static void refresh_main(DarkWindow* window) {
    AdapterFrame raw;
    std::string status;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        status = g_adapter_status;
    }
    set_text(window, ID_ADAPTER_STATUS, status);
    if (g_dsu.last_error().empty()) {
        const std::string name = client_name();
        std::string text = g_dsu.client_count() > 0 ? name + " connected." : "Waiting for " + name + " to connect.";
        if (input_lag().count() > 0) {
            text += " Input lag " + std::to_string(g_input_lag) + " ms.";
        }
        set_text(window, ID_DSU_STATUS, text);
    }
    for (int p = 0; p < PORT_COUNT; ++p) {
        set_text(window, ID_MAIN_PORT + p,
                 "Port " + std::to_string(p + 1) + ": " + (raw[p].connected ? "controller connected" : "no controller"));
    }
    set_text(window, ID_EDIT, g_settings_window ? "Editing settings" : "Edit settings");

    const LinkState state = g_editing ? LinkState::Paused : g_dsu.is_transmitting() ? LinkState::Active : LinkState::Idle;
    if (state != g_shown_state) {
        g_shown_state = state;
        redraw_control(window, ID_ACTIVE);
    }
}

static void paint_active_indicator() {
    fill(ACTIVE_RECT, COLOR_BG);

    const Fl_Color color = g_shown_state == LinkState::Active   ? COLOR_ACTIVE
                           : g_shown_state == LinkState::Paused ? COLOR_PAUSED
                                                                : COLOR_DIM_TEXT;
    const char* text = g_shown_state == LinkState::Active ? "Active" : g_shown_state == LinkState::Paused ? "Paused" : "Idle";
    const int dot = 10;
    const int top = (ACTIVE_RECT.top + ACTIVE_RECT.bottom - dot) / 2;
    fl_color(color);
    fl_pie(ACTIVE_RECT.left, top, dot, dot, 0, 360);

    Rect label = ACTIVE_RECT;
    label.left += dot + 6;
    fl_font(FONT_UI, FONT_SIZE);
    draw_text_in(text, label, false);
}

// The port whose zone map the stick plot shows: the first one with a controller.
static int map_port(const AdapterFrame& raw) {
    for (int p = 0; p < PORT_COUNT; ++p) {
        if (raw[p].connected) {
            return p;
        }
    }
    return 0;
}

static void refresh_settings(DarkWindow* window) {
    AdapterFrame raw, centered, out;
    bool calibrated[PORT_COUNT];
    std::string notice;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        centered = g_centered;
        out = game_has_now();  // the Game readout lags like the game
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
            // sees after Wii VC or SoH. Fixed-width fields in a monospace font so nothing shifts.
            int in_game[2];
            game_in_game(g_game, out[p].stick[0], out[p].stick[1], in_game);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "Stick %4d,%4d  Game %3d,%3d", centered[p].stick[0] - 128, centered[p].stick[1] - 128,
                          in_game[0], in_game[1]);
            text = buffer;
        }
        set_text(window, ID_PORT_TEXT + p, text);
        // A calibrated port shows Notch offsets and Clear in place of the text, except while it calibrates.
        set_text(window, ID_NOTCH_TEXT + p, calibrating == p ? "Calibrating" : calibrated[p] ? "" : "Not calibrated");
        const bool show_buttons = calibrated[p] && calibrating != p;
        for (const int id : {ID_NOTCH_OFFSETS + p, ID_CLEAR_NOTCHES + p}) {
            Fl_Widget* button = window->control(id);
            if ((button->visible() != 0) != show_buttons) {
                if (show_buttons) {
                    button->show();
                } else {
                    button->hide();
                }
            }
        }
        set_text(window, ID_CALIBRATE + p, calibrating == p ? "Cancel" : "Calibrate");
    }

    set_text(window, ID_SNAP_VALUE, std::to_string(g_snap_distance));
    set_text(window, ID_RANGE_START_VALUE, std::to_string(g_range_start));
    set_text(window, ID_RANGE_END_VALUE, std::to_string(g_range_end));
    set_text(window, ID_DEAD_ZONE_VALUE, std::to_string(g_dead_zone));
    set_text(window, ID_LAG_VALUE, std::to_string(g_input_lag));
    // Repaint a -/+ button when it reaches or leaves its limit (it looks dimmed there).
    static bool shown_at_limit[std::size(STEPPERS) * 2] = {};
    size_t button = 0;
    for (const Stepper& stepper : STEPPERS) {
        for (const int id : {stepper.minus, stepper.plus}) {
            const bool at_limit = limit_reason(id) != nullptr;
            Fl_Widget* control = window->control(id);  // null for a button only dev builds have
            if (control && at_limit != shown_at_limit[button]) {
                shown_at_limit[button] = at_limit;
                control->redraw();
            }
            ++button;
        }
    }

    set_text(window, ID_STICK_PLOT_TITLE, "Your stick, port " + std::to_string(map_port(raw) + 1));
    set_text(window, ID_GAME_PLOT_TITLE, g_game == Game::SoH ? "In game (OoT on Ship of Harkinian)" : "In game (OoT on Wii VC)");
    std::string prompt = notice.empty() ? std::string(client_name()) + " gets a neutral controller until you close this window." : notice;
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
    set_text(window, ID_PROMPT, prompt);
    redraw_control(window, ID_PLOTS);
}

// One plot square in the window, with unit 0 at its center.
struct Plot {
    int x, y, w, h;
    int px(double ux) const {
        return x + w / 2 + static_cast<int>(std::lround(ux * PLOT_SCALE));
    }
    int py(double uy) const {
        return y + h / 2 - static_cast<int>(std::lround(uy * PLOT_SCALE));
    }
};

// Draws one plot (and the legend beside the game plot). `beside` more columns follow to the
// plot's right. The window is double buffered, so the 60 Hz redraw doesn't flicker.
template <typename Draw>
static void draw_plot(const Rect& rect, int beside, Draw draw) {
    const Plot plot{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
    fl_push_clip(plot.x, plot.y, plot.w + beside, plot.h);
    fl_font(FONT_UI, FONT_SIZE);
    fill({plot.x + plot.w, plot.y, plot.x + plot.w + beside, plot.y + plot.h}, COLOR_BG);

    draw(plot);

    fl_line_style(0);
    frame({plot.x, plot.y, plot.x + plot.w, plot.y + plot.h}, COLOR_BORDER);
    fl_pop_clip();
}

// A plot's background: every unit colored by its zone, PLOT_SCALE pixels square, with unit 0
// under the axes. zone_at(x, y) gives the zone for a unit.
struct ZoneImage {
    std::vector<int> key;  // what it was made from, to know when to make it again
    int w = 0, h = 0;
    std::vector<uchar> pixels;  // top-down, RGB
};

template <typename ZoneAt>
static void make_zone_image(ZoneImage& image, int w, int h, int range, ZoneAt zone_at) {
    uchar colors[std::size(ZONE_COLORS)][3];
    for (size_t i = 0; i < std::size(ZONE_COLORS); ++i) {
        Fl::get_color(ZONE_COLORS[i], colors[i][0], colors[i][1], colors[i][2]);
    }
    const int span = 2 * range + 1;
    std::vector<int> units(span * span);
    for (int x = -range; x <= range; ++x) {
        for (int y = -range; y <= range; ++y) {
            units[(x + range) * span + (y + range)] = static_cast<int>(zone_at(x, y));
        }
    }
    // Pixel column c shows the unit whose center Plot::px puts nearest to it, counting from w/2
    // (likewise for rows, upward from h/2).
    auto unit = [range](int offset) {
        const int u = static_cast<int>(std::floor((offset + PLOT_SCALE / 2.0) / PLOT_SCALE));
        return std::clamp(u, -range, range) + range;
    };
    image.w = w;
    image.h = h;
    image.pixels.resize(static_cast<size_t>(w) * h * 3);
    for (int r = 0; r < h; ++r) {
        const int y = unit(h / 2 - r);
        for (int c = 0; c < w; ++c) {
            const uchar* color = colors[units[unit(c - w / 2) * span + y]];
            std::copy(color, color + 3, &image.pixels[(static_cast<size_t>(r) * w + c) * 3]);
        }
    }
}

static void draw_zone_image(const Plot& plot, const ZoneImage& image) {
    fl_draw_image(image.pixels.data(), plot.x, plot.y, image.w, image.h, 3);
}

static void draw_axes(const Plot& plot) {
    pen(COLOR_PLOT_AXIS, 1);
    fl_line(plot.x, plot.y + plot.h / 2, plot.x + plot.w - 1, plot.y + plot.h / 2);
    fl_line(plot.x + plot.w / 2, plot.y, plot.x + plot.w / 2, plot.y + plot.h - 1);
}

// Octagon through (0, straight) and (corner, corner) in each direction.
static void draw_gate(const Plot& plot, double straight, double corner, Fl_Color color) {
    const double points[NOTCH_COUNT][2] = {{0, straight},  {corner, corner},   {straight, 0},  {corner, -corner},
                                           {0, -straight}, {-corner, -corner}, {-straight, 0}, {-corner, corner}};
    pen(color, 1);
    fl_begin_loop();
    for (const auto& point : points) {
        fl_vertex(plot.px(point[0]), plot.py(point[1]));
    }
    fl_end_loop();
}

// A ring that reads on every zone color (dark outside, white inside, as in pyESS), and the port
// number with a dark shadow.
static void draw_dot(const Plot& plot, int port, double x, double y) {
    const int cx = plot.px(x), cy = plot.py(y), r = 5;
    for (const auto& [color, width] : {std::pair<Fl_Color, int>{fl_rgb_color(0, 0, 0), 3}, {fl_rgb_color(255, 255, 255), 1}}) {
        pen(color, width);
        fl_arc(cx - r, cy - r, 2 * r + 1, 2 * r + 1, 0, 360);
    }
    fl_line_style(0);
    const std::string label = std::to_string(port + 1);
    fl_color(fl_rgb_color(0, 0, 0));
    draw_text_at(label, cx + 9, cy - 17);
    fl_color(fl_rgb_color(255, 255, 255));
    draw_text_at(label, cx + 8, cy - 18);
}

// The calibration target ring, centered on cx, cy: an antialiased ring 2 pixels wide over a soft
// dark halo, so it reads on every zone color, the ESS green at the center included. Unlike the
// plots, which show the unit grid pixel by pixel, it's smooth: an image with an alpha channel,
// since the line drawing antialiases on Linux but not on Windows.
static void draw_target_ring(int cx, int cy, int radius) {
    constexpr int SPREAD = 4;          // pixels the halo fades over, each side of the ring
    constexpr double DARKEST = 0.75;   // halo opacity right under the ring
    constexpr double HALF_WIDTH = 1;   // the ring's half width, in pixels
    constexpr int SAMPLES = 4;         // per pixel and axis, for the ring's coverage
    static std::unique_ptr<Fl_RGB_Image> image;
    static std::vector<uchar> pixels;  // the image's RGBA, kept alive with it
    const int size = 2 * (radius + SPREAD) + 1;
    if (!image || image->w() != size) {
        uchar ring[3];
        Fl::get_color(COLOR_CALIBRATION_TARGET, ring[0], ring[1], ring[2]);
        pixels.assign(static_cast<size_t>(size) * size * 4, 0);
        const int center = radius + SPREAD;
        auto off_ring = [&](double x, double y) {
            return std::abs(std::hypot(x - center, y - center) - radius);
        };
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                // How much of the pixel the ring covers, and the halo's opacity at its center.
                int inside = 0;
                for (int sy = 0; sy < SAMPLES; ++sy) {
                    for (int sx = 0; sx < SAMPLES; ++sx) {
                        inside += off_ring(x + (sx + 0.5) / SAMPLES - 0.5, y + (sy + 0.5) / SAMPLES - 0.5) <= HALF_WIDTH;
                    }
                }
                const double coverage = static_cast<double>(inside) / (SAMPLES * SAMPLES);
                const double t = std::clamp(1.0 - off_ring(x, y) / SPREAD, 0.0, 1.0);
                const double halo = DARKEST * t * t * (3 - 2 * t);
                // The ring over the black halo: the halo only darkens, so the color is the ring's,
                // scaled by how much of what shows is ring.
                const double alpha = coverage + halo * (1 - coverage);
                uchar* pixel = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    pixel[c] = alpha > 0 ? static_cast<uchar>(std::lround(ring[c] * coverage / alpha)) : 0;
                }
                pixel[3] = static_cast<uchar>(std::lround(255 * alpha));
            }
        }
        image = std::make_unique<Fl_RGB_Image>(pixels.data(), size, size, 4);
    }
    image->draw(cx - radius - SPREAD, cy - radius - SPREAD);
}

// The zone legend, beside the game plot: a swatch and a name per zone.
static void draw_legend(const Plot& plot) {
    for (size_t i = 0; i < std::size(ZONE_NAMES); ++i) {
        const int x = plot.x + plot.w + 12, y = plot.y + 20 * static_cast<int>(i);
        fill({x, y + 2, x + 14, y + 16}, ZONE_COLORS[i]);
        frame({x, y + 2, x + 14, y + 16}, COLOR_BORDER);
        fl_color(COLOR_TEXT);
        draw_text_at(ZONE_NAMES[i], x + 22, y);
    }
}

// The settings a zone map is made from, as a key.
static std::vector<int> zone_map_key(int port, const StickSettings& settings) {
    return {port, static_cast<int>(settings.game), settings.ess, settings.range_start, settings.range_end, settings.dead_zone_distance};
}

static void paint_plots() {
    AdapterFrame raw, centered, calibrated;
    {
        std::lock_guard lock(g_mutex);
        raw = g_raw;
        centered = g_centered;
        calibrated = g_calibrated;
    }
    const int port = map_port(raw);
    const StickSettings settings = stick_settings(port);
    fill({STICK_PLOT.right, STICK_PLOT.top, GAME_PLOT.left, STICK_PLOT.bottom}, COLOR_BG);  // between the plots

    // Your stick, after notch calibration, so the ideal gate and the zones line up with it: every
    // stick position colored by what it does in game, through the rest of the path (ESS range,
    // dead zone, ESS map, the pass that keeps axes moving with their input, and the game). Made
    // again only when a setting changes.
    static ZoneImage stick_zones;
    if (const std::vector<int> key = zone_map_key(port, settings); key != stick_zones.key) {
        make_zone_image(stick_zones, STICK_PLOT_SIZE, STICK_PLOT_SIZE, STICK_PLOT_RANGE, [&](int x, int y) {
            uint8_t s[2] = {static_cast<uint8_t>(std::clamp(128 + x, 0, 255)), static_cast<uint8_t>(std::clamp(128 + y, 0, 255))};
            process_stick(s, NotchCalibration{}, settings);  // already calibrated: no warp, no snapping
            int in_game[2];
            game_in_game(settings.game, s[0], s[1], in_game);
            return oot_zone(in_game[0], in_game[1]);
        });
        stick_zones.key = key;
    }
    draw_plot(STICK_PLOT, 0, [&](const Plot& plot) {
        draw_zone_image(plot, stick_zones);
        draw_axes(plot);
        draw_gate(plot, GATE_STRAIGHT, GATE_DIAGONAL, COLOR_TEXT);

        // While calibrating, a ring around where the stick should go for the current step:
        // the ideal notch, or the center for a neutral step.
        if (g_calibrating_port >= 0) {
            const int step = g_calibration_step;
            const int8_t* goal = is_neutral_step(step) ? nullptr : IDEAL_NOTCHES[notch_of_step(step)];
            const int goal_x = goal ? goal[0] : 0, goal_y = goal ? goal[1] : 0;
            draw_target_ring(plot.px(goal_x), plot.py(goal_y), 12 * PLOT_SCALE);
        }

        // A port that is calibrating shows its raw stick instead, to match the blue target ring.
        for (int p = 0; p < PORT_COUNT; ++p) {
            if (raw[p].connected) {
                const GCPortState& shown = p == g_calibrating_port ? centered[p] : calibrated[p];
                draw_dot(plot, p, shown.stick[0] - 128, shown.stick[1] - 128);
            }
        }
    });

    // In game: every in-game value colored by its zone, the N64's range, and where each port is.
    static ZoneImage game_zones;
    if (game_zones.pixels.empty()) {
        make_zone_image(game_zones, GAME_PLOT_SIZE, GAME_PLOT_SIZE, GAME_PLOT_RANGE, [](int x, int y) {
            return oot_zone(x, y);
        });
    }
    AdapterFrame out;
    {
        std::lock_guard lock(g_mutex);
        out = game_has_now();  // with input lag, the dots lag behind the stick like the game does
    }
    draw_plot(GAME_PLOT, LEGEND_WIDTH, [&](const Plot& plot) {
        draw_zone_image(plot, game_zones);
        draw_axes(plot);
        draw_gate(plot, N64_STRAIGHT, N64_DIAGONAL, COLOR_TEXT);
        for (int p = 0; p < PORT_COUNT; ++p) {
            if (out[p].connected) {
                int in_game[2];
                game_in_game(g_game, out[p].stick[0], out[p].stick[1], in_game);
                draw_dot(plot, p, in_game[0], in_game[1]);
            }
        }
        draw_legend(plot);
    });
}

// Asks a yes/no question over the window.
static bool ask(DarkWindow* window, const std::string& question) {
#ifdef _WIN32
    return MessageBoxA(fl_win32_xid(window), question.c_str(), "Software ESS Adapter", MB_YESNO | MB_ICONQUESTION) == IDYES;
#else
    (void)window;
    fl_message_title("Software ESS Adapter");
    return fl_choice("%s", "No", "Yes", nullptr, question.c_str()) == 1;
#endif
}

static void open_offsets(DarkWindow* owner, int port);

static void on_settings_command(DarkWindow* window, int id) {
    if (id == ID_DONE || id == ID_CANCEL) {
        window->close();
        return;
    }
    if (const char* reason = limit_reason(id)) {
        set_notice(reason);
        refresh_settings(window);
        return;
    }
    for (const Stepper& stepper : STEPPERS) {
        if (id == stepper.minus || id == stepper.plus) {
            *stepper.value += id == stepper.plus ? 1 : -1;
            apply_settings();
            save_settings();
            refresh_settings(window);
            return;
        }
    }
    if (id >= ID_ESS_CHECK && id < ID_ESS_CHECK + PORT_COUNT) {
        const int port = id - ID_ESS_CHECK;
        g_ess[port] = !g_ess[port];
        save_ess(port);
        redraw_control(window, id);
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
        if (ask(window, "Clear the notch calibration and notch offsets for port " + std::to_string(port + 1) + "?")) {
            if (g_offsets_window && g_offsets_port == port) {
                g_offsets_window->close();
            }
            {
                std::lock_guard lock(g_mutex);
                g_notches[port] = NotchCalibration{};
            }
            clear_notches(port);
            set_notice("Port " + std::to_string(port + 1) + " notch calibration and notch offsets cleared.");
        }
    } else if (id >= ID_NOTCH_OFFSETS && id < ID_NOTCH_OFFSETS + PORT_COUNT) {
        open_offsets(window, id - ID_NOTCH_OFFSETS);
    } else if (id == ID_GAME_VC || id == ID_GAME_SOH) {
        g_game = id == ID_GAME_SOH ? Game::SoH : Game::WiiVC;
        apply_settings();
        save_settings();
        redraw_control(window, ID_GAME_VC);
        redraw_control(window, ID_GAME_SOH);
    } else if (id == ID_RANGE_DEFAULT) {
        g_range_start = default_ess_start(g_game);
        g_range_end = ESS_IN_GAME_END;
        g_dead_zone = DEAD_ZONE_DEFAULT;
        apply_settings();
        save_settings();
    }
    refresh_settings(window);
}

static void open_settings() {
    if (g_settings_window) {
        g_settings_window->show();  // brings it to the front
        return;
    }
    g_editing = true;
    DarkWindow* window = new DarkWindow(SETTINGS_WIDTH, SETTINGS_BOTTOM_Y + 34, "Software ESS Adapter settings", on_settings_command, [] {
        // Closing the settings window ends editing: real input goes back to the game.
        if (g_offsets_window) {
            g_offsets_window->close();
        }
        g_calibration_request = -1;
        g_editing = false;
        g_settings_window = nullptr;
    });
    g_settings_window = window;
    for (int p = 0; p < PORT_COUNT; ++p) {
        const int y = 10 + p * 30;
        const std::string label = "Port " + std::to_string(p + 1) + " ESS";
        add_button(window, label.c_str(), 10, y, 85, 22, ID_ESS_CHECK + p);
        add_button(window, "Calibrate", 100, y, 75, 22, ID_CALIBRATE + p);
        add_text(window, "", 185, y + 3, 212, 18, ID_PORT_TEXT + p)->labelfont(FONT_MONO);
        add_text(window, "", 404, y + 3, 134, 18, ID_NOTCH_TEXT + p);
        add_button(window, "Notch offsets", 404, y, 86, 22, ID_NOTCH_OFFSETS + p)->hide();
        add_button(window, "Clear", 494, y, 44, 22, ID_CLEAR_NOTCHES + p)->hide();
    }
    {
        // Settings as a table: label, -/+ controls, short hint. Every row's controls start at
        // the same x, and every hint too.
        const int label_x = 10, control_x = 145, hint_x = 420;
        auto stepper = [&](int x, int y, int minus, int value, int plus) {
            add_button(window, "-", x, y, 24, 22, minus);
            add_text(window, "", x + 26, y + 3, 30, 18, value, true);
            add_button(window, "+", x + 58, y, 24, 22, plus);
        };
        add_text(window, "Notch snapping", label_x, 137, 130, 18, ID_SNAP_LABEL);
        stepper(control_x, 134, ID_SNAP_MINUS, ID_SNAP_VALUE, ID_SNAP_PLUS);
        add_dim_text(window, "stick units, 0 = off", hint_x, 137, 115, 18, ID_SNAP_HINT);

        add_text(window, "ESS range", label_x, 167, 130, 18, ID_RANGE_LABEL);
        stepper(control_x, 164, ID_RANGE_START_MINUS, ID_RANGE_START_VALUE, ID_RANGE_START_PLUS);
        add_text(window, "to", control_x + 86, 167, 22, 18, ID_RANGE_END_LABEL, true);
        stepper(control_x + 112, 164, ID_RANGE_END_MINUS, ID_RANGE_END_VALUE, ID_RANGE_END_PLUS);
        add_button(window, "Default", control_x + 204, 164, 62, 22, ID_RANGE_DEFAULT);
        add_dim_text(window, "in game", hint_x, 167, 115, 18, ID_RANGE_HINT);

        add_text(window, "Software dead zone", label_x, 197, 130, 18, ID_DEAD_ZONE_LABEL);
        stepper(control_x, 194, ID_DEAD_ZONE_MINUS, ID_DEAD_ZONE_VALUE, ID_DEAD_ZONE_PLUS);
        add_dim_text(window, "in game, 0 = off", hint_x, 197, 115, 18, ID_DEAD_ZONE_HINT);

        if (SOH_FEATURES) {
            add_text(window, "Playing on", label_x, 227, 130, 18, ID_GAME_LABEL);
            add_button(window, "Dolphin (Wii VC)", control_x, 224, 130, 22, ID_GAME_VC);
            add_button(window, "Ship of Harkinian", control_x + 134, 224, 130, 22, ID_GAME_SOH);

            add_text(window, "Input lag", label_x, 257, 130, 18, ID_LAG_LABEL);
            stepper(control_x, 254, ID_LAG_MINUS, ID_LAG_VALUE, ID_LAG_PLUS);
            add_dim_text(window, "ms, SoH only, 0 = off", hint_x, 257, 115, 18, ID_LAG_HINT);
        }
    }
    add_text(window, "", STICK_PLOT.left, STICK_PLOT.top - 20, 250, 18, ID_STICK_PLOT_TITLE);
    add_text(window, "", GAME_PLOT.left, GAME_PLOT.top - 20, 250, 18, ID_GAME_PLOT_TITLE);
    window->add(new Canvas({STICK_PLOT.left, STICK_PLOT.top, GAME_PLOT.right + LEGEND_WIDTH, STICK_PLOT.bottom}, ID_PLOTS, paint_plots));
    add_text(window, "", 10, SETTINGS_BOTTOM_Y + 4, 410, 18, ID_PROMPT);
    add_button(window, "Done", SETTINGS_WIDTH - 110, SETTINGS_BOTTOM_Y, 100, 24, ID_DONE);
    refresh_settings(window);
    window->set_non_modal();  // owned by the main window, which it stays above
    window->show();
}

// Notch offsets window: an offset per notch, each axis with -/+ buttons, and where holding
// that notch lands in game with everything else applied.
static const int OFFSETS_ROW_Y = 70, OFFSETS_ROW_HEIGHT = 28;
static const int OFFSETS_BOTTOM_Y = OFFSETS_ROW_Y + NOTCH_COUNT * OFFSETS_ROW_HEIGHT + 4;  // the prompt line, then the buttons

// Highlight a notch while the stick, after calibration and snapping, is this close to its target
// (or within the snap distance, if that's further).
static constexpr int HELD_DISTANCE = 6;

static void refresh_offsets(DarkWindow* window) {
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
                redraw_control(window, ID_OFFSET_NAME + notch);
                redraw_control(window, ID_OFFSET_GAME + notch);
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
            set_text(window, ID_OFFSET_VALUE + 2 * i + axis, std::to_string(calibration.offset[i][axis]));
        }
        // The calibrated notch, centered on neutral as the bridge loop does it, through the
        // whole stick path.
        uint8_t stick[2] = {static_cast<uint8_t>(128 + calibration.notch[i][0]), static_cast<uint8_t>(128 + calibration.notch[i][1])};
        process_stick(stick, calibration, settings);
        int in_game[2];
        game_in_game(settings.game, stick[0], stick[1], in_game);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "Game %3d,%3d", in_game[0], in_game[1]);
        set_text(window, ID_OFFSET_GAME + i, buffer);
    }
    // Repaint a -/+ button when it reaches or leaves its limit (it looks dimmed there).
    static bool shown_at_limit[NOTCH_COUNT * 4] = {};
    for (int step = 0; step < NOTCH_COUNT * 4; ++step) {
        const bool at_limit = limit_reason(ID_OFFSET_STEP + step) != nullptr;
        if (at_limit != shown_at_limit[step]) {
            shown_at_limit[step] = at_limit;
            redraw_control(window, ID_OFFSET_STEP + step);
        }
    }
    set_text(window, ID_OFFSETS_PROMPT, notice);
}

static void on_offsets_command(DarkWindow* window, int id) {
    if (id == ID_OFFSETS_DONE || id == ID_CANCEL) {
        window->close();
        return;
    }
    if (const char* reason = limit_reason(id)) {
        set_offsets_notice(reason);
        refresh_offsets(window);
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
            refresh_offsets(window);
            return;
        }
        if (!ask(window, "Reset every notch offset for port " + std::to_string(port + 1) + " to 0?")) {
            return;
        }
        std::lock_guard lock(g_mutex);
        std::fill(&g_notches[port].offset[0][0], &g_notches[port].offset[0][0] + NOTCH_COUNT * 2, 0);
    } else {
        return;
    }
    save_notch_offsets(port);
    refresh_offsets(window);
}

static void open_offsets(DarkWindow* owner, int port) {
    if (g_offsets_window && g_offsets_port == port) {
        g_offsets_window->show();  // brings it to the front
        return;
    }
    if (g_offsets_window) {
        g_offsets_window->close();
    }
    g_offsets_port = port;
    DarkWindow* window = new DarkWindow(400, OFFSETS_BOTTOM_Y + 58, "Port " + std::to_string(port + 1) + " notch offsets", on_offsets_command, [] {
        g_offsets_window = nullptr;
        g_held_notch = -1;
    });
    g_offsets_window = window;
    add_dim_text(window, "Move where each calibrated notch lands, in stick units. Game shows what OoT reads when you hold it.", 10, 10,
                 380, 34, ID_OFFSETS_HINT);
    const int x_column = 90, y_column = 186, game_column = 284;
    add_text(window, "X", x_column, OFFSETS_ROW_Y - 20, 82, 18, ID_OFFSET_NAME + NOTCH_COUNT, true);
    add_text(window, "Y", y_column, OFFSETS_ROW_Y - 20, 82, 18, ID_OFFSET_NAME + NOTCH_COUNT + 1, true);
    for (int i = 0; i < NOTCH_COUNT; ++i) {
        const int y = OFFSETS_ROW_Y + i * OFFSETS_ROW_HEIGHT;
        add_text(window, NOTCH_NAMES[i], 10, y + 3, 75, 18, ID_OFFSET_NAME + i);
        for (int axis = 0; axis < 2; ++axis) {
            const int x = axis == 0 ? x_column : y_column;
            const int step = ID_OFFSET_STEP + 4 * i + 2 * axis;
            add_button(window, "-", x, y, 24, 22, step);
            add_text(window, "", x + 26, y + 3, 30, 18, ID_OFFSET_VALUE + 2 * i + axis, true);
            add_button(window, "+", x + 58, y, 24, 22, step + 1);
        }
        add_text(window, "", game_column, y + 3, 106, 18, ID_OFFSET_GAME + i)->labelfont(FONT_MONO);
    }
    add_text(window, "", 10, OFFSETS_BOTTOM_Y, 380, 18, ID_OFFSETS_PROMPT);
    add_button(window, "Reset all", 10, OFFSETS_BOTTOM_Y + 24, 90, 24, ID_OFFSETS_RESET);
    add_button(window, "Done", 290, OFFSETS_BOTTOM_Y + 24, 100, 24, ID_OFFSETS_DONE);
    refresh_offsets(window);

    // Beside the settings window, so its plots stay visible: to the right if it fits on that
    // screen, else to the left, else where the system puts it. The window frames are the same
    // size, so the settings window's tells how big this one's will be.
    const int frame_x = owner->decorated_w() - owner->w(), frame_y = owner->decorated_h() - owner->h();
    const int border = frame_x / 2, title = frame_y - border;  // left and top of the frame
    const Rect settings = {owner->x() - border, owner->y() - title, owner->x() - border + owner->decorated_w(),
                           owner->y() - title + owner->decorated_h()};
    int screen_x, screen_y, screen_w, screen_h;
    Fl::screen_work_area(screen_x, screen_y, screen_w, screen_h, owner->x(), owner->y());
    const int width = window->w() + frame_x, height = window->h() + frame_y;
    const int top = std::clamp(settings.top, screen_y, std::max(screen_y, screen_y + screen_h - height));
    if (settings.right + width <= screen_x + screen_w) {
        window->position(settings.right + border, top + title);
    } else if (settings.left - width >= screen_x) {
        window->position(settings.left - width + border, top + title);
    }
    window->set_non_modal();  // owned by the settings window
    window->show();
}

// Writes the license texts built into the program to a temporary text file and opens it in the
// default text editor.
static void open_licenses() {
    std::string text = std::string(reinterpret_cast<const char*>(GPL_TEXT.data), GPL_TEXT.size) + "\n\n" +
                       std::string(reinterpret_cast<const char*>(THIRD_PARTY_TEXT.data), THIRD_PARTY_TEXT.size);
#ifdef _WIN32
    // With Windows line endings, for Notepad.
    std::string crlf;
    for (char c : text) {
        if (c == '\n') {
            crlf += '\r';
        }
        if (c != '\r') {
            crlf += c;
        }
    }
    text = crlf;
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "Software ESS Adapter licenses.txt";
#else
    const std::filesystem::path path = user_runtime_dir() / "licenses.txt";
#endif
    std::ofstream(path, std::ios::binary) << text;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", path.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    fl_open_uri(("file://" + path.string()).c_str());
#endif
}

// About window: name and version, credits, the GPL's no-warranty notice, the source link, and a
// way to read the licenses. GPLv3 asks interactive programs to show this, and it keeps the release
// zip free of separate license files.
static void on_about_command(DarkWindow* window, int id) {
    switch (id) {
    case ID_ABOUT_SOURCE:
        fl_open_uri(SOURCE_URL);
        break;
    case ID_ABOUT_LICENSES:
        open_licenses();
        break;
    case ID_ABOUT_CLOSE:
    case ID_CANCEL:
        window->close();
        break;
    }
}

static void open_about() {
    if (g_about_window) {
        g_about_window->show();  // brings it to the front
        return;
    }
    DarkWindow* window = new DarkWindow(358, 160, "About Software ESS Adapter", on_about_command, [] {
        g_about_window = nullptr;
    });
    g_about_window = window;
    Text* title = add_text(window, "Software ESS Adapter v" APP_VERSION, 14, 14, 330, 24, ID_ABOUT_TITLE);
    title->labelfont(FONT_TITLE);
    title->labelsize(TITLE_SIZE);
    add_text(window, "Created by Torje Amundsen\nESS map from ESS-Adapter by Skuzee", 14, 44, 330, 34, ID_ABOUT_CREDITS);
    add_dim_text(window, "Free software under the GNU GPL v3, with no warranty.", 14, 88, 330, 18, ID_ABOUT_LEGAL);
    add_button(window, "Source code", 14, 122, 100, 24, ID_ABOUT_SOURCE);
    add_button(window, "Licenses", 122, 122, 90, 24, ID_ABOUT_LICENSES);
    add_button(window, "Close", 254, 122, 90, 24, ID_ABOUT_CLOSE);
    window->set_non_modal();  // owned by the main window
    window->show();
}

static void on_main_command(DarkWindow*, int id) {
    if (id == ID_EDIT) {
        open_settings();
    } else if (id == ID_ABOUT) {
        open_about();
    } else if (id == ID_CANCEL && Fl::callback_reason() != FL_REASON_CANCELLED) {
        // Closing the main window (not Escape, which only closes the others) quits.
        while (Fl::first_window()) {
            Fl::first_window()->hide();
        }
    }
}

static DarkWindow* create_main_window() {
    DarkWindow* window = new DarkWindow(MAIN_RIGHT_X + 80, MAIN_BOTTOM_Y + 34, "Software ESS Adapter", on_main_command, nullptr);
    window->xclass(WINDOW_CLASS);
    window->add(new Canvas(ACTIVE_RECT, ID_ACTIVE, paint_active_indicator));
    // The status lines wrap to two lines so error messages fit.
    add_text(window, "", 10, 10, 335, 34, ID_ADAPTER_STATUS);
    add_text(window, g_dsu.last_error().c_str(), 10, 46, 335, 34, ID_DSU_STATUS);
    for (int p = 0; p < PORT_COUNT; ++p) {
        add_text(window, "", 10, 86 + p * 20, 340, 18, ID_MAIN_PORT + p);
    }
    add_button(window, "Edit settings", 10, MAIN_BOTTOM_Y, 120, 24, ID_EDIT);
    add_dim_text(window, "Stick values only show while editing.", 140, MAIN_BOTTOM_Y + 4, 205, 18, ID_MAIN_HINT);
    add_button(window, "About", MAIN_RIGHT_X, MAIN_BOTTOM_Y, 70, 24, ID_ABOUT);
    refresh_main(window);
    return window;
}

// Makes the built-in fonts available, then names them for FLTK.
static void load_fonts() {
#ifdef _WIN32
    for (const Embedded* font : {&SELAWIK_TTF, &SELAWIK_SEMIBOLD_TTF, &CASCADIA_MONO_TTF}) {
        DWORD count = 0;
        AddFontMemResourceEx(const_cast<unsigned char*>(font->data), static_cast<DWORD>(font->size), nullptr, &count);
    }
#else
    // fontconfig only loads fonts from files, so write them out first.
    const std::filesystem::path dir = user_runtime_dir();
    for (const auto& [font, name] : {std::pair{&SELAWIK_TTF, "Selawik-Regular.ttf"}, {&SELAWIK_SEMIBOLD_TTF, "Selawik-Semibold.ttf"},
                                     {&CASCADIA_MONO_TTF, "CascadiaMono-SemiLight.ttf"}}) {
        const std::filesystem::path path = dir / name;
        std::error_code error;
        if (std::filesystem::file_size(path, error) != font->size) {
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(font->data), static_cast<std::streamsize>(font->size));
        }
        FcConfigAppFontAddFile(nullptr, reinterpret_cast<const FcChar8*>(path.c_str()));
    }
#endif
    Fl::set_font(FONT_UI, "Selawik");
    Fl::set_font(FONT_MONO, "Cascadia Mono SemiLight");
    Fl::set_font(FONT_TITLE, "Selawik Semibold");
}

#ifdef _WIN32
// Windows draws the title bars. Make them dark to match, while each window is made so a light one
// never shows first, and leave the minimize button to the main window, as for dialogs.
static LRESULT CALLBACK on_window_message(int code, WPARAM wparam, LPARAM lparam) {
    const CWPSTRUCT& message = *reinterpret_cast<const CWPSTRUCT*>(lparam);
    if (code == HC_ACTION && (message.message == WM_NCCREATE || message.message == WM_CREATE)) {
        char window_class[64] = "";
        GetClassNameA(message.hwnd, window_class, sizeof(window_class));
        if (std::strcmp(window_class, WINDOW_CLASS) != 0) {
            // not ours, like a message box
        } else if (message.message == WM_NCCREATE && GetWindow(message.hwnd, GW_OWNER)) {
            SetWindowLongPtrW(message.hwnd, GWL_STYLE, GetWindowLongPtrW(message.hwnd, GWL_STYLE) & ~WS_MINIMIZEBOX);
        } else if (message.message == WM_CREATE) {
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(message.hwnd, DWM_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
#endif

// Refreshes the windows 60 times a second. FLTK's timers on Windows wait in whole ticks of the
// 15.6 ms system clock, which lands nowhere near 60, so a thread waits on a precise timer instead
// and hands each frame to the UI thread. Without high-resolution support (before Windows 10 1803)
// this falls back to a normal timer.
static std::atomic<bool> g_frame_pending{false};

static void refresh_windows(void*) {
    g_frame_pending = false;
    refresh_main(g_main_window);
    if (g_settings_window) {
        refresh_settings(g_settings_window);
    }
    if (g_offsets_window) {
        refresh_offsets(g_offsets_window);
    }
}

static void frame_loop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / 60));
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
#endif
    auto next_frame = clock::now() + period;
    clock::time_point posted;
    while (g_running) {
        auto now = clock::now();
        if (now - next_frame > period) {
            next_frame = now;  // fell behind, don't try to catch up
        }
#ifdef _WIN32
        LARGE_INTEGER due;  // negative = relative, in 100 ns units
        due.QuadPart = -std::max<LONGLONG>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(next_frame - now).count() / 100);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
#else
        std::this_thread::sleep_until(next_frame);
#endif
        next_frame += period;
        // One frame at a time waits for the UI thread. A Windows message box drops the wake-up,
        // so one waiting longer than a quarter second is sent again.
        now = clock::now();
        if (!g_frame_pending || now - posted > std::chrono::milliseconds(250)) {
            g_frame_pending = true;
            posted = now;
            Fl::awake(refresh_windows, nullptr);
        }
    }
#ifdef _WIN32
    CloseHandle(timer);
#endif
}

int main(int, char**) {
    // Only one copy can own the adapter and the DSU port, so a second start just shows the first
    // (on Linux it just quits).
#ifdef _WIN32
    CreateMutexA(nullptr, FALSE, "SoftwareESSAdapter.Running");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND running = FindWindowA(WINDOW_CLASS, "Software ESS Adapter")) {
            ShowWindow(running, SW_RESTORE);
            SetForegroundWindow(running);
        }
        return 0;
    }
#else
    const int lock = open((user_runtime_dir() / "running.lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        return 0;
    }
#endif

    load_settings();
    g_dsu.start(DsuServer::DEFAULT_PORT);
    std::thread bridge(bridge_loop);

    Fl::lock();  // lets the frame thread wake the UI thread
    Fl::keyboard_screen_scaling(0);  // no Ctrl+/Ctrl- zoom
    Fl::visible_focus(1);
    Fl::event_dispatch([](int event, Fl_Window* window) {
        if (event == FL_KEYBOARD && (Fl::event_key() == FL_Tab || (Fl::event_key() >= FL_Left && Fl::event_key() <= FL_Down))) {
            g_keyboard_used = true;
            // No button has focus before the keyboard is used, so the first Tab focuses the first
            // one, as in a Windows dialog.
            if (window && (!Fl::focus() || Fl::focus() == window)) {
                for (int i = 0; i < window->children(); ++i) {
                    if (window->child(i)->take_focus()) {
                        return 1;
                    }
                }
            }
        }
        return Fl::handle_(event, window);
    });
    load_fonts();
#ifdef _WIN32
    // App icon from assets/resources.rc, at the system's large and small icon sizes.
    auto load_icon = [](int size_metric) {
        const int size = GetSystemMetrics(size_metric);
        return static_cast<HICON>(LoadImageA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(APP_ICON), IMAGE_ICON, size, size, 0));
    };
    Fl_Window::default_icons(load_icon(SM_CXICON), load_icon(SM_CXSMICON));
    HHOOK hook = SetWindowsHookExA(WH_CALLWNDPROC, on_window_message, nullptr, GetCurrentThreadId());
#else
    static Fl_PNG_Image icon(nullptr, ICON_PNG.data, static_cast<int>(ICON_PNG.size));
    Fl_Window::default_icon(&icon);
    // The yes/no questions are FLTK's own dialog on Linux: dark, in the same font.
    Fl::background(32, 32, 32);
    Fl::background2(55, 55, 55);
    Fl::foreground(230, 230, 230);
    fl_message_font(FONT_UI, FONT_SIZE);
#endif

    g_main_window = create_main_window();
    g_main_window->show();
    std::thread frames(frame_loop);
    Fl::run();

    g_running = false;
    frames.join();
    bridge.join();
    g_dsu.stop();
#ifdef _WIN32
    UnhookWindowsHookEx(hook);
#endif
    return 0;
}
