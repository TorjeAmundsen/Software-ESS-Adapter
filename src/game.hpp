//game.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#pragma once

// Where the stick goes. The ESS map aims for N64 stick values in both; what differs is the layer in
// between. On Wii VC (in Dolphin), VC turns the Gamecube stick we send into N64 values, so the map
// sends whatever stick VC turns into the right ones. Ship of Harkinian reads the DSU stick through
// libultraship's stick processing, so the bytes are worked out against that instead.
enum class Game { WiiVC, SoH };
