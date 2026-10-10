# Software ESS Adapter

A small Windows program that reads the official Gamecube controller adapter (WUP-028), runs the main stick through an ESS map, and sends the result to Dolphin. Use it to play Ocarina of Time (Gamecube/Wii VC) in Dolphin with N64-like stick response. No additional hardware is needed: just your Gamecube controller and the official adapter.

Dolphin's own adapter support reads the USB device directly, and nothing else can hook into that path. So Software ESS Adapter takes the adapter and gives Dolphin a DSU (cemuhook) controller over UDP on `127.0.0.1:26760`. Dolphin's DSU client is built in.

Building from source and how the code is laid out are covered in `DEVELOPMENT.md` in the source code.

## Setup

1. The adapter needs the WinUSB driver. If Dolphin already works with your adapter, it has it. If not, install it with [Zadig](https://zadig.akeo.ie/) as the [Dolphin guide](https://dolphin-emu.org/docs/guides/how-use-official-gc-controller-adapter-wii-u/) describes.
2. Start `Software ESS Adapter.exe`. The top line should say "Adapter connected".
3. In Dolphin, open Controllers. Set each Gamecube port you use to **Standard Controller**. Do not pick "GameCube Adapter for Wii U". Dolphin would then try to take the adapter itself.
4. Under Alternate Input Sources, enable DSU Client and add a server: description `Software ESS Adapter`, address `127.0.0.1`, port `26760`. The Software ESS Adapter window should now count a Dolphin client.
5. Copy `dolphin/Software ESS Adapter.ini` to `<Dolphin user folder>/Config/Profiles/GCPad/`.
6. Click Configure on port 1, pick device `DSUClient/0/Software ESS Adapter`, then load the `Software ESS Adapter` profile. Port 2 uses `DSUClient/1/Software ESS Adapter`, and so on.
7. Check that the Control Stick dead zone is 0 and that its calibration is the default. If you calibrated it before, open the menu on the Calibrate button and pick Reset Calibration. Don't calibrate this stick, because the ESS output already has the exact range the game expects.

## Using it

OoT speedrun rules don't allow you to learn more about your controller than looking at it would tell you. So the main window only shows whether the adapter, Dolphin, and each controller are connected. It shows no stick values, no plots, and no settings. A green "Active" light in the top right corner means your controller input is reaching Dolphin. It turns amber and says "Paused" while the settings window is open.

Settings, stick readouts, and plots are all in a separate settings window. Open it with Edit settings. While it's open, Dolphin gets a neutral controller on every port, so nothing you do on the controller reaches the game. Close it, or click Done, to go back to playing. Closing it also cancels any notch calibration in progress.

Each port's readout shows two pairs. "Stick" is your stick, measured from its resting center. "game" is the value OoT receives after Wii VC. Both use the usual convention: neutral is 0, 0, and right and up are positive.

Each adapter port has an ESS checkbox. Checked means the main stick goes through the ESS map, which is what you want for Ocarina of Time on VC. Unchecked passes the stick through untouched.

On a port without a notch calibration, the program takes the stick's center from the first report after you plug the controller in, like Dolphin and the console do. If the stick drifts, unplug the controller and plug it back in while not touching the stick. A calibrated port uses its saved neutral instead (see below).

The program does not do rumble.

## Dead zone

Sticks rarely come to rest at exactly 0, 0. With ESS on, a stick resting a unit off center reads 1, 1 or 0, -1 in game. The game ignores that, since its own dead zone is 7, but practice tools like gz show it on screen.

The Dead zone setting is in in-game units. N makes every stick position that would normally read N or less in game read exactly 0, 0. It's a circle around center on your physical stick, applied before notch calibration, the ESS range and ESS. It defaults to 2 and must stay below the ESS range start. 0 turns it off.

## ESS range

In Ocarina of Time, Link turns in place without walking when the stick's in-game value is 8 to 26 in a cardinal direction. Values 0 to 7 are the game's dead zone. These numbers come from the OoT decompilation. Link starts walking once the game's stick magnitude reaches 20, which is 27 on a cardinal.

The ESS range start and end are in-game values. They set how the range feels, not what the game receives. Setting 7 to 32 makes ESS start where the stick would normally read 7 in game and end where it would normally read 32. Everything in between is squeezed onto the game's real ESS values, so the range feels like 7 to 32. The defaults, 9 to 26, are what the stock ESS map already does, so leaving them alone changes nothing. The game's ESS range starts at 8, but VC never produces 8, so 9 is the first ESS value you can reach.

- **Wider range:** lower the start or raise the end. The stick is stretched along its distance from center, so everything below the start becomes dead zone and everything past the end is spread out to full tilt. Full tilt itself doesn't move.
- **Lower start:** this also shrinks the dead zone. Anything under the start reads as 0 to 7 in game.
- **Skipped values:** VC skips 8, 13, 17, 22, and 25. As settings, those behave the same as the next value VC can produce.
- **Exact on cardinals only:** the range is exact straight up, down, left, and right. Between the cardinals it's close, but not exact, because the game applies its dead zone to each axis separately.

The ESS range only applies to ports with ESS checked. Default puts both values back.

The settings window has two plots, each with a numbered dot per connected controller:

- **Your stick:** the dead zone and the ESS range as stick distances, inside the Gamecube's octagon. The dot turns blue while your stick is inside the ESS range.
- **In game (OoT on Wii VC):** the zones as OoT defines them, in the values it receives. ESS is a rounded square, because the game removes a dead zone of 7 from each axis and then needs the remaining magnitude under 20. The orange outline is where Link reaches full speed. The dot is the value VC actually delivers, and it turns blue when OoT counts it as ESS.

## Notch calibration

A controller's notches are rarely perfectly straight or at exactly 45 degrees. Notch calibration fixes that. Once a port is calibrated, the program reshapes the whole stick so every notch lands exactly where a perfect controller's notch would be, with smooth transitions in between. With ESS on, a calibrated notch reads exactly 80 straight or 70 diagonal in game.

To calibrate a port, open the settings window and click the port's Calibrate button. Then follow the prompt at the bottom of the settings window. There are 16 steps, alternating notch and neutral: Up, Neutral, Up-Right, Neutral, and so on clockwise to Up-Left, Neutral. For a notch, hold the stick in it and press A. For Neutral, let go of the stick and press A. B cancels. A green ring on the stick plot marks where the stick should go for each step.

All 8 neutral samples are saved, because a stick doesn't come back to exactly the same spot every time. Together they show where your stick can rest.

- **Neutral range:** any position between the smallest and largest sample on each axis counts as exactly neutral. It reads 0, 0 in the readout and in game.
- **Center:** the average of the samples. A calibrated port centers the stick there instead of on the first report after plugging in. Your center stays the same through replugs and power cycles, even if you touch the stick while plugging in. Outside the neutral range, positions and notches are measured from this center.
- **Per port:** calibrations belong to the adapter port, not the controller, so recalibrate if you move a controller to another port.

### Notch snapping

Notch snapping is optional and off by default. When it's on, a calibrated port that is within the snap distance of a notch reads exactly that notch, so the notch is a little easier to hit. The snap distance applies to every calibrated port and runs from 0 (off) to 10.

Calibrations and all settings are saved to `Software ESS Adapter.ini` next to the exe.

## Button layout on the DSU side

You only need this if you map buttons by hand instead of loading the profile. The names come from the DSU protocol, which copies a PlayStation controller.

To map the main stick by hand, uncheck that port's ESS box first and check it again when you're done. ESS output never goes past half deflection, because VC already treats that as full tilt, and Dolphin only registers a stick direction once it passes 55%.

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
