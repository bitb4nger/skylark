# SKYLARK — flight school for the CHGame

A Pilotwings-style demake with simulator vibes, for the CHGame (Rev0, board package 0.3.0).

Each event has its own courses: pick one with **LEFT/RIGHT** on the title screen. Best scores are saved per course.

| Event | Course | What it is |
|---|---|---|
| **Light Plane** | Ring Course | 8 rings over the town and lake, then land on runway 09 (the last ring is the final-approach gate) |
| | Town Slalom | Low up the avenue between the downtown towers, back over the town, then land westbound on runway 27 |
| | River Run | 30 m up the winding river, skim the lake at 12 m, then round to the gate and land |
| **Hang Glider** | Thermals | Climb in the thermals to get through 5 rings, then land on the bullseye |
| | Cloud Street | Thermal to thermal across the country, then a long glide home to the target |
| **Jet** | Speed Gates | 10 big gates on a loop out past the edge of the map. Fly them in order; time counts |
| | Low Level | The downtown avenue at jet speed, a skim over the lake, a pass down the runway, then the last gate |
| **Free Flight** | | Take off from the runway at golden hour and go anywhere |

The town is **3D**: houses, a church and steeple, hangars, a control tower, and five glass towers downtown. Hitting one is a crash.

## Controls

| Button | Action |
|---|---|
| D-pad left / right | Roll |
| D-pad up / down | Pitch. Up is nose down, like a real stick. SELECT on the title screen flips it. |
| A / B (plane, jet) | Throttle up / down. B also brakes on the ground. |
| A at full throttle (jet) | Afterburner, for as long as you hold it |
| B at idle throttle (jet, in the air) | Speedbrake, for as long as you hold it |
| B (glider) | Airbrake, for losing height over the target |
| START | Pause. SELECT while paused quits to the title. Holding START for 3 s goes back to the SD menu. |

## Flying tips

- There are no attitude limits, so rolls and loops work. **Loop:** dive to about 110 knots at full throttle, then hold back-stick all the way round. **Barrel roll:** hold RIGHT with a touch of back-stick. While you're upside down the nose sags unless you push.
- Hands off, the plane holds about 1 g, which is level flight at your current speed. When you're upright it also eases back to wings-level. In a bank you sink a little, so hold some back-stick in turns. Hands off, the glider settles into its trimmed glide.
- Pull the nose too high and you **stall**: a warning sounds and the nose drops. Push forward and let the speed build back up.
- Glider: the variometer beeps faster and higher as you climb, and drones low when you sink hard. The bar on the right shows the same thing. Circle tightly (about 25–30° of bank) where it beeps.
- **Jet:** it turns hard (up to 9 g, the G meter is bottom left) and rolls fast, so small taps go a long way. It's slick: cruise needs only about 15% throttle, it climbs at 55° or steeper on dry thrust and straight up on afterburner (CLB, bottom left, is your climb angle), and it won't slow down by itself, so pull the throttle to idle and hold B for the speedbrake before the runway. Drag climbs steeply past about 420 knots, so it tops out near 460. Its gates count in order: a pass outside one is a miss and the next one lights up. Use the afterburner on the long legs, and come off it before tight turns.
- Landing: the plane needs under 3.4 m/s of sink, under 14° of bank, and the nose not pointed down. Cut the throttle on final and ease back just before touchdown. Softness and staying on the centreline both score.

## Scoring

Each ring is worth 10 points. The plane's landing scores up to 40, plus a time bonus of up to 30 that shrinks once you're past the course's par time. The glider's target scores 50, 30 or 10 by distance. The jet scores 10 per gate, plus up to 50 for finishing near par (it drops 2 points a second after that). A crash keeps your ring points. Best scores are saved per course. (Scores saved by the earlier version are reset by this one.)

## Upload: Arduino IDE (easiest)

1. One-time setup: under **File > Preferences > Additional boards manager URLs**, add
   `https://github.com/bateske/CHGame/releases/latest/download/package_chgame_index.json`.
   Then install **CHGame** in the Boards Manager.
2. Unzip so the folder is named `Skylark` and contains `Skylark.ino` and `src/`. Open `Skylark.ino`.
3. Set **Tools > Board > CHGame Boards > CHGame Rev0**. Leave Optimize at *Smallest + LTO* and Peripherals at *game*. Setting **Tools > USB** to *Upload only* is recommended.
4. Plug in over USB-C, pick the port, and press **Upload**. You don't need a driver or any buttons; it resets itself and starts.

## Upload: command line

```bash
arduino-cli compile --fqbn CHGame:ch32v:rev0 Skylark
arduino-cli upload  --fqbn CHGame:ch32v:rev0 -p <port> Skylark
```

## Upload: SD card menu

If you've burned the SD Text/Graphic Menu bootloader, copy `SKYLARK.CHG` into the card's `GAMES/` folder (FAT32) and pick it from the menu. *Sketch > Export Compiled Binary* in the IDE also rebuilds it, as `build/.../Skylark.ino.chg`.

## Files

- `Skylark.ino` — the game: the ray-cast ground (runs from SRAM), the flight model, the events and the HUD
- `src/Math3.h`, `src/Game.h` — vector math and types. These are headers so the IDE's prototype generator sees them first.
- `src/WorldData.*` — the terrain, generated by `python3 tools/make_world.py [--preview world.png]`. Don't edit these by hand. It packs the map into 240 tiles to leave flash for the 3D buildings and the jet.
- The courses are tables near the top of `Skylark.ino` (`P1_RINGS` … `J2_RINGS`, `COURSES`), and the buildings are `LANDMARKS` plus the houses `makeTown()` places on the painted roofs. The downtown towers' footprints in `make_world.py` and `LANDMARKS` must match.

**Flash is nearly full:** 50.3 KB of 50.9 KB. A few more course tables fit; bigger features will need something taken out.

**Simulator test hooks.** These compile only in the repo's PC simulator, never on the device. `-D SKYL_TRACE=1` prints telemetry. `-D SKYL_AUTO=1` adds a crude test pilot. `-D SKYL_TEST=1|2|3|4|5|6|7` starts on final approach, at the first thermal, near the target, in a 90° bank, heading straight at a tower, at the jet's last two gates, or over the town (the heaviest view for frame time).
