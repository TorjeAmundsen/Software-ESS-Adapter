//ess_range.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Configurable ESS range. In Ocarina of Time, Link turns in place without walking when the
// stick's in-game value is 8 to 26 in a cardinal direction (0 to 7 is the game's dead zone).
// This stretches the physical stick radially so that a chosen physical [start, end] lands on
// that range, before the ESS map runs. Works on origin-centered stick bytes (128 = center).
//
// Users set it in stick units. The start is the stick distance where ESS begins, and the range is
// how many distances it lasts, so the last ESS distance is start + range - 1. The stock map's ESS
// runs from 11 to 34 on Wii VC, which never produces 8, and from 10 to 34 on Ship of Harkinian.

#pragma once
#include <cstdint>

#include "game.hpp"

// The game's ESS range.
constexpr int OOT_ESS_START = 8;
constexpr int ESS_IN_GAME_END = 26;

// Physical distances where the stock ESS map enters and leaves the ESS range on a cardinal, on
// Wii VC. stock_ess_start and stock_ess_end give them for either game.
constexpr int ESS_DEFAULT_START = 11;
constexpr int ESS_DEFAULT_END = 34;
constexpr int ESS_MAX_END = 100;

// The settings' defaults and limits, in stick units. With start 10 and range 25, ESS is the stock
// map's on Ship of Harkinian and starts one distance sooner on Wii VC. The range goes up to 45,
// and 0 means no ESS at all.
constexpr int DEAD_ZONE_DEFAULT = 5;
constexpr int ESS_START_DEFAULT = 10;
constexpr int ESS_RANGE_DEFAULT = 25;
constexpr int ESS_MAX_START = 50;
constexpr int ESS_MAX_RANGE = 45;
// The last ESS distance with this start and range. With range 0 it's start - 1, before the start.
constexpr int ess_end(int start, int range) {
    return start + range - 1;
}

// Physical distances where the stock ESS map enters and leaves the ESS range on a cardinal. The
// remap is anchored on these, so a range of exactly these distances changes nothing.
int stock_ess_start(Game game);
int stock_ess_end(Game game);

// In-game values Wii VC produces from signed Gamecube stick offsets.
// Port of OOTVCMap.map in ESS-Adapter/generate-map.py.
void vc_in_game(int gc_x, int gc_y, int in_game[2]);
// Same, for one axis with the other at 0.
int vc_in_game(int gc_offset);

// In-game values the game makes from the stick bytes we send (128 = center).
void game_in_game(Game game, uint8_t x, uint8_t y, int in_game[2]);

// How OoT reads an in-game stick value: a per-axis dead zone of 7, then up to 60 more per axis, and
// the magnitude of that, capped at 60 (PadUtils_UpdateRelXY in libu64/pad.c,
// Lib_GetControlStickData in z_lib.c). Below 20 Link turns in place (ESS), from 20 he walks
// (Player_CalcSpeedAndYawFromControlStick in z_player.c). Standing actions switch from walking to
// running once magnitude * 0.8 * 0.14 passes 4.9, so past 43.75. Running speed follows a curve up
// to the speed cap, 6 with normal boots (R_RUN_SPEED_LIMIT 600), reached at about 58.6. The run
// and full speed edges are for flat ground: slopes and other boots move them.
constexpr int OOT_DEAD_ZONE = 7;
constexpr int OOT_REL_MAX = 60;
constexpr int OOT_WALK_MAGNITUDE = 20;
constexpr double OOT_RUN_MAGNITUDE = 4.9 / (0.8 * 0.14);
enum class OotZone { DeadZone, Ess, Walk, Run, FullSpeed };
// The magnitude OoT works out from an in-game pair, 0 to 60.
float oot_magnitude(int x, int y);
OotZone oot_zone(int x, int y);

// In-game value the stock ESS map gives at a physical distance on a cardinal.
int stock_in_game(Game game, int distance);

// First physical distance where the stock map reaches `in_game` (start of the range).
int physical_start(Game game, int in_game);
// Last physical distance where the stock map does not go past `in_game` (end of the range).
int physical_end(Game game, int in_game);

// The software dead zone is a distance from center, per axis. Closer than that on both axes, the
// stick goes to center, and 0 turns it off. It goes up to the ESS range start. Below the start,
// the ESS range stretches the stick evenly onto the game's own dead zone, 0 to 7, and stretches
// the software dead zone with it. At the start it covers exactly the game's dead zone and never
// any of ESS.
// physical_dead_zone gives the distance that covers where the stock map reads `in_game` or less.
// Only old settings files, which stored the dead zone in game units, need it.
int physical_dead_zone(Game game, int in_game);
// The dead zone distance after the ESS range has moved the stick: scaled like everything below
// the range start, by stock_ess_start / start.
double dead_zone_after_range(Game game, int distance, int start);
// Centers the stick if both axes are closer than `distance` to center (a square, since the game
// and VC treat each axis separately).
void apply_dead_zone(uint8_t stick[2], double distance);

// Full speed distance: the first stick distance on a cardinal where Link runs at full speed. The
// stock map goes full speed from 87 on the Wii VC and 86 on SoH. AUTO_FULL_SPEED keeps the stock
// map's proportions: everything past the ESS end is spread evenly out to full tilt, so a wider
// ESS range reaches full speed a little sooner. A set distance needs MIN_FULL_SPEED_GAP units past
// the ESS end, so walking and running keep some of the stick, and goes up to
// MAX_FULL_SPEED_START, so full speed does too.
constexpr int AUTO_FULL_SPEED = -1;
constexpr int MIN_FULL_SPEED_GAP = 4;
constexpr int MAX_FULL_SPEED_START = 100;
// First physical distance where the stock map goes full speed on a cardinal.
int stock_full_speed_start(Game game);
// The full speed distance AUTO_FULL_SPEED gives with this ESS range end (a physical distance).
double auto_full_speed_start(Game game, int end);
int min_full_speed_start(int end);

// Requires 1 <= start and start - 1 <= end <= ESS_MAX_END, in physical distances, where
// end = start - 1 means no ESS at all. The full speed distance is AUTO_FULL_SPEED or
// min_full_speed_start(end) to MAX_FULL_SPEED_START. Distances below start become dead zone, [start, end] becomes ESS, then walking and running split the way the stock map
// splits them up to the full speed distance, then full speed out to full tilt (105), which stays
// where it is. With AUTO_FULL_SPEED, or a distance equal to it rounded, everything past the end is
// spread evenly out to full tilt instead. Start, end and the full speed distance are cardinal
// distances. In other directions they scale with where the stock map's ESS starts and ends and
// full speed starts there, so the zones keep the stock map's shape all the way around.
void remap_ess_range(Game game, uint8_t stick[2], int start, int end, int full_speed = AUTO_FULL_SPEED);
