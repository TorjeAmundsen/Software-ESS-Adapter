//ess.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// PC port of the Gamecube path in ESS-Adapter/ESS.cpp (github.com/Skuzee/ESS-Adapter). Logic is
// unchanged, only the AVR PROGMEM access is replaced with plain array reads.

#pragma once
#include <cstdint>

void gc_to_n64(uint8_t coords[2]);

uint16_t triangular_to_linear_index(uint8_t row, uint8_t col, uint8_t size);

void invert_vc(uint8_t coords[2]);

void invert_vc_gc(uint8_t coords[2]);

void normalize_origin(uint8_t coords[2], uint8_t origin[2]);
