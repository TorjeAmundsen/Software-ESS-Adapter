//embedded.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Files built into the program by cmake/embed.cmake, so the release is a single executable.

#pragma once
#include <cstddef>

struct Embedded {
    const unsigned char* data;
    size_t size;
};

extern const Embedded GPL_TEXT;              // LICENSE.md, shown from the About window
extern const Embedded THIRD_PARTY_TEXT;      // assets/licenses/third-party.txt, likewise
extern const Embedded SELAWIK_TTF;           // assets/fonts: Selawik, metric-compatible with Segoe UI
extern const Embedded SELAWIK_SEMIBOLD_TTF;
extern const Embedded CASCADIA_MONO_TTF;     // SemiLight, as light as Consolas, for the stick readouts
extern const Embedded ICON_PNG;              // the window icon on Linux; Windows uses icon.ico
