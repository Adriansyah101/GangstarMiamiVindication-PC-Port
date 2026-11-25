# Patch Notes

Every fix and addition in the port, compared with the original Mac game.

A new `savegame/GangstarMV.ini` turns the fixes and enhancements on: 60 fps, the graphics, draw distance 2, crowds
at 3, the gameplay, control and menu changes, and the add-ons. The intro still plays. A key left blank, or missing
from the file, keeps the Mac's behaviour, except `TIME_OF_DAY`, `RANDOM_TIME_OF_DAY`, `AI_STEERING_FIX` and
`BOAT_SEAT_FIX`, which are on. A name in `code` style is a key in that file, and most of them are also on
the **GAMEPATCH** tab in the game's options. `ORIGINAL_MAC_EXPERIENCE=1` plays the game as the Mac did: it keeps the
fixes for running on a PC and the loading speed-ups, and turns everything else off, the fixes of the original game's
own bugs included. [`FIXES.txt`](FIXES.txt) lists exactly what it changes, and [`CHANGELOG.md`](CHANGELOG.md)
describes every part of the port in more detail.

**Contents:** [PC fixes](#fixes-for-running-on-a-pc) • [Loading and speed](#loading-and-speed) •
[Original-game bugs](#bugs-in-the-original-game) • [High frame rates](#high-frame-rates) • [Graphics](#graphics) •
[The world](#the-world) • [Gameplay](#gameplay) • [Controls](#controls) •
[Menus and cutscenes](#menus-and-cutscenes) • [Music and add-ons](#music-and-add-ons) •
[Display, sound and start-up](#display-sound-and-start-up) • [Log and privacy](#log-and-privacy) • [Builds](#builds)

## V1.0.1

V1.0.1 adds two anti-aliasing options and a time-of-day choice, and fixes collision, AI driving, helicopter, traffic,
bloom and reflection bugs. The six new keys are on by default and off under `ORIGINAL_MAC_EXPERIENCE=1`.

### New Features

- **Alpha to coverage** (`ALPHA_TO_COVERAGE`; DISPLAY, ANTI-ALIASING 4X + A2C): with `MSAA` on, the edges of leaves,
  fences and railings are smoothed too, without getting thinner or thicker.
- **Post-process anti-aliasing** (`POST_AA`; GRAPHICS, POST AA): FXAA over the 3D picture, drawn under the HUD so text
  stays sharp. `auto` runs it only while `MSAA=0`.
- **Time of day** (`TIME_OF_DAY`; FIXES, TIME OF DAY TAB): a new SERVICES tab sets the clock an hour at a time, and
  the sky, lights and shadows follow at once. It is hidden during missions.
- **Random start time** (`RANDOM_TIME_OF_DAY`; FIXES, RANDOM START TIME): CONTINUE starts the city at a random time of
  day. NEW GAME and saves are unchanged.

### Fixes

- **Walk-through buildings:** with `DRAW_DISTANCE` above 1, houses, fences and trees near the map's centre could lose
  their collision after CONTINUE.
- **AI drivers weaving** (`AI_STEERING_FIX`; FIXES, ORIGINAL BUG FIXES): AI cars no longer weave on straight roads, as
  Clive's SWAT van did in ON A LION HUNT. This was a bug of the original game.
- **Clive's pose on his yacht** (`BOAT_SEAT_FIX`; FIXES, ORIGINAL BUG FIXES): Clive now sits at the wheel after the
  ON A LION HUNT cutscene instead of hovering over the seat in a swimming pose, a bug of the original game.
- **Helicopter rotor wear** (`PHYSICS_SYNC`): above 60 ticks/s a rotor touching a wall wore the helicopter down
  faster than on the Mac, so in MARKED ON THE BELLE CURVE it could burn out before leaving the alley.
- **Checkpoint markers and the bloom** (`BLOOM`): checkpoint domes, store and elevator markers and target arrows no
  longer glow over the road and the cars around them.
- **Car reflections** (`VEHICLE_REFLECTIONS`): cars keep their ENHANCED reflection in frames with dynamic shadows or
  sea reflections, and the mirrored seam across trunks and roofs is gone.
- **Sea reflections** (`WATER_REFLECTIONS`): no more crash with sea reflections on and ambient occlusion and bloom
  off, and nearby posts, boats and quays reflect more clearly.

### Improvements

- **A shorter options menu:** GAMEPATCH rows that belong together are now one row, such as ANTI-ALIASING (`MSAA` with
  `ALPHA_TO_COVERAGE`) and ORIGINAL BUG FIXES. Existing `GangstarMV.ini` values are kept, and `STOPLOCK_FIX`,
  `LOAD_SLEEP_SKIP` and `LOAD_BUDGET` are now set in the file only.
- **Less CPU time on NVIDIA:** the graphics enhancements keep their own GL state and draw the shadow and car
  reflection passes in one batch a frame, with the same picture.
- **Less CPU time on the main thread:** reads of the game's memory no longer ask the system about every address.
- **Traffic** (`TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY`, `PARKED_CAR_DENSITY`): extra cars and people spawn one per
  tick, stuck cars back up or are removed, and traffic keeps driving below a helicopter. A car you chase in a mission
  keeps its speed through traffic, except in races.
- **Bloom colours** (`BLOOM`): only the brightest parts glow, in their own colour, so walls, roads and the sky keep
  theirs.

## V1.0.0

The first release of the PC Port, for Windows and Linux (32-bit, from one source), on x86 processors only.
Everything below is in it.

### Fixes for running on a PC

These problems came from running a Mac game on a PC. They stay on under `ORIGINAL_MAC_EXPERIENCE`.

- **Fades that stayed invisible** (`TEXFIX`): the fade to the hospital after death and the map menu's fade turn the
  screen black again.
- **Cutscene fades** (`SWAP_COPY`): the fade to black before a cutscene shows properly, because the port keeps the
  last drawn frame as a Mac does.
- **Black flicker on the menus** (`NO_FLICKER`): only frames the game really drew are shown.
- **Crash within a few steps of moving** (`VTXFIX`): the game's own safety check is added on the path that lacked it.
- **Crash about 13 minutes into play** (`CRASH58_GUARD`): the crash becomes a logged no-op. The root cause is not
  proved.
- **Gas and brake dying after a full stop** (`STOPLOCK_FIX`): the game's 200 ms lock runs on the real clock. The bug
  is the original game's, but the port's even frame pacing is what made it show, so this fix stays on.
- **Game speed** (`FRAME_MS`): the game ran at the monitor's refresh rate, which broke the engine sound, box-climbing
  and more. It now runs on a steady clock: 30 fps as on the Mac, 60 by default, or another rate up to 400.
- **Sleep timing:** a 1 ms timer serves the game's sleeps.
- **Jagged edges** (`MSAA`): the Mac's 4x anti-aliasing is back. On graphics built into the processor, keep it at 4
  at most: on the built-in AMD graphics, 8 with every graphics option on sometimes kept the GPU busy for about 5 s on
  one frame, Windows then reset it, and the game stopped.
- **Blurry picture with display scaling** (`DPI_AWARE`, Windows): with scaling above 100%, the picture is drawn at the
  screen's real pixels instead of being stretched.
- **Random numbers:** the Mac's range of random values is restored, which had broken particle spread and chance rolls.
- **White square on the pause cursor:** the 2D renderers get the names the game gives them again.
- **AZERTY keyboards:** the game's own Z Q S D keys work on a French or Belgian keyboard, as on the Mac.
- **Stuck keys:** a key held while switching away with Alt+Tab does not stay pressed.
- **Library functions:** `assert`, `swprintf`, `opendir`, `NSClassFromString` and string functions were added or
  corrected, and shared stream and file code was made thread-safe.
- **Camera sticks:** the sticks turn the camera at the same speed at any frame rate.
- **Early bring-up fixes:** the black window at the start, the saved resolution read as 0x0, asset files that failed
  to open, the resolution picker crash, and the intro logo that never finished.

### Loading and speed

Nothing on screen changes with these, and they stay on under `ORIGINAL_MAC_EXPERIENCE`.

- **Scene loads** (`FAST_FTELL`, Windows): the worst freeze on the way to the menu went from 1765 ms to 258 ms.
- **Start-up hitches** (`FAST_FSEEK`, Windows): seeks inside a file's buffer are served directly, as the Mac's C
  library does.
- **Physics stutters while driving** (`FAST_MEMMOVE`): memory moves are 3.5-8x faster.
- **Models kept on the graphics card** (`STATIC_VBO`): time inside the draw calls fell from 185 to 77 ms a second.
- **Capped frame rate after loads** (`LOAD_SLEEP_SKIP`): the game's 16 ms sleep per tick after a load is skipped.
- **Jerky main menu** (`LOAD_BUDGET`): the city's collision is built in smaller steps, 200 items a tick instead of
  1500. Scene changes in the main menu stall for 31-83 ms instead of 116-219 ms.
- **Start-up loading** (`FAST_LOAD`): the "Loading" screen before the title takes about 0.7 s instead of 2.1 s.
- **Collision queries and the physics solver:** faster versions with results identical to the game's. Always on.
- **HUD health and armour bars:** no full UI search per update. Always on.
- **Billboard turn:** its range tests run first, with the same results. Always on.
- Together, in a busy scene with draw distance and crowds at 2, these took the test laptop from about 70 to about 113
  frames a second.
- **The effects' depth copies** (`SHARED_RESOLVE`): shadows, reflections, ambient occlusion and bloom share one copy
  of the screen's depth a frame instead of up to five; about 17 % off their GPU time.
- **The city past the fog** (`FOG_SKIP_FAR`): with `FOG_MODE` 1 or 2, what lies wholly past the fog's end is no longer
  sent to the graphics card.
- **The effects' questions to the driver** (`QUERY_FREE`): the bindings they change, and what the game's vertex arrays
  point at, are kept instead of asked for tens of thousands of times a second.
- **The game's vertices in its own memory** (`STREAM_VBO`): copied into a buffer of the graphics card's before each
  draw, so the driver does not do it inside the draw call; about 5 % more frames a second in a busy city scene on an
  NVIDIA GPU, the same on an AMD built-in one. Needs OpenGL 4.4.
- **Missing and reopened files** (`FILE_MISS_CACHE`, `FILE_POOL`): files the game looks for but the `.app` lacks are
  remembered, and the packs it reopens many times a second while you move are kept open.

### Bugs in the original game

The Mac game does these too. The fixes are on by default and off under `ORIGINAL_MAC_EXPERIENCE`.

- **Flash before the main menu** (`MENUFLASH_FIX`): two frames of raw 3D background no longer show between the title
  and the menu.
- **Main-menu clicks ignored** (`MENU_FADE_CLICKS`): clicks work during the menu's background fades.
- **Line at the screen's edge during fades** (`FADE_EDGE_FIX`): black fades cover the top row and left column too.
  With anti-aliasing on, the original game left a thin line of picture there.
- **Wrong radio logo** (`RADIO_LOGO_FIX`): the HUD shows the car's own station from the moment you start getting in.
- **Betty's things going to Johnny** (`SWITCH_SAVE_FIX`): a save made as Betty in free roam keeps Johnny's cash,
  armour, outfit and weapons.
- **Reginald's missions forgotten** (`MISSION_SAVE_FIX`): the original game keeps its mission flags in 8 groups, but
  its save writes only 7, so Reginald's five missions read as not done after every load. The save now keeps all 8,
  and the file still loads in the original game. Unlike the other fixes here, this one stays on under
  `ORIGINAL_MAC_EXPERIENCE`, so that a save made there keeps them and MISSION REPLAY still offers them once the preset
  is off.
- **No collision near the map's centre:** fixed when `DRAW_DISTANCE` is above 1, and left as on the Mac at 1.

### High frame rates

These matter most above 60 ticks a second.

- `PHYSICS_SYNC`: steps car and object physics once per tick, so driving stays smooth at any rate.
- `CAMERA_SYNC`: keeps the running camera sway as at 60 ticks a second.
- `WALK_SYNC`: stops walking characters in menus and cutscenes from flicking back and forth.
- `AI_SYNC`: stops running characters from being taken as stuck.
- `PRECISE_WAIT`: between ticks the pacer waits on a high-resolution timer instead of keeping a CPU core spinning for
  the last milliseconds; the ticks come at the same times. On by default, and under `ORIGINAL_MAC_EXPERIENCE`.
- `BACKGROUND_THROTTLE`: behind other windows the game draws at most 30 frames a second (`BACKGROUND_FPS`) and keeps
  running; minimized, at most 10 (`MINIMIZED_FPS`), with nothing drawn to the screen and a game in progress paused as
  with Esc. Off under `ORIGINAL_MAC_EXPERIENCE`.

### Graphics

- `SUN_MOON`: a sun by day and a moon at night with their glow, restored from the iPhone version's code and textures
  still in the Mac game. `2` draws the glow as the iPhone does, and `1` hides it behind buildings with a soft edge.
- `ANISOTROPIC_FILTERING`: roads and walls stay sharp at a distance.
- `AMBIENT_OCCLUSION`: soft contact shadows in corners and under cars. `AO_STRENGTH` and `AO_RADIUS` tune it.
- `BLOOM`: a soft glow around bright parts of the world. `BLOOM_INTENSITY` and `BLOOM_THRESHOLD` tune it. `BLOOM=2`
  adds light sprites and lit windows (`BLOOM_LIGHTS`, `BLOOM_WINDOWS`).
- `WATER_REFLECTIONS`: the sea reflects the city and the sky. `REFLECTION_STRENGTH` tunes it.
- `VEHICLE_REFLECTIONS`: shiny cars reflect the street, buildings and other cars, while matte cars stay matte.
  `VEHICLE_REFLECTION_STRENGTH` tunes it. Where nothing on screen lies along the reflection, as on roofs and trunks
  seen from above, the screen itself is used as a sphere map, looked up by the reflected direction.
- `DYNAMIC_SHADOWS`: vehicles and people cast shadows that follow the sun and the moon. At dusk and dawn a soft shadow
  stays under them.
- `SHADOW_QUALITY` (GAMEPATCH tab, GRAPHICS): HIGH as before; LOW is lighter for graphics built into the
  processor, with slightly softer shadow edges; AUTO (the default) picks by `GPU_PROFILE`.
- `GPU_PROFILE`: the settings left on auto (`MSAA`, `SHADOW_QUALITY`) follow the kind of GPU, found at start-up
  without slowing it: a GPU built into the processor gets the lighter shadows, a graphics card the full ones, and
  anti-aliasing is 4x on both. A value you set yourself always wins.
- `SPAWN_FADE`: cars and pedestrians, and their lights, fade in and out instead of popping.
- `FOG_MODE`: `1` is linear fog over the whole city in the sky's colour, and `2` is a hybrid fog by true distance that
  thins with height (`FOG_HEIGHT`). With either, the skyline fades in without a seam and the sea carries on past the
  map's edge.

### The world

- `DRAW_DISTANCE`: draws the city up to 4 times as far as the Mac. The collision tables grow with it.
- `TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY`, `PARKED_CAR_DENSITY`: 0 to 4 times the Mac's numbers, applied at once from
  the menu.
- `MINIMAP_SHOPS`: the minimap shows nearby weapon shops, car rentals and repair shops, with the pause map's icons.

### Gameplay

- `ALLOW_WEAPON_SPRINT`: keep your weapon in hand while you sprint, as on the iPhone. Two-handed weapons too.
- `MISSION_REPLAY`: a tab in the SERVICES menu lists the missions you have completed, with their giver, location, type
  and reward, and arrows switch between story, side missions (Reginald's included) and races. START plays one again.
  You keep the cash you earn; everything else goes back when it ends.
- `PLAYERSWITCH_INVENTORY`: in free roam, Betty shares Johnny's cash and weapons.
- `LC_SWITCH`: once every mission is done, a marker at L.C.'s hotel lets you play as him and back, as the game's own
  marker does for Betty. L.C. drives cars and motorbikes, and missions can be replayed as him.

### Controls

- Xbox, PlayStation and other pads work in play, with radial dead zones set by the kind of pad.
- `KEY_MOVE`: W/A/S/D walk diagonally, turns are smooth, and Alt walks instead of running.
- `PAD_LAYOUT`: a gamepad layout like GTA V's, on foot, in a car, on a motorbike (stick wheelie), flying and with a
  sniper rifle. In a car, holding A locks on for drive-by shooting. The CONTROLS page shows the layout.
- `PAD_MENU`: the pad works the menus: stick or D-pad moves the pointer, A clicks and B goes back. A pad button also
  skips the intro.
- `PAD_HINTS`: tutorial boxes name the pad's buttons while the pad is the last thing you used.
- `PAD_ICONS`: the options menu's pad pictures match an Xbox pad, DualShock 4 or DualSense.
- `SHOW_CURSOR`: keeps the system's pointer visible. Off by default.

### Menus and cutscenes

- **GAMEPATCH tab** (`GAMEPATCH_TAB`): most settings change from inside the game, by category, and most take effect
  at once. `ORIGINAL_MAC_EXPERIENCE` switches there with no restart, in both directions.
- `CUTSCENE_KEYS`: no on-screen skip button or arrows. Esc or Enter skips, and Space shows the next line.
- `TUTORIAL_KEYS`: Esc, Enter or Space answers tutorial popups. In a question popup, Enter or Space is YES and Esc is
  NO.
- `SHORT_WARNING`: the start-up warning lasts 3 s instead of 15, and any input skips it.
- `HIDE_MORE_GAMES`: removes the "More Games!" corner from the main menu.
- `HIDE_GAMELOFT_LIVE`: removes the Gameloft LIVE! badge from the main menu.

### Music and add-ons

- `CUSTOM_RADIO`: a sixth radio station plays your own music from the `custom radio` folder, shuffled (mp3, ogg, flac,
  wav, m4a, aac or wma). It follows the game's MUSIC volume, scaled by `CUSTOM_RADIO_VOLUME`, which also has a slider
  on the SOUND page, and the HUD shows the game's PLAY LIST / MY MUSIC logo for it.
- `RADIO_SONGS`: the nine songs of the iPhone version that the Mac does not have, on the electro, hip-hop and rock
  stations, raised to the Mac's loudness, and the Java version's two rock songs, from Gangstar: West Coast Hustle, on
  the rock station.
- `UC_CRUSHER`: the Crusher monster truck of Urban Crime, parked on the beach by L.C.'s hotel after ON A LION HUNT. It
  is drivable, with its own engine sound and exhaust smoke.
- The add-ons switch on and off in game with no restart.

`RADIO_SONGS` and `UC_CRUSHER` are not included. They need an add-on you make from your own copy of the versions they
come from.

### Display, sound and start-up

- **Full screen** (`FULLSCREEN`, `SCREEN_WIDTH`, `SCREEN_HEIGHT`): the game is drawn at the chosen resolution and
  scaled to the monitor, with black bars if the shapes differ. **Alt+Enter** switches to a window.
- **Window resizing:** a window you resize or maximize is filled, with the picture's shape kept and black bars where
  it differs.
- `SWAP_INTERVAL`: sets vsync.
- `NO_INTRO`: skips the intro movie. `INTRO_FLIP` flips it if a decoder shows it upside down.
- `NO_AUDIO`, `AUDIO_FRAMES`, `AUDIO_NB`: turn sound off or set its buffers. Sound follows the default output device
  when it changes.
- **Error box:** says why the game had to stop and points to the log. Always on.

### Log and privacy

- `LOG`: on by default. `cache/GangstarMV.log` stays on your computer until you choose to send it.
- The log writes your user folder, the program's folder and the game's folder as `%USERPROFILE%` (`$HOME` on Linux),
  `%EXE%` and `%APP%`, and a release log does not name your songs.
- **Crash dump:** after a crash, `cache/GangstarMV.dmp` is left for the bug report, with your folders, environment
  values and computer name blanked.
- `LOG=0` turns the log and the crash dump off.
- `SPIKE_MS`: off by default; set to a number of milliseconds, the log notes each frame that took longer, with where
  the time went.
- The port has no network code.

### Builds

- **Windows and Linux** from one source, with the same settings, saves and patch table. The Linux build has run in
  WSL2 (Debian 13) so far.
- **Release builds** have no debug mode: the traces, test aids, console and `GMV_*` environment overrides are not
  compiled in, and the program holds no path or name from the machine that built it.
- **Dev builds** keep every diagnostic, for people working on the port. Their log is much longer and names your songs.
- **No-icon builds** have no game art at all.

Everything the port does, by part, is in [`CHANGELOG.md`](CHANGELOG.md). Open problems are listed under
[Known Issues](README.md#known-issues).
