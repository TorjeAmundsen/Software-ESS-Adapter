# Developing Software ESS Adapter

How the program works, how to build it, and where things are in the source. For using it, see [README.md](README.md).

## How it works

Dolphin's own adapter support reads the USB device directly, and nothing else can hook into that path. So the program claims the adapter (WUP-028) with libusb, runs each report through origin centering (with the calibrated neutral range), the notch warp, notch snapping, the ESS range, the dead zone, the ESS map and a pass that keeps each axis moving with its input in game, and serves the result to Dolphin (or Ship of Harkinian) as a DSU (cemuhook) controller over UDP on `127.0.0.1:26760`. It forwards every adapter report as it arrives, so its rate matches the adapter's (125 Hz stock, higher when overclocked).

### Does the ESS output survive Dolphin?

Yes, if the stick has no dead zone and no calibration in Dolphin, with one correction on our side. Dolphin turns a DSU stick byte `v` into `(v-128)/127` above center and `(v-128)/128` below, then maps it back to a byte with `MapFloat(x, 128, 1)` (GCPadEmu.cpp), which scales by 127 on both sides. Above center every byte comes back unchanged. Below center, bytes 0 to 64 come back one too high, so ESS full tilt left (64) would arrive as 65. `to_dolphin_stick` in `dsu_server.cpp` sends one less for 1 to 64, so every byte from 1 to 255 arrives as intended (0 can't be reached and arrives as 1). `bridge-tests` checks all of them with Dolphin's formulas. A dead zone or a calibration other than the default in Dolphin rescales the stick and breaks the ESS map. The profile doesn't need to set them: loading a profile resets any setting it leaves out to its default (`NumericSetting::LoadFromIni`), and an empty calibration and center mean the defaults (`ReshapableInput::LoadConfig`).

### Ship of Harkinian

Dev builds only (`.\build.ps1 --dev`, the CMake option `SOH_DEV`) until SoH's DSU input is in an official libultraship release. Other builds have no "Playing on" or input lag rows, always play on Wii VC, and ignore those settings in the settings file. The SoH code and its tests are in `bridge_core` and build and run either way.

With "Playing on" set to Ship of Harkinian, there is no VC in between: the game reads the stick from libultraship. So the target is the ESS map's N64 stage (`gc_to_n64`), with each axis capped at 80 the way the VC inversion caps it, and the program sends whichever bytes SoH turns into that. This needs a libultraship with DSU input, which isn't upstream yet, and SoH's stick set to dead zone 0%, sensitivity 100% and notch snap angle 0.

With those settings, libultraship reads a byte `v` as `(v-128)/127` of 85, clamped, and runs `ControllerStick::Process`: an octagon through 85, 0 and 69, 69, a trip through `atan2` and `cos`/`sin` past 90% of the range, and truncation to `int8_t`. `soh_in_game` in `soh.cpp` copies that math float for float. For each in-game pair, the table sends the byte pair furthest from a truncation edge and leaves out pairs within 1e-6 of one, so a build that rounds a last digit differently still reads the same. The octagon's corner, 69, 69, only comes out exactly on a truncation edge, so targets stay strictly inside the octagon, and a calibrated diagonal reads 68, 68 (70, 70 on N64). SoH reads bytes as they are, so `to_dolphin_stick` is skipped. SoH can produce 8, so there the default ESS range is 8 to 26.

SoH answers the stick much sooner than Dolphin running Wii VC, so "Input lag" (SoH only, 0 to 100 ms, as in pyESS) delays everything that goes out, buttons and disconnects included. `DelayedSender` sends each frame exactly that long after it came in, from its own thread on a high-resolution waitable timer, so the lag isn't rounded to the adapter's report rate (the tests measure it under half a millisecond late). Changing the lag drops frames still waiting, so nothing goes out of order. The main window shows the lag next to the connection status. In the settings window, the "In game" plot and the Game readouts lag by the same amount (from the last 150 ms of processed frames), so they show what SoH has at that moment.

### Centering

Without calibrated notches, the first report after a controller is plugged in is its origin, like Dolphin and the console. With calibrated notches, the 8 neutral samples replace it. Their average is the center, and any position between the smallest and largest sample on each axis reads exactly 0, 0. So the center survives replugs and power cycles.

### ESS range and software dead zone

OoT turns Link in place without walking when the in-game stick value is 8 to 26 on a cardinal. 0 to 7 is the game's dead zone, and walking starts at magnitude 20 (27 on a cardinal), per `PadUtils_UpdateRelXY` and `Player_CalcSpeedAndYawFromControlStick` in the OoT decomp. VC never produces 8, 13, 17, 22 or 25, so on VC 9 is the first reachable ESS value.

The ESS range settings are "feel" values. Start S and end E are turned into the physical distances where the stock map reads S and E in game, and the stick is remapped radially so those distances land on the stock 9 and 26. Below the start goes into the game dead zone, past the end is spread out to full tilt, and full tilt doesn't move. A skipped value behaves like the next reachable value as a start and the previous one as an end. The game applies its dead zone per axis, so its ESS edge isn't round: on a diagonal the stock map's ESS starts and ends further out than on a cardinal. So the remap scales the range with the stock map's edges in each direction, worked out from `gc_to_n64`'s formula before rounding (dead zone edge at 7.5, ESS edge at magnitude 19.5), and a custom range keeps the stock ESS shape all the way around. A plain radial remap would warp it, more the narrower or wider the range. The range is exact on cardinals; elsewhere it follows the stock shape to within rounding.

The software dead zone uses the same feel units, scaled with the range start. It's square: a stick reads 0, 0 when both axes are inside it. It goes up to the range start minus 1, so ESS itself is never affected.

The "In game" plot draws ESS as a rounded square, because OoT removes a dead zone of 7 from each axis before checking the magnitude against 20. With ESS on, a calibrated notch with no notch offset reads 81 straight and 58, 58 diagonally, the furthest VC goes.

### Notch calibration checks

A notch only counts if it's less than 22.5 degrees from its own direction and at least `NOTCH_MIN_DISTANCE` (half of full tilt) from neutral. That keeps the notches in clockwise order, so the warp can't fold the stick over itself. Each notch press is checked against the current center, and the finished calibration again against the calibrated neutral. Saved calibrations that fail the check load as uncalibrated.

### Notch offsets

Each notch has a notch offset (as in ElectroModder's adapter): the warp sends the calibrated notch to its target, the ideal notch plus the offset, instead of the ideal notch, and snapping snaps to that spot. Offsets are in stick units before the ESS range and ESS map. A target follows the same rule as a calibrated notch: within 22.5 degrees of its own direction and at least `NOTCH_MIN_DISTANCE` from center, so the warp still can't fold. It also has to fit in a stick byte. That's room for setups like an Up-Left notch moved well toward Left for a specific angle (-75, 48 is 12.4 degrees off). They're saved per port as `NotchOffsets`, kept when calibrating again, and removed by Clear. Our ideal diagonal is 75, 75, so offsets copied from an adapter whose diagonal is 70, 70 land somewhere else. The notch offsets window shows the in-game result through the whole path, so check that instead.

Moving a notch target moves both axes, and so does snapping. VC then shortens long sticks (`vc_in_game`), which can pull one axis back as the other moves on, so the in-game value can step backwards even with ESS off: snap 5 alone does it at 8 spots inside the gate, and large offsets at hundreds. So the monotonic pass in `process_stick` runs with ESS off too. Without calibration and with ESS off it changes nothing.

The rule exists to keep the order dead zone, ESS, walking, running, and keeping each axis in order isn't quite enough for that. The held axis may still move toward center: past the ESS range end the remap squeezes the rest of the stick, so a held y shrinks a little as x grows. With a narrow range (5 to 13 in game) that dropped the magnitude back under 20 in places, going walk, ESS, walk within three stick units (32 spots on VC, 24 on SoH). So the pass also keeps every position in the same zone as its neighbors further in, or a later one (full speed counts as running). When the game can't do both, keeping the axes in order wins, since that is the rule as written. The tests check the zone order on every line too.

### DSU buttons

DSU copies a PlayStation controller, so Gamecube buttons go out under PlayStation names (A is Cross, Z is R1, and so on). `dolphin/Software ESS Adapter.ini` has the full mapping. To map the main stick by hand in Dolphin, ESS has to be off for that port: ESS output never goes past half deflection, because VC already treats that as full tilt, and Dolphin only registers a stick direction for mapping past 55%. SoH's input editor needs 70%, which the Ship of Harkinian output reaches.

## Build

You need CMake 3.20+, Ninja, and a C++17 compiler. Either works:

- **MSVC**, from Visual Studio with the "Desktop development with C++" workload. `build.ps1` finds it on its own, no developer prompt needed. Releases are built with MSVC, so it's preferred when both are installed.
- **MinGW-w64 g++** 9 or newer, for example from [MSYS2](https://www.msys2.org/) (`C:\msys64\mingw64\bin` on PATH).

```
.\build.ps1
```

This builds the program in `build/msvc` or `build/mingw`, depending on the compiler (a separate folder each, so switching compilers just works), and writes `dist/software-ess-adapter.zip` with the exe and the Dolphin profile. `.\build.ps1 --output <name>` writes `dist/<name>.zip` instead, and `--dev` adds the Ship of Harkinian features (see above). The exe has the C++ runtime built in, so it only needs DLLs that come with Windows.

To run the tests:

```
cmake --build build/mingw --target bridge-tests
.\build\mingw\bridge-tests.exe
```

With MSVC, use `build/msvc` instead.

CMake downloads libusb 1.0.27 and compiles its Windows sources directly, since libusb has no CMake build. libusb only ships a `config.h` for MSVC, so `cmake/libusb-mingw/config.h` provides the same settings for MinGW. To build offline, point CMake at any libusb checkout, for example Dolphin's, with `-DFETCHCONTENT_SOURCE_DIR_LIBUSB="<dolphin>/Externals/libusb/libusb"`.

## Releases

The version is `APP_VERSION` in `CMakeLists.txt` (`set(APP_VERSION "0.1.0-alpha")`). It follows semver, optionally with a prerelease suffix like `-alpha` or `-beta.1`. The About window shows it as "Software ESS Adapter v0.1.0-alpha".

To release:

1. Bump `APP_VERSION` in `CMakeLists.txt` and merge it to `main`.
2. Tag the commit and push the tag:

   ```
   git tag v0.1.0-alpha
   git push origin v0.1.0-alpha
   ```

The Release workflow (`.github/workflows/release.yml`) checks that the tag is exactly `v` plus `APP_VERSION`, runs the tests, builds `software-ess-adapter-v0.1.0-alpha-win-x64.zip`, and creates a draft GitHub release with it. Review the draft and publish it. A version with a suffix becomes a prerelease.

The Validate workflow (`.github/workflows/validate.yml`) builds and runs the tests on every pull request and every push to `main`.

## Code style

The style is in `.clang-format`: 4-space indents, 140-column lines, and braces on every `if` and loop. Visual Studio picks it up automatically. To format from the command line, run `clang-format -i src/*.cpp src/*.hpp test/*.cpp`. `src/ess.cpp` is left out on purpose, through `.clang-format-ignore`, because its functions are copied unchanged from `ESS.cpp` in ESS-Adapter. Only a few defines at the top replace the Arduino-specific memory macros.

## Source

- `src/ess.cpp` holds the Gamecube functions from `ESS-Adapter/ESS.cpp` in [Skuzee/ESS-Adapter](https://github.com/Skuzee/ESS-Adapter), copied unchanged, with a few defines at the top in place of the Arduino-specific memory macros. If the map changes there, copy the functions here too.
- `src/gc_adapter.cpp` reads the adapter with libusb, using the same protocol as Dolphin's `GCAdapter.cpp`.
- `src/dsu_server.cpp` is the UDP server, with packet layouts from Dolphin's `DualShockUDPProto.h`.
- `src/delayed_sender.cpp` adds the optional input lag on SoH.
- `src/soh.cpp` has the ESS map's N64 stage for Ship of Harkinian and a copy of SoH's stick math.
- `src/ess_range.cpp` has the ESS range remap, the software dead zone, the Wii VC stick mapping (ported from `generate-map.py` in ESS-Adapter), and OoT's dead zone and ESS rules (`PadUtils_UpdateRelXY` and `Player_CalcSpeedAndYawFromControlStick` in the OoT decomp).
- `src/stick.cpp` has `process_stick`: the main stick's path from the centered stick to what goes to the game (notch warp, snapping, ESS range, software dead zone, ESS map, then, ESS on or off, a pass that keeps each axis's in-game value, after Wii VC or SoH, from ever moving the opposite way to its own input, as the speedrun rules require). That pass is worked out for every stick position at once, into a table per settings and calibration, which takes a few milliseconds the first time each combination is used (around 50 at most). Near VC's length limit a position can be boxed in: x has to reach as far out as one neighbor and y as far out as another, and VC can't do both. Then the positions further in on the axis that fell short get bounded and the table is worked out again, until nothing is boxed in. The tests sweep every raw stick position for fixed and 150 random settings and calibrations, for each game. The bridge loop and the tests both call it.
- `src/notch.cpp` has notch calibration, the notch warp, and snapping. The warp splits the stick into 8 slices between neighboring notches and the center, and maps each slice linearly so its two calibrated notches land on their targets (the ideal notches plus any notch offsets).
- `src/main.cpp` has the windows, calibration, the settings file, and the bridge loop: read the adapter, center each stick, `process_stick`, send to the game.
- `src/resource.h` has the resource ids shared by `assets/resources.rc` and the code.
- `test/bridge_tests.cpp` has the tests.
- `assets/icon.svg` is the app icon: a GameCube control stick in its octagonal gate. `assets/icon.ico` is made from it and committed, so building doesn't need any image tools. `assets/resources.rc` puts it in the exe. After changing the SVG, regenerate the `.ico` with ImageMagick:

  ```
  magick -background none -density 384 assets/icon.svg -define icon:auto-resize=256,64,48,32,24,16 assets/icon.ico
  ```

## Licenses

The program is GPLv3 (`LICENSE.md`). The release zip has no separate license files. Instead, `assets/resources.rc` builds `LICENSE.md` and `assets/licenses/third-party.txt` into the exe, and the About window shows the credits, the no-warranty notice, and the source link, as GPLv3 asks of interactive programs. Its Licenses button writes both texts to a temporary text file and opens it in the default text editor.

`assets/licenses/third-party.txt` covers what ends up inside the exe:

- **libusb:** LGPL 2.1, built in from source. Its copyright lines come from libusb's `AUTHORS`, and the license text from its `COPYING`.
- **MinGW-w64 runtime and winpthreads:** only in MinGW builds, from MSYS2's `share/licenses/crt/COPYING.MinGW-w64-runtime.txt` and `share/licenses/winpthreads/COPYING`.

The GCC runtime needs no notice (GCC Runtime Library Exception), and MSVC's runtime needs none either. If you update libusb or change toolchains, update this file to match.
