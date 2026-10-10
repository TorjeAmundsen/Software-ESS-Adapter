//ess_range.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Configurable ESS range. In Ocarina of Time, Link turns in place without walking when the
// stick's in-game value is 8 to 26 in a cardinal direction (0 to 7 is the game's dead zone).
// This stretches the physical stick radially so that a chosen physical [start, end] lands on
// that range, before the ESS map runs. Works on origin-centered stick bytes (128 = center).
//
// Users set start and end in in-game units: "start 6" means ESS begins where the stick
// would normally read 6 in game. physical_start/physical_end convert those to stick distances.

#pragma once
#include <cstdint>

// The game's ESS range and the default setting. The game's range starts at 8, but VC never
// produces 8, so 9 is the first ESS value a player can actually reach.
constexpr int ESS_IN_GAME_START = 9;
constexpr int ESS_IN_GAME_END = 26;

// Physical distances where the stock ESS map enters and leaves the ESS range on a cardinal.
// The remap is anchored on these, so a range of exactly these distances changes nothing.
constexpr int ESS_DEFAULT_START = 11;
constexpr int ESS_DEFAULT_END = 34;
constexpr int ESS_MAX_END = 100;

// In-game values Wii VC produces from signed Gamecube stick offsets.
// Port of OOTVCMap.map in ESS-Adapter/generate-map.py.
void vc_in_game(int gc_x, int gc_y, int in_game[2]);
// Same, for one axis with the other at 0.
int vc_in_game(int gc_offset);

// How OoT reads an in-game stick value: a per-axis dead zone of 7, then up to 60 more per axis, and
// the magnitude of that. Below 20 Link turns in place (ESS), from 20 he walks, and at 60 he is at
// full speed (PadUtils_UpdateRelXY in libu64/pad.c, Player_CalcSpeedAndYawFromControlStick in
// z_player.c).
constexpr int OOT_DEAD_ZONE = 7;
constexpr int OOT_REL_MAX = 60;
constexpr int OOT_WALK_MAGNITUDE = 20;
enum class OotZone { DeadZone, Ess, Walk };
OotZone oot_zone(int x, int y);

// In-game value the stock ESS map gives at a physical distance on a cardinal.
int stock_in_game(int distance);

// First physical distance where the stock map reaches `in_game` (start of the range).
int physical_start(int in_game);
// Last physical distance where the stock map does not go past `in_game` (end of the range).
int physical_end(int in_game);
// Largest in-game end the settings allow.
int max_in_game_end();

// Software dead zone, in the same in-game units as the ESS range: N covers where the stick would
// normally read N or less in game, so with the default range a stick resting at 1, 1 shows 0, 0
// in practice tools. It goes up to the ESS range start. Below the start, the ESS range stretches
// the stick evenly onto the game's own dead zone (0 to 7), and the software dead zone is stretched
// along with it: at range start - 1 it covers exactly that, and never any of ESS.
constexpr int DEAD_ZONE_DEFAULT = 2;
// Distance from center, per axis, below which the dead zone applies (0 = off).
int physical_dead_zone(int in_game);
// The dead zone distance after the ESS range has moved the stick: scaled like everything below
// the range start, by ESS_DEFAULT_START / start.
double dead_zone_after_range(int distance, int start);
// Centers the stick if both axes are closer than `distance` to center (a square, since the game
// and VC treat each axis separately).
void apply_dead_zone(uint8_t stick[2], double distance);

// Requires 1 <= start < end <= ESS_MAX_END, in physical distances. Distances below start
// become dead zone, [start, end] becomes ESS, and above end is stretched out to full tilt
// (105), which stays where it is.
void remap_ess_range(uint8_t stick[2], int start, int end);
