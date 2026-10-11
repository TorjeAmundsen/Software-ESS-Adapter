//soh.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Ship of Harkinian: the in-game values the ESS map aims for, and the in-game values SoH makes
// from the DSU stick bytes we send.

#pragma once
#include <cstdint>

// The ESS map's N64 stage (gc_to_n64) on an origin-centered stick (128 = center), without the Wii
// VC inversion: the in-game values the map aims for, rounded to whole values. Past the gate it
// would go over the N64's range, so each axis stops at 80, as the VC inversion does it.
void n64_in_game(const uint8_t stick[2], int in_game[2]);

// SoH's stick processing bounds the stick to an octagon through 85, 0 and 69, 69, a little inside
// the N64's corner of 70, 70. Moves a pair on or outside its edge straight toward center until it
// is inside, so the corner reads 68, 68.
void fit_soh_octagon(int in_game[2]);

// In-game values SoH makes from a DSU stick byte pair, with the stick set to dead zone 0%,
// sensitivity 100% and notch snap angle 0. A copy of libultraship's math, float for float:
// DSUAxisDirectionValue, DSUAxisDirectionToAxisDirectionMapping and ControllerStick::Process.
// `margin`, if given, gets how far the less safe axis is from truncating to a different value,
// in in-game units, so callers can avoid pairs a rounding difference between builds could flip.
void soh_in_game(uint8_t x, uint8_t y, int in_game[2], double* margin = nullptr);
