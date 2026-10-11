//ess_range.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Configurable ESS range. In Ocarina of Time, Link turns in place without walking when the
// stick's in-game value is 8 to 26 in a cardinal direction (0 to 7 is the game's dead zone).
// This stretches the physical stick radially so that a chosen physical [start, end] lands on
// that range, before the ESS map runs. Works on origin-centered stick bytes (128 = center).
//
// Users set start and end in in-game units: "start 6" means ESS begins where the stick
// would normally read 6 in game. physical_start/physical_end convert those to stick distances.
// The stock map reads differently on Wii VC and on Ship of Harkinian, so the conversions take the
// game.

#pragma once
#include <cstdint>

#include "game.hpp"

// The game's ESS range.
constexpr int OOT_ESS_START = 8;
constexpr int ESS_IN_GAME_END = 26;
// The default range start: the first ESS value a player can actually reach. VC never produces 8,
// so on Wii VC that is 9.
int default_ess_start(Game game);

// Physical distances where the stock ESS map enters and leaves the ESS range on a cardinal, on
// Wii VC. stock_ess_start and stock_ess_end give them for either game.
constexpr int ESS_DEFAULT_START = 11;
constexpr int ESS_DEFAULT_END = 34;
constexpr int ESS_MAX_END = 100;

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
// Largest in-game end the settings allow.
int max_in_game_end(Game game);

// Software dead zone, in the same in-game units as the ESS range: N covers where the stick would
// normally read N or less in game, so with the default range a stick resting at 1, 1 shows 0, 0
// in practice tools. It goes up to the ESS range start. Below the start, the ESS range stretches
// the stick evenly onto the game's own dead zone (0 to 7), and the software dead zone is stretched
// along with it: at range start - 1 it covers exactly that, and never any of ESS.
constexpr int DEAD_ZONE_DEFAULT = 2;
// Distance from center, per axis, below which the dead zone applies (0 = off).
int physical_dead_zone(Game game, int in_game);
// The dead zone distance after the ESS range has moved the stick: scaled like everything below
// the range start, by stock_ess_start / start.
double dead_zone_after_range(Game game, int distance, int start);
// Centers the stick if both axes are closer than `distance` to center (a square, since the game
// and VC treat each axis separately).
void apply_dead_zone(uint8_t stick[2], double distance);

// Requires 1 <= start < end <= ESS_MAX_END, in physical distances. Distances below start
// become dead zone, [start, end] becomes ESS, and above end is stretched out to full tilt
// (105), which stays where it is.
void remap_ess_range(Game game, uint8_t stick[2], int start, int end);
