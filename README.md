# Software ESS Adapter

A small Windows program that reads the official Gamecube controller adapter (WUP-028), runs the main stick through an ESS map, and sends the result to Dolphin. Use it to play Ocarina of Time (Gamecube/Wii VC) in Dolphin with N64-like stick response. No additional hardware is needed: just your Gamecube controller and the official adapter.

Dolphin's own adapter support reads the USB device directly, and nothing else can hook into that path. So Software ESS Adapter takes the adapter and gives Dolphin a DSU (cemuhook) controller over UDP on `127.0.0.1:26760`. Dolphin's DSU client is built in.

Building from source and how the code is laid out are covered in `DEVELOPMENT.md` in the source code.

## Setup

1. The adapter needs the WinUSB driver. If Dolphin already works with your adapter, it has it. If not, install it with [Zadig](https://zadig.akeo.ie/) as the [Dolphin guide](https://dolphin-emu.org/docs/guides/how-use-official-gc-controller-adapter-wii-u/) describes.
2. Start `Software ESS Adapter.exe`. The top line should say "Adapter connected".
3. In Dolphin, open Controllers. Set each Gamecube port you use to **Standard Controller**. Do not pick "GameCube Adapter for Wii U". Dolphin would then try to take the adapter itself.
4. Under Alternate Input Sources, enable DSU Client and add a server: description `Software ESS Adapter`, address `127.0.0.1`, port `26760`. The Software ESS Adapter window should now say "Dolphin connected."
5. Copy `dolphin/Software ESS Adapter.ini` to `<Dolphin user folder>/Config/Profiles/GCPad/`.
6. Click Configure on port 1, pick device `DSUClient/0/Software ESS Adapter`, then load the `Software ESS Adapter` profile. Port 2 uses `DSUClient/1/Software ESS Adapter`, and so on.
7. Check that the Control Stick dead zone is 0 and that its calibration is the default. If you calibrated it before, open the menu on the Calibrate button and pick Reset Calibration. Don't calibrate this stick, because the ESS output already has the exact range the game expects.

## Using it

OoT speedrun rules don't allow you to learn more about your controller than looking at it would tell you. So the main window only shows whether the adapter, Dolphin, and each controller are connected. It shows no stick values, no plots, and no settings. The light in the top right corner shows what reaches Dolphin:

- **Active (green):** your controller input is reaching Dolphin.
- **Paused (amber):** the settings window is open, so Dolphin gets a neutral controller.
- **Idle (gray):** nothing is going to Dolphin. Either Dolphin isn't connected (check Setup step 4 and that Dolphin is running), or no controller is plugged into the adapter.

Only one copy of Software ESS Adapter runs at a time. Starting it again brings the open window to the front.

Settings, stick readouts, and plots are all in a separate settings window. Open it with Edit settings. While it's open, Dolphin gets a neutral controller on every port, so nothing you do on the controller reaches the game. Close it, or click Done, to go back to playing. Closing it also cancels any notch calibration in progress.

Each port's readout shows two pairs. "Stick" is your stick, measured from its resting center. "Game" is the value OoT receives after Wii VC. Both use the usual convention: neutral is 0, 0, and right and up are positive.

Each adapter port has an ESS checkbox. Checked means the main stick goes through the ESS map, which is what you want for Ocarina of Time on VC. Unchecked passes the stick through untouched. The checkboxes are saved.

Without calibrated notches, the program takes the stick's center from the first report after you plug the controller in, like Dolphin and the console do. If the stick drifts, unplug the controller and plug it back in while not touching the stick. With calibrated notches, it uses the saved neutral instead (see below).

The program does not do rumble.

### During a run

The settings window is for setup only. Set everything up and close the window before you start a run, and don't open it again until the run is over. OoT's rules require one mapping and calibration for the whole run, and the settings window shows stick values the rules don't allow during a run. While it's open, Dolphin also gets a neutral controller.

## Software dead zone

Sticks rarely come to rest at exactly 0, 0. With ESS on, a stick resting a unit off center reads 1, 1 or 0, -1 in game. The game ignores that, since its own dead zone is 7, but practice tools like gz show it on screen.

The Software dead zone setting uses the same in-game units as the ESS range: N covers the part of the stick that would normally read N or less in game, on both axes, and makes it read exactly 0, 0. It defaults to 2, which with the default ESS range turns a resting 1, 1 into 0, 0. It goes up to just below the ESS range start. At that point everything before ESS reads 0, 0, and ESS itself is never affected. 0 turns it off.

## ESS range

In Ocarina of Time, Link turns in place without walking when the stick's in-game value is 8 to 26 in a cardinal direction. Values 0 to 7 are the game's dead zone. These numbers come from the OoT decompilation. Link starts walking once the game's stick magnitude reaches 20, which is 27 on a cardinal.

The ESS range start and end are in-game values. They set how the range feels, not what the game receives. Setting 7 to 32 makes ESS start where the stick would normally read 7 in game and end where it would normally read 32. Everything in between is squeezed onto the game's real ESS values, so the range feels like 7 to 32. The defaults, 9 to 26, are what the stock ESS map already does, so leaving them alone changes nothing. The game's ESS range starts at 8, but VC never produces 8, so 9 is the first ESS value you can reach.

- **Wider range:** lower the start or raise the end. The stick is stretched along its distance from center, so everything below the start lands in the game dead zone and everything past the end is spread out to full tilt. Full tilt itself doesn't move.
- **Lower start:** this also shrinks the game dead zone. Anything under the start reads as 0 to 7 in game.
- **Skipped values:** VC skips 8, 13, 17, 22, and 25. As the range start, one of those behaves like the next value VC can produce. As the range end, it behaves like the previous one.
- **Exact on cardinals only:** the range is exact straight up, down, left, and right. Between the cardinals it's close, but not exact, because the game applies its dead zone to each axis separately.

The ESS range only applies to ports with ESS checked. Default puts both values back.

The settings window has two plots, each with a numbered dot per connected controller:

- **Your stick:** the game dead zone, the software dead zone and the ESS range as stick distances, inside the Gamecube's octagon. The dot turns blue while your stick is inside the ESS range.
- **In game (OoT on Wii VC):** the zones as OoT defines them, in the values it receives. ESS is a rounded square, because the game removes a dead zone of 7 from each axis and then needs the remaining magnitude under 20. The orange outline is where Link reaches full speed. The dot is the value VC actually delivers, and it turns blue when OoT counts it as ESS.

## Notch calibration

A controller's notches are rarely perfectly straight or at exactly 45 degrees. Notch calibration fixes that. Once the notches are calibrated, the program reshapes the whole stick so every notch lands exactly where a perfect controller's notch would be, with smooth transitions in between. With ESS on, a calibrated notch reads 81 straight in game, and 58 on both axes diagonally. Those are the furthest VC goes in each direction.

To calibrate a port, open the settings window and click the port's Calibrate button. Then follow the prompt at the bottom of the settings window. There are 16 steps, alternating notch and neutral: Up, Neutral, Up-Right, Neutral, and so on clockwise to Up-Left, Neutral. For a notch, hold the stick in it and press A. For Neutral, let go of the stick and press A. B cancels. A notch has to point within 22.5 degrees of its own direction and be at least halfway out. Otherwise the program says it's not that notch and asks again. A green ring on the stick plot marks where the stick should go for each step. When you're done, the prompt line says the notches were calibrated and saved, and the port shows "Calibrated" with a Clear button next to it. Clear removes the calibration.

All 8 neutral samples are saved, because a stick doesn't come back to exactly the same spot every time. Together they show where your stick can rest.

- **Neutral range:** any position between the smallest and largest sample on each axis counts as exactly neutral. It reads 0, 0 in the readout and in game.
- **Center:** the average of the samples. With calibrated notches, the stick is centered there instead of on the first report after plugging in. Your center stays the same through replugs and power cycles, even if you touch the stick while plugging in. Outside the neutral range, positions and notches are measured from this center.
- **Per port:** calibrations belong to the adapter port, not the controller, so recalibrate if you move a controller to another port.

### Notch snapping

Notch snapping is optional and off by default. When it's on, a stick within the snap distance of a calibrated notch reads exactly that notch, so the notch is a little easier to hit. It only works with calibrated notches. The snap distance is in stick units and runs from 0 (off) to 5.

Calibrations and all settings are saved to `Software ESS Adapter settings.ini` next to the exe. Don't confuse it with the Dolphin profile, `dolphin/Software ESS Adapter.ini`.

## Button layout on the DSU side

You only need this if you map buttons by hand instead of loading the profile. The names come from the DSU protocol, which copies a PlayStation controller.

To map the main stick by hand, uncheck that port's ESS box, close the settings window, and map the stick in Dolphin. Then open the settings window again and check ESS. Dolphin only gets a neutral controller while the settings window is open, so it can't see the stick then. ESS output never goes past half deflection, because VC already treats that as full tilt, and Dolphin only registers a stick direction once it passes 55%.

| Gamecube | DSU input in Dolphin |
| --- | --- |
| A, B, X, Y | Cross, Circle, Square, Triangle |
| Z | R1 |
| Start | Options |
| L / R analog | L2 / R2 |
| L / R click | L3 / R3 |
| Main stick | Left X/Y |
| C-stick | Right X/Y |
| D-pad | Pad N/S/W/E |

## License

Software ESS Adapter is free software under the GNU General Public License, version 3. Click About, then Licenses, in the program to read the license. The source code is at https://github.com/TorjeAmundsen/Software-ESS-Adapter.
