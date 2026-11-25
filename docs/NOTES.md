# GangstarMV PC Port: technical notes

The technical reference behind the [README](../README.md), which has the setup, the settings table and the known
issues.

- [How it works](#how-it-works)
- [Running and settings](#running-and-settings)
- [Source layout](#source-layout)
- [Settings reference](#settings-reference)
- [Fix records](#fix-records)
- [Diagnostics](#diagnostics)

## How it works

The game is a non-PIE i386 Mach-O linked at `0x1000` with no rebase info, so it must run at its
own addresses. The host exe is based at `0x10000` (the lowest address Windows allows) and its
first section reserves the whole game range so nothing else can be mapped there.

- **Image layout** is fixed in [`host/games/gangstar/layout.h`](../host/games/gangstar/layout.h) and enforced by the linker
  script [`host/core/gmv.ld`](../host/core/gmv.ld) + the reservation in [`host/core/reserve.S`](../host/core/reserve.S).
- Only the code below `0x11000` (the Mach-O header + early CRT) cannot sit at its own address,
  because the PE header page owns `0x10000..0x11000`. That range runs from a **copy at
  `0xC70000`** with every branch into/out of it patched exactly; a stray jump into the original
  range faults and a vectored exception handler ([`host/core/veh.c`](../host/core/veh.c)) redirects it.
- The exact low-range patches (4360) come from [`tools/genlow.py`](../tools/genlow.py) →
  `build/gmv_patch.bin` (CRC-locked to the binary, not to its location). Applied and
  byte-verified at load.
- Every Mach-O bind slot (4243 slots, 528 distinct imports) is written: to a host shim if
  implemented, else to a name-reporting thunk (code) or a no-access stand-in (data) so a missing
  import announces itself instead of corrupting silently.

### Running game code

A run loads and verifies the image and then runs the game, always under the VEH + page protections
so a fault is contained and reported, never silent. The plain and `-Dev` builds can stop earlier
(`--verify-only`, `--run-init`).

- **Static initializers** ([`host/core/runinit.c`](../host/core/runinit.c)): the 451 `__mod_init_func`
  entries are called one at a time; the VEH is told which one is live so a fault names it.
- **`main()`** (`--run-main`): runs all initializers, initializes the ObjC runtime and the NS
  string constants, then calls `main` with a minimal `argc/argv/envp/apple`.

### The Objective-C runtime ([`host/shims/shimsobjc.c`](../host/shims/shimsobjc.c) + [`.S`](../host/shims/shimsobjc.S))

The game ships 8 of its own ObjC classes in `__OBJC` (`GameDelegate`, `GameApplication`,
`GameWindow`, `EAGLView`, `MovieView`, `igphtmlmacViewController`,
`GLLivehtmlmacViewController`, `sendInfo`). No dyld ObjC runtime ever ran, so the
class-refs/message-refs still hold **pointers to the class-name / selector-name C-strings**, and
the metadata sits at its own addresses. On this basis:

- `gmv_objc_init()` parses `__OBJC` (`__module_info → symtab → class defs`) into a class
  registry. (Struct layout is in [`tools/objc1dump.py`](../tools/objc1dump.py).)
- `objc_msgSend` ([`.S`](../host/shims/shimsobjc.S)) reads `self`/`_cmd`, asks `gmv_objc_lookup` for the
  IMP, and **tail-jumps** to it — a game method (real code in `__text`), a Foundation stub, or
  `nil` for a no-op. `objc_msgSend_stret` (struct returns) and `objc_msgSendSuper` have their own
  asm entries + lookups (`gmv_objc_stret_lookup`, `gmv_objc_super_lookup`); the legacy
  `objc_exception_*` are stubbed (the real `@try` uses our `setjmp`/`longjmp`).
- A game instance's `isa` is the game's `objc_class`; a Cocoa instance we create gets a "fake
  class" descriptor tagged with its name. `+alloc` / `gmv_objc_new_instance` allocates
  `instance_size` bytes and sets `isa`. Compile-time `CFConstantString` literals are recognized
  as `NSString` instances and routed accordingly.
- Lookup for an instance: game methods (up the game superclass chain) → NSObject defaults →
  Foundation/AppKit stubs, **walking the superclass chain by name** — and through a hardcoded
  Cocoa hierarchy (`cocoa_super`: `QTMovieView→NSView→NSResponder→NSObject`, `NSWindow→…`) so a
  message to `GameWindow` resolves against the `NSWindow` stub, an `EAGLView`/`MovieView` against
  `NSView`, etc.

### Foundation / AppKit / Cocoa stubs ([`host/shims/shimsfnd.c`](../host/shims/shimsfnd.c), [`host/shims/shimsappkit.c`](../host/shims/shimsappkit.c))

Cocoa methods are host IMPs in `{class, selector, imp}` tables (class, instance, struct-return).
NSString objects are `gmv_objc_new_instance("NSString")` with the C-string pointer at offset 4;
`CFConstantString` literals keep theirs at offset 8, and `nsstring_cstr` handles both. Coverage:

- **NSString**: `stringWithFormat:`/`initWithFormat:` (shared printf-style `nsformat`, `%@`
  aware), `stringWith/initWithCString/UTF8String`, `substringToIndex:`, `hasPrefix:`,
  `stringByAppendingPathComponent:` / `AppendingString:`, `replacingOccurrences…`,
  `AddingPercentEscapes…`, `writeToFile:…`, `isEqual(ToString):`, `length`, `UTF8String`.
- **NSBundle** (real `.app` path, `resourcePath`, `pathForResource:ofType:`, `infoDictionary`),
  **NSUserDefaults** (the resolution and full screen live in `GangstarMV.ini`), **NSFileManager** (real Win32 file ops),
  **NSNotificationCenter** (real: stores observers, delivers on post), **NSWorkspace**,
  **NSThread** (`detachNewThreadSelector:` spawns a real thread), **NSURL/NSURLRequest/
  NSURLConnection/NSData** (inert — the IGP/network feature fails silently), **NSLocale**,
  **NSDate/NSDateFormatter**, **NSArray**, **NSWindow/NSView/NSScreen/NSColor/
  NSCursor/NSOpenGL\***, **QTMovie/QTMovieView** (a stand-in object; see below for the movie).
- `NSApplicationMain` ([`shimsappkit.c`](../host/shims/shimsappkit.c)) creates `GameApplication` as
  `NSApp`, creates `GameDelegate`, sends `-applicationDidFinishLaunching:`, then runs the loop: a
  Win32 message pump, the deferred `performSelector:…afterDelay:` queue, the scheduled `NSTimer`,
  `-[GameDelegate update]`. At frame 3 it plays `intro.mov` ([`shimsvideo.c`](../host/shims/shimsvideo.c),
  Media Foundation) and posts `QTMovieDidEndNotification`. The game's `startGame` then enters its own
  render loop and does not come back. Also AudioUnit (the render callback is captured for
  [`shimsaudio.c`](../host/shims/shimsaudio.c)), CoreFoundation and IOKit (fail-soft),
  `Gestalt`, `NSSearchPathForDirectoriesInDomains` (returns a writable dir **next to the exe**;
  logged as `[dirs] NSSearchPath dir=N -> …`. The game asks only for `dir=5` (Library) and funnels
  everything there — save, settings, and its `home/`,`tmp/` sandbox — so the dir alone can't split
  them. That dir maps to `cache\`, and the `fopen` shim then **redirects by basename**:
  `*.sav`/`*.preferences` → `savegame\`, leaving the scratch sandbox in `cache\`. Confirmed from the
  `[fopen]` trace — the save is opened `rb` on load then `a+b`/`r+b` as the game autosaves), NS
  string constants.

### C++ runtime / libstdc++ ([`host/shims/shimscxx.c`](../host/shims/shimscxx.c), [`host/shims/shimsstr.c`](../host/shims/shimsstr.c), [`host/shims/shimsstream.c`](../host/shims/shimsstream.c))

`__cxa_*`, RB-tree insert/erase/increment, `_List_node_base` ops, `std::allocator<char>` (ported
from libstdc++ 4.2); `std::string` (COW `_Rep` layout). `std::ostringstream` is **not** a real
stream reimplementation — [`shimsstream.c`](../host/shims/shimsstream.c) keeps a per-stream **accumulator
keyed by address range**: `std::ios_base::ios_base()` (always out-of-line) registers a buffer in
a window around the object, `operator<<` appends to whichever buffer contains the ostream
pointer, `str()` returns a COW `std::string`, and the `__ZTV*`/type_info vtable data symbols are
inert (never dispatched — all stream ops are out-of-line to our shims).

### Rendering ([`host/shims/gl/`](../host/shims/gl/), [`host/games/gangstar/render/`](../host/games/gangstar/render/))

`ensure_gl()` creates an SDL3 window (the chosen resolution, or SDL's borderless full screen over the
monitor), a 32/24/8 double-buffered pixel format and a GL context; verified live on OpenGL 4.6 (NVIDIA RTX 3050 Ti).
`NSOpenGLContext` `makeCurrentContext`/`flushBuffer` map to `SDL_GL_MakeCurrent`/`SDL_GL_SwapWindow`. On Windows the
format comes from a hidden helper window, because SDL can only ask for one by its attributes and `SWAP_COPY` needs
`PFD_SWAP_COPY`: with the format SDL chooses, the driver took over a full-screen window, and screen captures froze
(seen on NVIDIA 617.14). A window the player resizes keeps the screen FBO at the size the game asked
for and `msaa_present` scales it with black bars, as in full screen: the game sets its main viewport only a few times a
run, so it could never follow the client area (measured on the Win32 window too: a larger window showed the
old size in its corner). `gl_window_fit` moves a window only for a new size, so the game asking the same one again
leaves the player's size alone; a new resolution, or coming back from full screen, still sizes it. During a drag Windows
keeps the main thread inside `SDL_PumpEvents`, so the game stops; SDL still sends expose events from that loop, and an
event watch (`redraw_watch`, shimsinput.c) presents the FBO's last frame again (`gmv_gl_redraw`), only during the pump, as
mid-frame calls such as `SDL_SyncWindow` send them too and the FBO holds a whole frame only at the pump. **All GL imports are
bound**: a `_gl*` import resolves (lazily, on first call, once a context exists) via
`gl_real` (below), wrapped in a per-function thunk
(`mov eax,fn; jmp gmv_gl_forward`). `gmv_gl_forward` is a generic cdecl→stdcall adapter: it
copies a fixed 64 bytes of args into a fresh frame, calls the stdcall GL function (which pops its
own args), and restores `esp` from `ebp` so the arg count need not be known.

**One GL loader.** Every GL function the host looks up, the game's imports and the enhancements' own,
comes from `gl_real(name)` in [`shims/gl/gl_bind.c`](../host/shims/gl/gl_bind.c), declared in `shims.h`, on both systems.
It is `SDL_GL_GetProcAddress`, which asks the driver first (`wglGetProcAddress`, `glXGetProcAddress` or
`eglGetProcAddress`) and then the GL library. No loader library such as GLAD is used: the host needs names, not
generated prototypes, and SDL already does the lookup. Two additions:

- Some Windows drivers answer 1, 2, 3 or -1 for a name they do not hand out, and SDL 3.4 passes that on as a function
  (it tests only for NULL). `gl_real` counts those as no answer.
- On Windows, a name with no answer is then taken from `opengl32.dll`, where GL 1.1 lives.

Before, Windows asked `opengl32.dll` first and the driver second. Compared on the RTX 3050 Ti over 7 scenarios, every
name got the same function except 13 GL 1.1 ones (`glBindTexture`, `glDrawArrays`, `glDrawElements`, `glGenTextures`,
`glDeleteTextures`, `glTexSubImage2D`, `glPolygonOffset`, the four array pointers and the client-state pair). NVIDIA's
driver hands those out itself, so they now go straight to it instead of through opengl32's jump to it. No name was
left unresolved and no driver gave 1, 2, 3 or -1. On Linux nothing changed: it already went straight to SDL.

### Note on the abandoned "slide" approach

An earlier idea slid the whole image (`tools/genlink.py` → `build/gmv_link*.bin`) but needed
~64k heuristic relocations (1713 unfilled weak-bind slots, 184 unslid jump tables) with
silent-corruption risk. It was dropped in favour of running the image at its own addresses.
`genlink.py` and `gmv_link*.bin` remain only as a fallback and are not used by the current host.

## Running and settings

**Just double-click `GangstarMV.exe`** (it carries the game's icon) — with no arguments it launches
the game directly. The dev build also opens a console window for the stage log (closing the game
closes both); the release build (`host\build.ps1 -Release`) has no console and only writes `cache\GangstarMV.log`.

The staged flags work only in the plain and `-Dev` builds. A release build takes only `--app`.

```powershell
.\GangstarMV.exe                 # no args (same as a double-click): load, patch, bind, then run the game
.\GangstarMV.exe --verify-only   # stop after load+verify, no game code (the old default; --no-run is an alias)
.\GangstarMV.exe --run-init      # run all 451 static initializers (or `--run-init N` for the first N)
.\GangstarMV.exe --run-main      # explicit: run all initializers, then main() → the game runs
.\GangstarMV.exe --app "<project root>\GangstarMV.app"   # if the .app isn't found automatically
```

All runtime files are located relative to the **exe's own folder** (not the working directory), so a
double-click works from anywhere: `gmv_patch.bin` next to the exe, the log in `cache\`, the save and
`GangstarMV.ini` in `savegame\`, and the bundle via `find_app` (next to the exe, then `..\GangstarMV.app`,
the project root). Everything is logged to `cache\GangstarMV.log`. Reports: `[MISSING] ... import <name>`,
`[objc] unhandled message [cls sel]`, `[fault] ...` + `while running ...` — each stops cleanly
with the exact cause.

**Game data (portable).** The game's writable data lives next to the exe, created on demand:
`build\savegame\` holds the save game (`Gangstar2.sav`), the settings (`Gangstar2.preferences`),
and `GangstarMV.ini` (an older copy next to the exe is moved there automatically), and `build\cache\` holds the game's own
sandbox (`home\`, `tmp\`) and the log (`GangstarMV.log`). To wipe progress/settings, delete those
folders.

**Settings: `GangstarMV.ini` (in `savegame\`).** Every host switch, including the bug fixes
(`TEXFIX`, `SWAP_COPY`, `NO_FLICKER`, `VTXFIX`, `CRASH58_GUARD`, `STOPLOCK_FIX`), frame pacing,
audio, mouse, intro and all the diagnostics, is a key in this file. The file is created with
the built-in defaults and a one-line description of each key on first run, so the game behaves
the same with or without it; delete it to restore the defaults. Keys are case-insensitive, the
`GMV_` prefix is optional, and a blank value means "default". A key is read in whichever
`[section]` it stands, so a file written before a key moved keeps its value; when the key appears in
its own section as well, that line wins, and the keys taken from elsewhere are listed in one `[config]`
line. The original-game fixes `MENU_FADE_CLICKS`, `RADIO_LOGO_FIX` and `SWITCH_SAVE_FIX` sit in
`[Fixes]` (older files have them in `[Input]`, `[Audio]` and `[Gameplay]`), and the speed-ups that
leave the picture and rules unchanged (`FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE`, `STATIC_VBO`,
`SHARED_RESOLVE`, `FILE_MISS_CACHE`, `FILE_POOL`, `QUERY_FREE`, `STREAM_VBO`, `FOG_SKIP_FAR`) in
`[Advanced]` (older files have them in `[Fixes]`). Unknown keys and malformed lines are reported as
`[config]` lines in `GangstarMV.log`, together with every non-default value in effect. In a plain or
`-Dev` build an environment variable `GMV_<key>` overrides the file for one run, so every `GMV_*` name in these notes is also an ini key. A
release build ignores the environment and leaves out the `[Testing]` and `[Diagnostics]` keys, along
with the code behind them; its `LOG` key sits in a `[Log]` section (item 30). Lookup: `gmv_getenv` in [`host/core/config.c`](../host/core/config.c), which also
holds the key list and the template text. The resolution and full screen are the `[Display]` keys
`SCREEN_WIDTH`, `SCREEN_HEIGHT` and `FULLSCREEN`; the game's own Video options write them there
the moment they change (shimsfnd.c's NSUserDefaults shim). An older build's `gmv_prefs.txt` is
moved into the ini on the first run and deleted. In full screen the game draws at the chosen
resolution, as on the Mac (its `setBackingSize::` sets the GL surface to it and `fitWindowSize`
switches the display to that mode): the screen FBO has that size and each present scales it into
the borderless window with its shape kept and black bars (`gmv_gl_render_size`,
`gmv_gl_view_rect` and `msaa_present` in `shims/gl/gl_msaa.c`; the mouse maps through the same rectangle).
The screen FBO is therefore made at any `MSAA` (without samples at 0).

`ORIGINAL_MAC_EXPERIENCE=1` (section `[Preset]`) plays the game as on the Mac without editing the
other keys. The table is `mac_preset` in [`host/games/gangstar/settings.c`](../host/games/gangstar/settings.c), and it goes ahead of the
file and, in a dev build, of any `GMV_*` environment variable:

- **Stay on:** `TEXFIX`, `SWAP_COPY`, `NO_FLICKER`, `VTXFIX`, `CRASH58_GUARD`, `STOPLOCK_FIX`,
  `FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE`, `STATIC_VBO`, `DPI_AWARE`, the loading
  speed-ups `LOAD_SLEEP_SKIP`, `LOAD_BUDGET` (200) and `FAST_LOAD`, and `SHARED_RESOLVE`,
  `FOG_SKIP_FAR`, `QUERY_FREE`, `FILE_MISS_CACHE` and `FILE_POOL` (item 35), `STREAM_VBO` (item 37) and
  `PRECISE_WAIT` (item 38).
- **Set to the Mac's:** `MSAA` 4, `DRAW_DISTANCE` 1, `FOG_MODE` 0, and `TRAFFIC_DENSITY`,
  `PEDESTRIAN_DENSITY` and `PARKED_CAR_DENSITY` 1.
- **Set to their defaults:** `FRAME_MS` and `SWAP_INTERVAL` (60 ticks/s, vsync off), `FOG_HEIGHT`.
- **Forced off:** `PHYSICS_SYNC`, `CAMERA_SYNC`, `WALK_SYNC`, `AI_SYNC`, every fix of an original-game bug
  (`MENUFLASH_FIX`, `FADE_EDGE_FIX` and `MENU_FADE_CLICKS`, `RADIO_LOGO_FIX`, `SWITCH_SAVE_FIX`), `SOLO_CAR`,
  `ANISOTROPIC_FILTERING`, `AMBIENT_OCCLUSION`, `BLOOM`, `WATER_REFLECTIONS`, `VEHICLE_REFLECTIONS`,
  `DYNAMIC_SHADOWS`, `SUN_MOON`, `SPAWN_FADE`, `HIDE_MORE_GAMES`, `HIDE_GAMELOFT_LIVE`, `SHORT_WARNING`, `CUTSCENE_KEYS`,
  `TUTORIAL_KEYS`, `ALLOW_WEAPON_SPRINT`, `MINIMAP_SHOPS`, `MISSION_REPLAY`, `PAD_ICONS`, `PAD_LAYOUT`,
  `PAD_MENU`, `PAD_HINTS`, `KEY_MOVE`, `PLAYERSWITCH_INVENTORY`, `LC_SWITCH`, `UC_CRUSHER`,
  `CUSTOM_RADIO`, `RADIO_SONGS`, `NO_INTRO`, `SHOW_CURSOR`, `BACKGROUND_THROTTLE` (item 38).

Each value it overrides is logged as a `[config] ... overridden` line. `FULLSCREEN`, the screen size,
`INTRO_FLIP`, `NO_AUDIO`, the audio-buffer, diagnostic and testing keys are left as set.

`GAMEPATCH_TAB` is on by default, and blank or a missing key counts as on (`gmv_config_tab` in
[`host/core/config.c`](../host/core/config.c)). Under `ORIGINAL_MAC_EXPERIENCE=1`, where the tab holds only that
switch, the hooks of the enhancements are not in at all, and the collision tables and spawn pool are the
Mac's. The switch needs no new start (`mac_hooks_in`, `mac_hooks_out`, `mac_features_in` and
`mac_features_out` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)):

- Going off, the hooks go in, the running game's collision tables grow (`gmv_bp_grow` in
  [`host/games/gangstar/features/broadphase.c`](../host/games/gangstar/features/broadphase.c)), the pool moves and gets its extra copies, which can take
  a moment.
- Going on, the bytes the game shipped with come back at once, with these exceptions: `DRAW_DISTANCE`
  follows after the next cell update, a copy still out in the city is let go when it returns (item 27), and during
  a mission replay the other hooks stay until it ends. The collision tables of the world in use shrink to
  the Mac's sizes after that cell update, as soon as what is in use fits them with room to spare
  (`gmv_bp_shrink`).

What stays in under the preset: the fixes it keeps on, and what the switch itself needs, since these are
built once at the start: the options tab, the SERVICES menu's replay tab (hidden), and the menu file's
additions. Those are the tab's lines and the Custom Radio rows (below the stage, the game's rows where the
file has them).

Rows that set several keys (`OM_JOIN` in [`host/games/gangstar/patch/tab.c`](../host/games/gangstar/patch/tab.c))
write each of their keys and show CUSTOM while the keys hold a mix the row has no choice for. V1.0.1 joined such
rows: FIXES went from 25 rows on 5 pages to 13 on 3, GRAPHICS from 16 to 14 and MENU from 7 to 5. `STOPLOCK_FIX`,
`LOAD_SLEEP_SKIP` and `LOAD_BUDGET` left the menu and are set in the file only. No key was renamed.

`UC_CRUSHER` and `RADIO_SONGS` follow the switch and their menu lines at once, because with the add-on
there, their files are read at every start, whatever the keys say and under the preset too. (So that
neither ever needs a new start. A game started under the preset then behaves as
the Mac's, with no truck, hooks or new songs, but with the add-on there it has read the longer sound, station
and type lists and the merged animation bank. Without the add-on it reads the Mac's files.) Going on, the
truck leaves the city once no menu is up, and a player at the wheel carries on in a D340 (the Mac's `Pickup`)
at the same place and speed.

`GAMEPATCH_TAB=0` leaves all of it out.

`[Graphics]` holds the anti-aliasing the Mac game used and optional enhancements beyond it.

`MSAA=4` (what the default, `auto`, picks on any GPU) restores the Mac's multisample anti-aliasing. `-[EAGLView
initWithFrame:shareContext:]` (0x199bd0) asks `NSOpenGLPixelFormat` for Multisample,
SampleBuffers 1, Samples 4 (the list at 0x5f6de0) unless `-[GameDelegate isOldDevice]` picks the
list without them (0x5f6e40). That flag is set at 0x19e06e only for eight early Intel-graphics
models in `hw.model` (Macmini1,1 to iMac10,1), so the Mac version ran with 4x MSAA on any other Mac,
and so does the port, whose `sysctl` reports no model. The port's window format was
single-sampled, so the anti-aliasing was lost until this setting. The game's screen is now a
multisampled FBO of the window's size (`msaa_*` in [`host/shims/gl/`](../host/shims/gl/)), with RGB8
colour like the window format and depth 24/stencil 8. Binding framebuffer 0 binds that FBO, and
the binding queries still answer 0. Each present resolves the FBO and copies it to the back buffer,
and the FBO keeps its contents between presents as the Mac back buffer did. The game's
`glReadPixels` and `glCopyTexSubImage2D` of the screen read the resolved copy. `MSAA=0` turns it
off, and 2 or 8 pick another count, capped at `GL_MAX_SAMPLES`. `[msaa]` lines log the count and
screen size.

**MSAA 8 on built-in graphics.** On an AMD GPU built into the processor, with `MSAA=8` and all enhancements on, the game sometimes stopped. The log
showed one frame of about 5 s. Then the driver dropped the game's GPU memory, and the game hung,
or a NULL `glGetString` crashed `strstr`. That matches Windows resetting a GPU that took too long
(TDR). VRAM was not full: the 8x screen with its depth and glow layer is about 200 MB. Eleven
scripted drives (`zz-msaa-*` scenarios, deleted after the hunt) narrowed it down:
- Hangs and crashes came only with ambient occlusion on, in 3 of 6 runs. The AO pass reads the
  depth of every pixel many times, and at 8x each pixel has 8 samples.
- AO off: 4 of 4 runs passed, but with the glow layer on, some screenshots were black except
  for the glow, HUD included.
- `MSAA=4` with everything on: 20 minutes clean, no black frame, longest frame 0.25 s.

Nothing was changed in the code. This is neither an original nor a port bug: the chip is too slow
for that much work a frame. The ini description, FIXES and README say to keep 4 on built-in
graphics, and `MSAA` on `auto` picks 4 (item 38). The preset is not affected (it uses 4).

`ANISOTROPIC_FILTERING=N` (2-16) sharpens textures seen at a shallow angle, such as roads and walls
in the distance. The engine already sets `GL_TEXTURE_MAX_ANISOTROPY_EXT` per texture to
min(the texture's level, the GPU maximum) in its texture-state code (0x501010), and that stays the
default. The setting wraps `glTexParameterf`/`glTexParameteri` (`af_*` in
[`host/shims/gl/`](../host/shims/gl/)) to raise every level the game sets, and every texture given a
mipmapped minifying filter, to at least N, capped at the GPU maximum. `[aniso]` lines log the level
in effect and the levels the game itself asks for.

`AMBIENT_OCCLUSION=1` adds screen-space ambient occlusion, which the Mac version never had. It is
applied between the 3D world and the 2D layer, so the HUD, map and text are not darkened (`ao_*` in
[`host/shims/gl/`](../host/shims/gl/)). The engine is fixed-function and loads its projections with
`glLoadMatrixf`. The only `glOrtho` call, at 0x19a6fd, belongs to the EAGLView. So the switch is
read from the projection matrix: a perspective matrix has w depending on z. On the screen, after
enough depth-tested perspective draws, the AO goes in before the first draw under a
non-perspective projection. If the frame has no 2D layer, it goes in before a depth clear or the
present instead. It runs at most once per presented frame, so the frames the game keeps on screen
between draws, as in fades, are not darkened twice. The pass works in four steps:

1. Copy the depth buffer to a texture.
2. Compute AO at half resolution from view-space positions, using the inverse of the scene's
   projection. The engine can draw parts of a frame under different projections and depth ranges
   (`setDepthRange` calls `glDepthRange`), so one depth buffer can hold several layers. Each
   depth-writing perspective draw is counted under its projection and depth range. The AO uses the
   pair most draws had and leaves pixels from the other layers alone.
3. Blur it twice without crossing depth edges.
4. Multiply it into the screen with depth-aware upsampling.

The AO fades out with the game's fog. All the GL state the pass touches is saved and restored.
`AO_STRENGTH` (default 1.2) scales the darkening, and `AO_RADIUS` (default 60) sets the reach in
world units, which are about 1 cm (the scene's near plane is 50 and the fog starts at 45000). `[ao]` lines log the scene's near and far planes, the fog, and where the pass was
applied.

`BLOOM=1` (LOW in the options menu) adds a soft glow around the bright parts of the 3D world, such
as neon, street lights, headlights and the sun. `BLOOM=2` (HIGH) also makes the game's light sprites
and the lit windows on the buildings glow, at `BLOOM_LIGHTS` and `BLOOM_WINDOWS` (blank = 1, 0 = not
at all). The Mac version never had it. It runs at the same point as the AO, after
it, inside the same state save (`bl_*` in [`host/shims/gl/`](../host/shims/gl/)):

1. Resolve the screen's colour and depth to textures.
2. Run a bright pass at half resolution. The cut is 0.74 + 0.26 x `BLOOM_THRESHOLD` (default 0.75,
   0-0.95), 0.17 lower at night by the game's clock, with a soft knee 0.65 of the remaining headroom
   wide on each side. Each tap is weighted down by its brightness, so single bright pixels do not
   flicker. On the world the brightness is the luminance (Rec. 709 weights) plus 0.35 of the spread
   between the strongest and the weakest channel, at most the strongest channel: grey walls and
   pavement are judged by their luminance, and a saturated neon sign or tail light, dark by
   luminance alone, still gets through. On the sky it is the dimmest channel, so the sun and
   its glow bloom but a sunset or a blue sky does not wash out to white. A pixel counts as sky when it is at the far end of the depth range
   most draws used, or beyond it.
3. Spread the glow over six levels: dual-filter downsamples, then tent upsamples, each added onto
   the level above at 1.08 times, so the widest levels weigh a little more.
4. Screen-blend the sum into the frame (1 - (1 - frame)(1 - glow)) at `BLOOM_INTENSITY` (default 1.0),
   with a dither against banding. Where the sum passes 1 in a channel, it is divided by its largest
   channel and moved at most a fifth of the way toward white instead of being cut per channel, so a
   strong orange glow stays orange, not yellow or white.

The game draws in 8-bit colour, so the glow comes from how bright a pixel is on screen, not from
light values. With only the brightest channel, a sunset sky turned near-white, which is why the
sky is judged on its dimmest channel. `[bloom]` lines log the size, the number of levels and the
settings.

`WATER_REFLECTIONS=1` makes the sea reflect the city and the sky. Neither the Mac nor the iPhone
version has any reflection in its sea: it is a translucent textured sheet at height 0 that writes no
depth. The reflections are screen-space, and they run at the same point as the AO, before it and
the bloom (`sea_mark_*` and `ssr_*` in [`host/shims/gl/`](../host/shims/gl/)):

1. Each chunk of sea sets a stencil bit where it is drawn. A chunk is alpha-blended, writes no
   depth, lies flat at sea level and is at least 50 m across. The game's stencil state is saved
   around the draw.
2. On those pixels the water point is where the eye's ray meets height 0. Pixels where something
   drawn is nearer than that point are left alone. Four travelling waves ruffle the normal, fading
   out where a wave would be only a few pixels long.
3. The reflected ray is marched across the screen against the depth of the city's near pass,
   perspective-correct, with a binary search at the first crossing. A hit takes its colour from the
   frame as drawn so far.
4. A ray that hits nothing takes the screen where its direction meets the sky. Off screen it takes
   the sky's colour at eye level (with `FOG_MODE` 1 or 2), or fades out.
5. The reflection is mixed in by Fresnel's law for water: 2% looking straight down, rising towards
   grazing angles. It is scaled by `REFLECTION_STRENGTH` (default 1.0, 0-2) and thinned with the
   sea's own fog.

Only what is on screen can be reflected, so the tops of tall buildings above the screen and
anything behind the camera are missing. `[ssr]` lines log the passes.

`DYNAMIC_SHADOWS=1` draws shadows that follow the sun and the moon, in place of the game's own, a ready-made
shadow-mesh under each model that is the same at any hour (`ds_*` in [`host/shims/gl/`](../host/shims/gl/),
`gmv_ds_node` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)):

1. **The game's own go.** The vehicle update (0x112bc4) and the character update (0x76090) set the shadow-node's
   visibility each tick while it is visible. Those two sites (0x112c3a, 0x775db) now hide it and keep it in a
   list. Once the new shadows stop, the nodes whose owners the world or a pool still holds are shown again.
2. **What casts.** Those same updates give the owner, so every scene node under it goes in a table (two tables,
   swapped every 30 frames). A draw is a caster if the node being drawn is in the table (`gmv_ds_caster`, through
   the `SPAWN_FADE` list hooks).
3. **The map.** Casters draw into a 2048x2048 depth map around the camera, 60 m each way (1024x1024 with
   `SHADOW_QUALITY=1`, item 36).
4. **The pass.** It runs first in `ao_run`. Each pixel's position comes from the frame's depth, as for the AO, and
   is looked up in the map with 16 filtered taps (8 with `SHADOW_QUALITY=1`). Casters set a stencil bit and take a
   larger bias, because the
   bias a car's own surface needs would slide its shadow away from the feet of whatever stands on the ground.
5. **The light.** The sun's strength is 0.45, the moon's 0.30. The sun is above the 3% line from about 5.05 h to
   16.9 h and the moon from about 18.1 h to 3.2 h, so for part of the day neither is up. A low light's direction is
   drawn towards straight up and its strength towards 0.40, so as it sets the shadow shortens to a soft one under
   the thing, with no second map, and the next light draws it out again (`ds_light`).

Buildings do not cast, so a car under a bridge still gets a sun shadow. A vehicle off screen casts nothing. People
without a shadow-mesh cast only through their shiny-draw test. Moon shadows
also show under street lights. `[shadow]` lines log the passes: casters, source, direction, hour and strength.

The frame's first caster gives the map its view. When that caster's view cannot be found (`ds_begin`), it stays out of
that frame's map and the next caster is asked, so it costs that one shadow for one frame, or every shadow if no caster
gives a view. In a run with about 14400 maps, 1123 casters in 838 frames gave no view. For 204 of them, in 198 frames,
the node was not in the table and the caster was within the map's 96 m; the nearest was 11 m. 13 frames had no shadows
at all, 2 of them with such a caster within 96 m. One of those frames had 69 casters, none with a missing node, and is taken
to be a camera cut. Dev builds print a summary of
this at most once a minute, and when the Mac switch puts the game's own shadow back, the log gives the reason for each
node left out.

`SUN_MOON` brings back the sun and moon of the iPhone version (Gangstar2, the same engine): `2`
(IPHONE) draws the glow as the iPhone does and `1` (ENHANCED) adds the depth handling and the gain
described below. The Mac game has no sun or moon in the sky, but most of the iPhone code and all four textures
(`add-sun.tga`, `add-moon.tga`, `add-glow.tga`, `add-glow-moon.tga`) are still in it. The anchors
in `sun.bdae` cross the sky with the time of day. The time-of-day update (0x142892) still places
two glow billboards towards them, and the skybox render (0xbe468) still draws a sun disc and a moon
disc around the camera, hidden below the horizon. Two pieces were removed. The skybox constructor
never makes the discs, and the update hides both glows every tick, where the iPhone shows the sun
glow by day and the moon glow at night (time state 3). The setting puts both back (`apply_sun_moon`
in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)). Each disc is made as the iPhone makes it, a
1024x1024 billboard of the texture, and the game's own tweaks "draw skybox sun" and "draw skybox
moon" still switch each one. Then the glow for the time of day is shown. A `[sunmoon]` line logs
the discs made for each skybox.

The glow's material has no depth test, and it is drawn after the world in the world's depth range,
so as shipped it would shine through buildings. The discs come before the world, since the skybox
draws them, but the skybox draws them last among its layers: after `sky-mesh` and after
`farcity-mesh`, the distant skyline. They have no depth test either, so the moon would sit in front
of the skyline. The iPhone's skybox render (0x2bfef4) draws the discs before the skyline, so both
modes replace the skyline draw (0xbe9f1) with a call that draws the discs
first and the skyline over them, then skips the game's own disc draws. The skyline texture is a
cutout (sky fully transparent, buildings opaque), so the moon disappears only while it is behind a
building. Its arc peaks at about 27° above the horizon, so around moonrise and moonset it is
often behind the skyline.

The iPhone's glow uses the same material as the Mac's: material 8 of `DefaultEffects.bdae`, "Unlit
Textured Additive Blend No Depth No Culling", and the file's contents are identical in both versions. So
`SUN_MOON=2` keeps that material's own draw, glow through buildings included, with the Mac's texture as it
is. The Mac's glow textures are DXT5 with visible rings and the iPhone's PVRTC ones have none, so the rings
stay in this mode; the smoothing described below is `SUN_MOON=1`'s only. It has not been compared with a
real iPhone.

With `SUN_MOON=1`, the billboard render (vtable 0x676a70) is
wrapped so that a glow's draw can be told apart. Its fixed-function shading (texture times vertex
colour, added ONE/ONE) is replaced by a small shader that does the same and reads a copy of the
depth drawn so far. Where that is sky (the far end of the world's range or beyond), the glow is
drawn, at 1.6 times the material's brightness (`GLOW_GAIN`). Where it is the world, the glow leaks
a little way in past the silhouettes, like light wrapping around an edge, and fades out 3% of the
screen height inside them (`GLOW_WRAP`). Thin
things like poles and rotor blades get most of it. The glow textures are DXT5, whose 5- and 6-bit
colours turn the gradient into about a dozen visible rings. So the first time each glow is drawn, its
texture is read back, blurred in floating point by a Gaussian of about 1/60 of its width, and kept as
a 16-bit copy that the shader reads instead. The shader also dithers its output by less than one
8-bit step, so the screen's own 8 bits per channel cannot make new rings. If the shader cannot be used, a plain depth
test (LEQUAL, no depth writes, all fragments at the far end of the world's range) hides the glow
behind the world with a hard edge ([`host/shims/gl/`](../host/shims/gl/), `glow_begin`).

The glow counts the skyline as sky, since the skyline is drawn in the sky's depth range, so it
stays when the moon is behind a skyline building. At night those buildings are dark navy, and the
glow added over them looks like plain sky. The result is a glow with no moon in it.

`DRAW_DISTANCE=N` (more than 1, up to 4) draws the city N times as far as the Mac version. How far
the game draws comes from a device profile, `Structs::PhonePerformanceProfile`, read at start-up
(0x1a8c5e) from `_packMac/file002743.dat` into 0x949a00. The file has two profiles. The phones'
profile is 15000/30000, and the Mac takes the other one:

- `perfFarHighRes` 45000 (about 450 m) is the full-detail city. It sets the camera's far plane each
  frame, the start of the fog and the window of full-detail cells streamed around the player
  (1.5 times the distance).
- `perfFarLowRes` 90000 is the far end of the low-detail band drawn behind it and of the fog.

The setting scales both right after the read, before the city is built, so all of these move
together (`apply_draw_distance` in [`host/games/gangstar/patch/`](../host/games/gangstar/patch/)). The profile also sets
how far away cars and pedestrians are removed (1.5 times `perfFarHighRes`, at 0x76c59). That check
is pointed at a copy of the Mac's profile, so traffic comes and goes as on the Mac. A
`[gamepatch] draw distance` line logs the distances in effect. The world's share of the depth
buffer stays the same, so the further it reaches, the coarser the depth steps at the far end.

The buildings' collision consists of Box2D shapes. The city grid (`CCustomBatchGridSceneNode`)
creates them for the objects in the cells within `perfFarHighRes` of the camera, so the setting
affects collision in two ways.

The first is the starting rectangle. `updateCells` (0x13d5a4) adds a cell's objects when the cell
comes into range. It decides that by comparing with a rectangle around the last position (+0x1a4),
which a new or reset grid sets to (0,0,0). On the first update, every cell within that distance of
the world origin therefore counts as already added. At 3 times the distance this covers most of
the city, and those buildings get no collision; you walk through them. The origin collision fix
(item 13) mends that and is applied only with this setting. The Mac
game has the same starting rectangle, but it only covers about ±225 m around the origin.

The second is table size, since a window N times as wide loads N² times as many shapes. Right after
the profile read (0x1a8c79), the game sizes the Box2D broadphase from `perfFarHighRes`. Above 7500
it gets 16384 proxies (0x649680) and 131072 pairs (0x649684, also the size of the pair hash table).
The menu scenes alone come close to 16384 at 3 times, and neither table checks for being full: more
shapes would overwrite the heap. The setting therefore also:

- scales both sizes by N², rounding the pairs up to a power of two;
- guards the two `b2BroadPhase::CreateProxy` calls, so a full pool gives the shape no proxy (as
  Box2D does for a shape outside the world) and logs `[gamepatch] collision proxies full` instead
  of corrupting memory;
- logs `[gamepatch] collision in use` lines at 1024, 2048, 4096, ... proxies, showing how full the
  tables get.

The fog's distances follow `DRAW_DISTANCE` (an enhancement; the game's own under
`ORIGINAL_MAC_EXPERIENCE`): with `FOG_MODE` 1 or 2 from 100 m to 800 m at 1x, with `FOG_MODE` 0
from 450 m to 900 m at 1x, the end 50 m further for each 0.5x more. The game sets linear fog
every tick in its time-of-day update (0x142892):
- **Start:** `perfFarHighRes`, where the full detail ends.
- **End:** the low-detail far distance (0xb5a00) plus 2/3 of the low-detail band.
- **Values:** 450-1200 m on the Mac, 1350-3600 m at `DRAW_DISTANCE=3`. The fog is 60% dense where
  the city's geometry ends, which hides that edge.

The pair goes to the shader parameter setter 0x54cec0 through the call at 0x142d5a, which
`apply_fog` redirects to substitute ours, from the draw distance in force, so a change in the
options menu moves the fog at the next tick. `[gamepatch] fog` logs the distances and the game's own
at the first tick and after each change. Past the fog's end nothing can be seen.

`FOG_MODE` picks how the fog is drawn (an enhancement; `0` under `ORIGINAL_MAC_EXPERIENCE`):
- **0:** the game's own fog, on only some materials, as on the Mac, at the distances above. This is the value of a blank key; a new ini sets 2.
- **1:** linear fog over the whole city, as below.
- **2:** the hybrid fog (below), with `FOG_HEIGHT` for its thinning with height.

With `FOG_MODE` 1 or 2, the fog covers the whole city. In the game, fog is a per-material
switch: bits 12-13 of the render-state word, read at 0x4fc871, where 0 means `glDisable(GL_FOG)`.
Most of the city's materials have it off, on the Mac too: buildings, roads and the sea.
- **What gets the fog:** `fog_force_begin` in `render/fog.c` draws every perspective, depth-tested draw
  to the screen that the game left unfogged with the game's linear fog.
- **What stays clear:** the sky dome, the sun and moon discs, the glows, the 2D layer and the game's
  own framebuffers. For the sky, `patch/fog_distance.c` wraps the skybox render 0xbe468 (vtable entry 0x6522d8)
  to mark its draws.
- **Blended draws:** additive ones fade towards black and multiplying ones towards white, so they
  thin out rather than turn into fog-coloured shapes.
- **Fog colour:** the game's fog colour is a near-white that drifts away from the sky at dusk and
  at night, so the fogged city showed as a pale band against the sky. The fog now takes the sky's
  own colour at eye level instead, for both the forced fog and the game's own.
  - **Where it comes from:** the sky gradient is a wall kept in front of the camera. Its vertex
    colours change with height and are symmetric about eye level.
  - **How it is read:** `sky_read` reads those colours and the wall's position as the sky is drawn,
    every frame.
- **Where the world ends:** past `FOG_END` the city is only fog-coloured shapes, and they covered
  the distant skyline's foot. When the fog ends before the drawn city does, world draws to the
  screen are clipped at the fog's end, at the depth from the eye that the fog counts (a user clip
  plane in eye space, `GL_CLIP_PLANE5`). This also spares the GPU the hidden city.
  - **Log:** `[fog] the world ends at ... m`.
- **Skybox height:** the skybox render (0xbe468) places the sky gradient and the distant skyline
  around the camera at height 0, which is the sea, 600 m out.
  - **The problem:** from a helicopter the skyline sinks below where the sea fades out, about 14°
    under the horizon at 150 m up. The city drawn out past it then rises over the skyline with its
    square edge. Lifted to the camera's height, the skyline floats over a band of haze instead.
  - **The fix:** with the whole-world fog, the instruction at 0xbe5c6 that zeroes the height goes to
    a trampoline that stores the camera's height × (1 − 600 m / D). D is where the world is seen to
    end: the fog's end, or the city's far end if that comes first. The skyline's foot, 600 m out, is
    then seen at the same angle as the sea D away, so it stands where the fogged sea ends at any
    altitude. On the ground it is at sea level, as on the Mac.
  - **Log:** `[gamepatch] skybox raised with the camera`.
- **The sea past the map's edge:** the city's sea fills a box from x −1207 to 1196 m and y −1104 to
  1461 m, and stops there. Near the map's edges and from the air that edge was in view well inside
  the fog, a straight line with nothing past it.
  - **The fix:** after the first chunk of sea on the box's edge in each pass of a frame, the sea goes
    on past the box out to beyond the camera's far plane. It uses that chunk's state, the edge's deep
    blue, and the texture laid as the sea lays it (fitted to the chunk's own texture coordinates, so
    it scrolls with them). The fog then takes it into the sky's colour.
  - **Per pass:** the city is drawn in a near pass and a low-detail far pass, each with its own depth
    range. At first the sea was carried on only once per frame, and that first chunk could be the
    far pass's. Then the whole extension lay past the fog's clip and the box's edge showed again, a
    slanted line across the sea about 70% into the fog (`DRAW_DISTANCE=2`, `FOG_END=800`).
  - **Log:** `[fog] the sea goes on past the map's edge`. The code is `sea_beyond` in `render/fog.c`.
- **Below the horizon:** the sky gradient repeats the sky upside down below eye level, which showed
  as an orange or dark void wherever no world was drawn. Those colours are now the fog's colour, as
  fully fogged sea would be.
- **Distant skyline (farcity):** it has no real distance. The mesh is a half cylinder around the
  camera with Z up and its foot where the world is seen to end, and it lies past the end of the drawn
  city.
  - **At its foot:** it takes the fog of the city's far end, so the city's fogged edge runs into it
    without a seam. `render/fog.c` works out where the city ends from the game's own fog end:
    far = (3·end + 2·start)/5.
  - **Higher up:** the fog thins to 60% of that (`FOG_FARCITY`) by about 8° above the horizon
    (`FOG_FARCITY_RISE`), so the tall buildings stand out of the haze. Its colour there turns to the
    sky's right behind each point, found by taking the point's direction into the sky's own space
    and following it to the wall. Below eye level that is the fog's colour.
  - **How it is drawn:** a small shader repeats the game's material (unit 0 texture × vertex colour;
    the alpha test stays fixed-function) and adds the fog. Without shaders, the skyline gets 60%
    throughout through a constant fog coordinate.
- **Log:** `[gamepatch] fog on the whole world` and `[fog] the skyline: ...` confirm it.

The hybrid fog (`FOG_MODE=2`) answers two faults of the linear fog:
- **It counts depth along the view:** the fog is the same at a screen edge as in the middle, so it
  shifts as the camera turns.
- **It stops dead at its end:** the end shows as a line where the fogged sea meets the skyline.

Each fogged material is drawn through a fragment shader instead (`fog_hybrid_begin` in `render/fog_hybrid.c`).
The fixed-function vertex stage keeps the game's lighting and texture matrices, and the alpha test
and blending stay as they were.
- **Distance:** the true distance from the eye at each pixel, from its depth (`gl_FragCoord` through
  the inverse projection, as the AO does). It is right across the sea's triangles, some of them
  almost 1 km across.
- **Curve:** an Exp2 curve fitted between `FOG_START` and `FOG_END`:
  clear = (e^(−a·t²) − e^(−a)) / (1 − e^(−a)) × (1 − t⁸). Here t runs from 0 at the start to 1 at
  the end, and a = 3 at sea level. It eases in at the start and reaches complete fog at the end
  without a kink.
- **Height:** a scales with the mean of e^(−height / `FOG_HEIGHT`) along the line of sight
  (default 60 m; 0 = the same at every height). The sea and the streets haze sooner than the tops of
  the towers, but every height is complete at `FOG_END`.
  - **Camera height:** the skybox trampoline also stores it (`g_cam_z`).
  - **Up direction:** taken from the sky's orientation (`sky.inv`).
- **End:** nothing is drawn past `FOG_END`. The clip plane counts depth, which reaches further at
  the screen's edges, so the world ends on the circle where the skyline's foot stands.
- **Materials:** three shaders cover every setup a survey of the city's draws found (`FOG_SURVEY`, a dev
  diagnostic since taken out):
  - plain colour;
  - unit 0 `MODULATE`;
  - unit 0 `MODULATE` plus unit 1 `COMBINE`, which adds its colour and multiplies its alpha (lit
    windows and reflections).
- **Texture environment:** each unit's is tracked through the `glTexEnv*` calls, so picking a
  shader asks GL nothing.
- **Fallback:** any other setup keeps the linear fog, logged once as
  `[fog] hybrid fog: a setup without a shader ...`.
- **Log:** `[fog] hybrid fog, frame ...` counts the draws each way every 10 s.

## Source layout

`host/` has a part that knows no game and a part for each game:

```
host/
  CMakeLists.txt      the build, for Windows and Linux; CMakePresets.json names the variants, cmake/ the 32-bit
                      Linux toolchain
  build.ps1           calls CMake for one game: -Game <folder under games\>, gangstar by default
  linux/              build.sh and build_stage1.sh, which call CMake too; stage1_stubs.c
  core/               the loader: Mach-O, image, imports, exception handler, log, settings file
  shims/              the macOS libraries, for any game
  shims/gl/           the GL window, its present and the GL imports
  games/gangstar/     layout.h, game.c, settings.c
    patch/            what is patched into the game's code, a file for each feature
    render/           the graphics enhancements, a file for each effect
    features/         the larger speed-ups and add-ons
  games/template/     a game with no patches, the starting point of a new one
  third_party/SDL3/   from tools\get_sdl3.ps1, not in the repo: official 32-bit build, one pinned version, checked by hash
```

[`host/core/game.h`](../host/core/game.h) is everything `core/` and `shims/` ask of a game: the settings table, the
patches at the start, and hooks for files, sound, input, the frame and each GL draw. A hook is an ordinary function,
called directly. Nothing else crosses the line: `core/` and `shims/` hold no game address in their code, and
`build.ps1 -Game template` links them with no Gangstar source, which the build check repeats. Their comments still
cite Gangstar's addresses where they explain why a shim is as it is.

### Building with CMake

`host/CMakeLists.txt` is the only build. `build.ps1`, `linux/build.sh` and `linux/build_stage1.sh` keep their old
arguments and only call it (build folders `build/cmake/<game>-<kind>`, `linux-` in front on Linux); `BUILD=dev` or
`BUILD=release` before `linux/build.sh` gives Linux the dev and release builds, and `-DGMV_STAGE1=ON` is the loader
alone. CMake 3.25 or newer for the presets, and Ninja (`linux/*.sh` fall back to make without it). Directly:

```
cmake --preset windows            cmake --build --preset windows            (like build.ps1)
cmake --preset windows-dev        ...                                        (build.ps1 -Dev)
cmake --preset windows-release    ...                                        (build.ps1 -Release)
cmake --preset linux              cmake --build --preset linux              (like linux/build.sh)
```

`-DGMV_GAME=<folder>` picks the game (gangstar by default on both systems), `-DGMV_NOICON=ON` leaves the icon out and
`-DGMV_OUT_DIR=<folder>` sends the program elsewhere; the presets' build folders go under `build/cmake/`. On Windows
the presets and `build.ps1` put MSYS2's `mingw32\bin` first on the PATH (`pacman -S mingw-w64-i686-cmake
mingw-w64-i686-ninja`); on Linux `cmake/linux-i386.cmake` adds `-m32` (`apt install cmake ninja-build`).
`compile_commands.json` in the build folder serves editors. `tools/linux_check.ps1` stays a separate check: clang and
a Debian sysroot from Windows.

Checked when CMake took over, against the scripts' own gcc commands: on Windows the release, dev and
template builds are the same bytes apart from the PE time stamp and checksum, and the plain build the same after
`strip` (the source order is the old `build.ps1`'s, case-insensitive). The old `linux/build.sh` took the game's folders
in `find`'s order, so on Linux the link order and a few bytes of padding differ from it; the symbol set is the same,
neither holds a local path, both reach the menu with the same log apart from addresses and timing, and two CMake builds
of one source are the same bytes but for the build id. The stage-1 loader verifies with the same log as before.

The CI workflow (`.github/workflows/build.yml`) builds through CMake in two jobs, with no game files and any warning a
failure: Windows (MSYS2 mingw32: gangstar and template, release and dev) and Linux (a `debian:trixie` container, as
Ubuntu's runner has no 32-bit SDL3 development files: the same four and the stage-1 loader, against the system's SDL3
3.2.10 headers since `third_party/` is not in a checkout).

### Adding a game

1. Copy `games/template` to `games/<yourgame>` and fill in its `layout.h`: the end of the binary's `__LINKEDIT`, the
   names, the Objective-C classes, `__text`.
2. Make the patch table: `python -I tools\genlow.py <binary> build\<yourgame>\gmv_patch.bin`.
3. `host\build.ps1 -Game <yourgame>`, then `<yourgame>.exe --verify-only` reports the load and lists the imports with
   no shim. Add those to `gmv_game_host_symbols` in the game's folder, or to `shims/` if any game could use them.

The game must be a non-PIE i386 Mach-O. This was tried with the binary of one other Mac game: it loads, 44961 low-range
patches apply and verify, and 407 of its 581 imports bind. Doing that led to two changes here: rebase info is ignored,
as nothing slides, and `genlow.py` can end the low range at a called function when the first named one is too far. That
game has not been run on this host. Only Gangstar has.

### Towards Linux

The aim is one source for Windows and Linux. Done so far: [`shimsaudio.c`](../host/shims/shimsaudio.c),
[`shimsjoy.c`](../host/shims/shimsjoy.c), [`shimsinput.c`](../host/shims/shimsinput.c) and
[`shimscg.c`](../host/shims/shimscg.c) use SDL3 only and include no Windows header, and the window and GL context are
SDL3's.

The loader builds for both. [`core/platform.h`](../host/core/platform.h) is all `core/` asks of the system, in UTF-8
paths, answered by `platform_win32.c` and `platform_linux.c`; [`fault.c`](../host/core/fault.c) says what a fault means,
and `veh.c` or `fault_linux.c` get it from the system. On Windows the game's range is a section of the exe, by
`gmv.ld`. On Linux the program is an ordinary non-PIE one linked at 0x08048000, and maps the range with
`MAP_FIXED_NOREPLACE` as it starts, so no link script is needed; the link address matters, since lld's default of
0x400000 lies inside the range. `vm.mmap_min_addr` is 0x10000 by default, the same limit as Windows, so the layout and
`gmv_patch.bin` are the same on both. The game cannot use `%gs` on its own, or it would not run on Windows, so glibc's
use of it is no conflict. `crashdump.c` has a Linux counterpart, `crashdump_linux.c`; `loadprof.c` has a part for each.

The shims build for both too. [`shims/compat.h`](../host/shims/compat.h) is `windows.h` on Windows, so that build is
unchanged, and on Linux the same Win32 names over pthreads, `clock_gettime`, `stat` and `dlsym`: an `SRWLOCK` is a
plain mutex there, and a thread `HANDLE` is joined by `WaitForSingleObject`. GL functions come from `gl_real` on both
systems (see "One GL loader"). Where Linux has the Mac's own call, `shimsw.c` has a Linux body beside the Windows one (mmap,
stat, opendir, nanosleep to the microsecond, gettimeofday, localtime, pread). Three things differ for real:

- glibc's `pthread_cond_t` is 48 bytes and the Mac's 28, so on Linux the game's condition holds a pointer to one on the
  heap; the mutex (24 + 4 bytes) still fits the Mac's 44;
- C code on Linux expects the stack 16-byte aligned at each call, so `shimsobjc.S`'s dispatchers make room before
  calling C (`ALIGN_CALL`), and `gmv_missing_import` realigns itself; GL is cdecl there, so a GL thunk jumps straight to
  the driver with the game's own arguments instead of going through `gmv_gl_forward`;
- the build needs `-freg-struct-return`, as the Mac returns small structs in registers (mingw does by default); glibc
  functions that return a struct (`div`) must not be called from the host.

Linux has no decoder that every system carries, so the intro movie and the custom radio's m4a, aac and wma songs come
from the system's `ffmpeg` program ([`ffpipe_linux.c`](../host/shims/ffpipe_linux.c)): the host starts it with `posix_spawnp` and reads
raw frames or samples from a pipe. It runs `ffmpeg` and `ffprobe` as programs rather than loading FFmpeg's libraries, so
a 64-bit `ffmpeg` of any version serves the 32-bit host. The intro takes the movie's size and frame rate from
`ffprobe`, then one `ffmpeg` sends BGRA frames at that rate and a second sends 48 kHz stereo; the queue, the pacing and
the drawing are the Media Foundation path's own (`show_front`, `intro_start`, `intro_end`, `intro_path`). A song goes
straight to 16-bit stereo at the output's rate. Without `ffmpeg` the intro is skipped and the custom station leaves out
those songs (mp3, ogg, flac and wav need no `ffmpeg`: see item 18); the log says why once.

On Linux FAST_FTELL and FAST_FSEEK are off, as glibc already keeps the offset and seeks inside its buffer (a
`LOAD_PROFILE` run with FAST_FTELL on loaded the menu's scene in the same time), and DPI_AWARE has no effect. An NSAlert is an SDL message box with the alert's own
button titles. The help texts in `settings.c` write paths with backslashes; `config.c` writes them with `/` into a
Linux `GangstarMV.ini`, and the words that differ (the program, the home folder as `$HOME` in the log) come from macros
there.

`host/linux/build.sh [game]` builds the host with gcc through CMake (default `template`), `host/linux/build_stage1.sh`
the loader alone with stubs, which stops after verifying with `--verify-only`. Both need `-D_FILE_OFFSET_BITS=64`: without it a
32-bit `stat` or `readdir` fails with EOVERFLOW on a file whose inode number is above 32 bits, which is every file on
WSL's `/mnt/c` and many on btrfs or xfs, and the game is "not found". `tools/get_linux_sysroot.py` (Debian trixie i386:
libc, GL, SDL3 3.2.10) + `tools/linux_check.ps1 [-Game g] [-Stage1]` compile and link the same from Windows with clang
and lld; the SDL3 functions the host uses are all in 3.2.10, or the link would fail.

**First run on Linux (WSL2 Debian 13, gcc 14.2, Mesa 25.0.7):** WSL2 has to be set up once (`wsl --install
-d Debian` from an administrator, then a restart of Windows), and inside it `dpkg --add-architecture i386` and
`gcc-multilib libsdl3-dev:i386 libgl-dev:i386 libgl1-mesa-dri:i386`. The loader's `--verify-only` log is the Windows
one apart from the lines that are each system's own (the exe's mapping, DEP): the same segments, 4360 patches, 451
initializers and protections. The template host, given the Gangstar binary through a `Template.app` of links, runs the
game to the title and the main menu: its window, GL with 4x MSAA, SDL3 audio, keys, timers and threads all work, and the
fault handler passes its self-test; the imports it lacks are the Windows build's own 49. WSLg has no GPU driver for a
32-bit program (`/usr/lib/wsl/lib` is 64-bit only), so GL is Mesa's llvmpipe in software (about 27 frames a second at
1920x1080).

**`games/gangstar` on Linux:** its 75 files that included `windows.h` include `compat.h`, and
[`compat_linux.c`](../host/shims/compat_linux.c) answers the few calls they make: `VirtualQuery` from
`/proc/self/maps`, `VirtualProtect` by `mprotect`. Paths it builds use `GMV_SEP`. A Windows path search for `\` had
crashed the Crusher's set-up, and the game's `./` in front of an absolute path in the bundle is now dropped
(`gmv_in_bundle`). The pad pictures draw their letters with FreeType and fontconfig, opened with `dlopen` (Liberation Sans
Bold where the Windows build uses GDI's Arial). Calls from assembly name C symbols through `GMV_ASM_SYM`. Packages:
those above, plus `libfreetype-dev:i386` to build and `ffmpeg` for the intro and the custom radio's m4a, aac and wma. Run in WSL: the
intro (220 frames, upright, its sound played by the log; not heard), the menu, the city on foot and in a car, the pause menu, the Crusher, the radio
songs, a mission replay, L.C., the DualSense pictures, a custom radio song from a file, and every graphics enhancement
(bloom, AO, shadows, the two reflections, the fogs: each logs its first pass as on Windows). Not tried yet: a fault in the
game's own code through the signal handler (`TEST_FATAL`'s faults in the host ran, see the crash dump), seeing the alert
box (the process waited on it after a crash), hearing the custom station in a car, and a real GPU (only a Linux on
the machine itself has one for a 32-bit program). Nothing ties the host to Windows any more: the crash dump and
`LOAD_PROFILE` were the last.

The window icon: when `games/<game>/app.ico` is present, `linux/build.sh` builds it into the program (`.incbin`, as
`app.rc` puts it in the exe; `NOICON=1` leaves it out) and `gl_context.c` hands its six 32-bit pictures to
`SDL_SetWindowIcon`, the 256 px one with the others as its smaller sizes. In WSL the taskbar showed the game's icon.

`LOAD_PROFILE` and `PERF_PROFILE` on Linux: no thread can read another's registers there, so the sampler thread sends
the main thread a real-time signal (`SIGRTMAX - 3`) about once a millisecond, and the handler takes the sample from
the registers it interrupted, with the same stack scan (its one guarded read is `gmv_readable`, a system call). Code is
named by `dl_iterate_phdr` and `dladdr` (`libc.so.6!fn+off`, `linux-gate.so.1` for the vDSO, `host 0x...` for the
program) instead of the export tables. The shims' sleeps loop on EINTR and the handler is installed with
`SA_RESTART`, so the game's calls carry on. The thread id is kept per thread: `gettid` is a system call on Linux,
and calling it for each of a scene's 2.3 million file calls had cost 0.5 s of a 2.3 s tick. In WSL the menu
scene's load is one tick of about 1.8 s (446 ms on Windows on the same machine), mostly the game's small reads of
the game folder on `/mnt/c` (`__syscall_cancel_arch` under `fread`), which crosses to the Windows drive. The
playtest's `profile` scenario checks both profilers' lines on Windows.

On Wayland the cursor does not move under relative mode, so `shimsinput.c` switches modes by the game's own pointer
rather than reading the system's cursor beside raw motion, which is what the Win32 version did.

### Where each thing is

| File | Role |
|---|---|
| [`host/core/main.c`](../host/core/main.c) | `gmv_main`: address-space checks, load, patch, bind, verify; **runs the game by default** (no args = double-click), `--verify-only`/`--run-init`/`--run-main` for staged modes; `find_app`; passes the `.app` path to the bundle stub |
| [`host/core/platform.h`](../host/core/platform.h), [`platform_win32.c`](../host/core/platform_win32.c), [`platform_linux.c`](../host/core/platform_linux.c) | What the loader asks of the system, and the entry point (`wmain`, `main`) |
| [`host/core/config.c`](../host/core/config.c) | The settings file: loaded first thing in `gmv_main` (template written if missing); `gmv_getenv` = environment, then ini, then built-in default. The keys are the game's ([`settings.c`](../host/games/gangstar/settings.c)) |
| [`host/core/macho.c`](../host/core/macho.c) | Read-only Mach-O parser + bind-opcode interpreter |
| [`host/core/image.c`](../host/core/image.c) | Maps segments, runs the low range from its copy, applies & verifies patches |
| [`host/core/imports.c`](../host/core/imports.c) | Collects imports, binds every slot (shim / thunk / data stand-in) |
| [`host/core/fault.c`](../host/core/fault.c), [`veh.c`](../host/core/veh.c), [`fault_linux.c`](../host/core/fault_linux.c) | Low-range redirect, the fault report, init/main fault context; the vectored handler and the signal handlers that feed them |
| [`host/core/runinit.c`](../host/core/runinit.c) | Runs the static initializers (`run_initializers`) and `main()` (`run_main`) |
| [`host/core/log.c`](../host/core/log.c) | The log: lines are buffered until the file opens, and the user's folder is written as `%USERPROFILE%` |
| [`host/core/crashdump.c`](../host/core/crashdump.c), [`crashdump_linux.c`](../host/core/crashdump_linux.c) | The dump `gmv_fatal` writes after a crash, with the log's hidden folders blanked: a minidump on Windows, an ELF core file on Linux |
| [`host/core/loadprof.c`](../host/core/loadprof.c) | `LOAD_PROFILE` and `PERF_PROFILE`: where the time goes in long ticks (Windows and Linux). Left out of a release build |
| [`host/core/thunks.S`](../host/core/thunks.S), [`host/core/reserve.S`](../host/core/reserve.S), [`host/core/gmv.ld`](../host/core/gmv.ld) | The missing-import thunks' common tail; the game's address range as a section of the exe; the linker script |
| [`host/shims/shims.c`](../host/shims/shims.c) | libc shims (mingw CRT); `gmv_shim_lookup` searches **all** tables, the game's too, then `gmv_gl_resolve` |
| [`host/shims/shimsw.c`](../host/shims/shimsw.c) | Win32-backed: pthread, mmap, clocks, time, stdio, `sysctl`, `chdir`/`mkdir`, wchar, `setjmp`/`longjmp`, `__DefaultRuneLocale`, TIS/keyboard |
| [`host/shims/shimscxx.c`](../host/shims/shimscxx.c) | C++ runtime / libstdc++ (`__cxa_*`, RB-tree, `_List_node_base`, `std::allocator<char>`) |
| [`host/shims/shimsstr.c`](../host/shims/shimsstr.c) | `std::string` (COW, libstdc++ 4.2 layout) + `_Rep::_M_destroy` + `gmv_std_string_new` |
| [`host/shims/shimsstream.c`](../host/shims/shimsstream.c) | `std::ostringstream` accumulator (`oss_str_impl`), streambuf get/put accessors, inert vtables |
| [`host/shims/shimssdl.c`](../host/shims/shimssdl.c) | SDL: init + video + `SDL_GL_SwapBuffers`→present; `SDL_PeepEvents` hands over the keyboard events from `shimsinput.c` |
| [`host/shims/shimsobjc.c`](../host/shims/shimsobjc.c) + [`.S`](../host/shims/shimsobjc.S) | Objective-C runtime; also the `gmv_gl_forward`/`gmv_gl_lazy` + `setjmp`/`longjmp` asm |
| [`host/shims/shimsfnd.c`](../host/shims/shimsfnd.c) | Foundation/AppKit/Cocoa method tables (instance / class / struct-return) |
| [`host/shims/shimsappkit.c`](../host/shims/shimsappkit.c) | AppKit C ABI + run loop; AudioUnit, CoreFoundation, IOKit, Gestalt stubs; NS string constants |
| [`host/shims/shimscg.c`](../host/shims/shimscg.c) | CoreGraphics: real display-mode list (`EnumDisplaySettings`), `CFArray`/`CFStringCompare` over it; `CGGetLastMouseDelta` (camera look) + cursor-visibility tracking |
| [`host/shims/shimsinput.c`](../host/shims/shimsinput.c) | Input: SDL3's events. Keyboard → the game's SDL events; menu/touch pointer → fake `NSEvent` → `EAGLView` `-mouseDown:`/`-mouseUp:`/`-mouseDragged:`/`-mouseMoved:` (hover forwarded); **camera look** from SDL's relative mouse while the game shows no pointer (`gmv_game_pointer_shown`), fed to `CGGetLastMouseDelta`, cursor confined to the picture while focused; pumps SDL from the game's `SDL_PumpEvents`; diagnostics (`GMV_CLICK_S3`, `GMV_WATCH`, `GMV_INPUT_TRACE`, `GMV_NO_MOUSELOOK`) |
| [`host/shims/shimsjoy.c`](../host/shims/shimsjoy.c) | Controllers: the game's SDL 1.3 joystick API (`SDL_NumJoysticks`/`Open`/`Update`/`GetAxis`/`GetButton`/`GetHat`) over SDL3: a pad SDL has a mapping for is presented as the DirectInput layout the game expects (buttons X A B Y LB RB LT RT Back Start LS RS, then PS and touchpad on Sony's, sticks on axes 0-3, d-pad as hat), any other device passed through; radial stick dead zone, by the kind of pad; notes the kind of pad last used. Dev builds: `PAD_TEST`, a virtual pad for the playtest |
| [`host/shims/shimsaudio.c`](../host/shims/shimsaudio.c) | Audio backend: a playback thread that pulls PCM from the game's captured CoreAudio render callback (`gmv_au_render_cb`) in 256-frame slices and queues it on an SDL3 stream (about 93 ms, as with `waveOut` before); also the intro movie's stream; level/underrun/lowest-queue diagnostics (`GMV_NO_AUDIO`) |
| [`host/shims/shimsvideo.c`](../host/shims/shimsvideo.c) | The intro movie: `intro.mov` decoded with Media Foundation on Windows, by the system's `ffmpeg` on Linux |
| [`host/shims/songdec.c`](../host/shims/songdec.c), [`songdec_impl.c`](../host/shims/songdec_impl.c) | The custom radio's own decoders for mp3, flac, wav and ogg (`gmv_song_*`), over the single files in [`host/vendor/`](../host/vendor/) |
| [`host/shims/ffpipe_linux.c`](../host/shims/ffpipe_linux.c) | Linux: runs `ffmpeg` / `ffprobe` and reads their output through a pipe (`gmv_ff_start`, `gmv_ff_read`, `gmv_ff_stop`, `gmv_ff_output`) |
| [`host/shims/compat.h`](../host/shims/compat.h), [`compat_linux.c`](../host/shims/compat_linux.c) | `windows.h` on Windows; on Linux the Win32 names the host uses, over pthreads, `clock_gettime`, `stat`, `mprotect` and `/proc/self/maps` |
| [`host/shims/gl/`](../host/shims/gl/) | `gl_window.c` the pacer's settings; `gl_msaa.c` the screen framebuffer; `gl_context.c` the SDL window, GL context, pixel format, `NSOpenGLContext`; `gl_present.c` the frame present (`gmv_gl_swap`), the tick pacer, `PRECISE_WAIT`, `BACKGROUND_THROTTLE` and `SPIKE_MS`; `gl_gpu.c` `GPU_PROFILE`; `gl_draw.c` the draw wrappers and `TEXFIX`; `gl_arrays.c` the game's array state kept (`QUERY_FREE`); `gl_stream.c` `STREAM_VBO`; `gl_trace.c`, `gl_diag.c` diagnostics; `gl_bind.c` CGL stubs, the GL binding (`gmv_gl_resolve`) and the one GL loader (`gl_real`); `glshim.h` what they share with a game's render code |
| [`host/games/gangstar/layout.h`](../host/games/gangstar/layout.h) | The fixed address-space layout, the names, the classes |
| [`host/games/gangstar/game.c`](../host/games/gangstar/game.c), [`settings.c`](../host/games/gangstar/settings.c) | Gangstar's side of `game.h`; every key of `GangstarMV.ini` and the `ORIGINAL_MAC_EXPERIENCE` preset |
| [`host/games/gangstar/patch/`](../host/games/gangstar/patch/) | Surgical patches to the game's own code and the hooks behind the fixes and enhancements, a file for each: `fix_*` (`fix_vertex.c` the movement crash, `fix_crash58.c` the null-delegate guard), `sync_*` the high tick rates, `pad_*`, `key_move.c`, `draw_distance.c`, `density.c`, `spawn_fade.c`, `mission_replay.c`, `lc_switch.c`, `tab*.c` the GAMEPATCH tab, `game_read.c` the game's memory, strings and script commands read without faulting, `diag_*` and `test_*` the diagnostics and test aids. `patch_common.c` writes to the game's code and `patch_apply.c` holds `gmv_apply_gamepatch`. Each patch no-ops unless the bytes match. `patch.h` is what they share |
| [`host/games/gangstar/render/`](../host/games/gangstar/render/) | The effects, a file for each: `ao.c`, `bloom.c`, `bloom_glow.c`, `fog.c`, `fog_hybrid.c`, `aniso.c`, `water_reflections.c`, `vehicle_reflections.c`, `dynamic_shadows.c`, `sun_moon_glow.c`. `draw_hooks.c` says what happens around each draw, `present_hold.c` is `MENUFLASH_FIX`, `render.h` what they share |
| [`host/games/gangstar/features/padicons.c`](../host/games/gangstar/features/padicons.c) | `PAD_ICONS`: recognises the menu's gamepad atlas (`splash_gamepad`, 4096×2048 DXT5) at its upload, keeps the blocks under its pictures, draws Xbox / PlayStation versions over them (distance-field shapes, GDI Arial letters on Windows, FreeType's on Linux, its own DXT5 encoder, mip levels 1-6 box-filtered) and swaps them in with `glCompressedTexSubImage2D` after a present when the pad kind changes |
| [`host/games/gangstar/features/customradio.c`](../host/games/gangstar/features/customradio.c) | `CUSTOM_RADIO`: the sixth station's songs, decoding (`songdec.c` first; Media Foundation on Windows, `ffmpeg` on Linux for the rest) and mixing (item 18) |
| [`host/games/gangstar/features/ucveh.c`](../host/games/gangstar/features/ucveh.c) | `UC_CRUSHER`: the Crusher monster truck, from the player's own add-on folder |
| [`host/games/gangstar/features/radiosongs.c`](../host/games/gangstar/features/radiosongs.c) | `RADIO_SONGS`: the iPhone version's extra radio songs, from the player's own add-on folder (item 26) |
| [`host/games/gangstar/features/addon_io.h`](../host/games/gangstar/features/addon_io.h) | The two add-on readers' shared helpers: 32-bit words, the add-on files' masked first bytes, whole files read and written |
| [`host/games/gangstar/features/vbostatic.c`](../host/games/gangstar/features/vbostatic.c) | `STATIC_VBO`: unchanged models kept in GL buffer objects (item 12) |
| [`host/games/gangstar/features/broadphase.c`](../host/games/gangstar/features/broadphase.c) | A faster `b2BroadPhase::Query` with identical results |
| [`host/games/gangstar/features/b2solve.c`](../host/games/gangstar/features/b2solve.c) | `b2World::Solve` without walking every body, with identical results |
| [`host/games/gangstar/features/hudbars.c`](../host/games/gangstar/features/hudbars.c) | The HUD's health and armour bars without a full UI search per update |
| [`host/games/gangstar/features/bbfast.c`](../host/games/gangstar/features/bbfast.c) | The billboard turn with its range tests first, same results |
| [`host/games/gangstar/features/perf_probes.c`](../host/games/gangstar/features/perf_probes.c) | `PERF_PROFILE`: the game's own routines, timed. Left out of a release build |
| `tools/` | Offline analysis/generators (genlow, machoinfo, disasm, initreach, objc1dump, ...) |
| `analysis/` | Parsed reports: imports, static-init reachability, ObjC classes, SDL |


### Shim coverage (477 of 528 imports)

The other 51 are 49 functions, whose thunk reports the name if called, and 2 data symbols, which get
a no-access stand-in. By library: libcrypto 20, libstdc++ 9, libSystem 8, Security 6, CoreVideo 3,
OpenGL 3, CoreFoundation 1, libobjc 1.

### The 51 unfilled imports

Audited by following each import's callers up through the binary (direct calls and jumps,
pointers in data, PIC references, Objective-C message references). None can be reached in this build:

| Imports | Count | Used by | Why it does not run |
|---|---|---|---|
| libcrypto, Security, `std::ifstream`, `std::istream` | 20 + 6 + 7 | One start-up routine of the game and its helper | The code that uses them does not run in the build the port was made with |
| `CGLCreateContext`, `CGLGetCurrentContext`, `CGLGetPixelFormat` | 3 | 0x595f30, at 0x596278 onwards | Needs the config's +0x10 set, and its direct caller 0x37d4 clears that byte first. Its only other reference is a base-class vtable slot (0x68293c). Start-up code, so a change would show at every launch |
| `CVDisplayLink*` | 3 | `-[EAGLView setupDisplayLink]` | No message reference to the selector in the binary |
| `getifaddrs`, `freeifaddrs`, `inet_ntoa` | 3 | `-[GameDelegate getIPAddress]`, sent only by `-testConnection` | `testConnection` sends it only if the reachability flags say reachable, and the shim always answers 0 (`sc_reach_get_flags`). **A shim that reports a network would reach these** |
| `CC_MD5` | 1 | `-md5HexDigest:`, sent only by `webView:didFinishLoadForFrame:` | The host has no WebKit, so that delegate never fires |
| `objc_enumerationMutation` | 1 | `-findButtonInView:` in the two web view controllers | Sent only by itself, and called only if a collection changes while being walked |
| `std::ios_base::Init::~Init` | 1 | 204 static destructors | They are registered through `__cxa_atexit`, which the shim ignores |
| `clock` | 1 | 0x1b8a30, a one-line wrapper | No reference to the wrapper |
| `system` | 1 | 0x595a30, which builds an `rm -rf` line | No reference to the function |
| `uname` | 1 | The constructor 0x527d40 (vtable 0x681028) | Its two callers, 0x527f90 and 0x527fa0, have no reference |
| `dyld_stub_binder` | 1 | `__stub_helper` | The loader writes every lazy pointer up front, so the helpers never run |
| `std::nothrow` (data) | 1 | One site, 0x53f188 | Its address is passed on and never read |
| `__CFConstantStringClassReference` (data) | 1 | The `isa` of 189 string literals | The Objective-C shims recognise literals by address and never read it |

This is static analysis, so a pointer computed at run time cannot be ruled out, though nothing suggests
one. None of the 24 game logs on hand records a call.
A call would log `[MISSING] game called unimplemented import <name>` and end the process with exit code
3 (`gmv_missing_import` in [`host/core/imports.c`](../host/core/imports.c)), after the error box (see Diagnostics).
Any other build of the game is refused by the patch table first, so this audit covers only the
build the port was made with.

| Area | State | Where |
|---|---|---|
| libSystem (libc, pthread, mmap, time, stdio, wchar, setjmp, rune locale, keyboard, `stat`) | nearly complete | `shims.c`, `shimsw.c` |
| libstdc++ / C++ runtime | init set + `std::string` (COW + `_Rep`) + RB-tree/list + `std::ostringstream` (accumulator + by-value `str()`) | `shimscxx.c`, `shimsstr.c`, `shimsstream.c` |
| libobjc | `objc_msgSend` / `_stret` / `Super` / exceptions | `shimsobjc.c/.S` |
| AppKit / Foundation / Cocoa | launch path + lots of value classes; more as walls appear | `shimsappkit.c`, `shimsfnd.c` |
| CoreGraphics / CGL | real display-mode list (`EnumDisplaySettings`), `CFArray`/`CFStringCompare` over it; `CGGetLastMouseDelta` camera look + cursor tracking | `shimscg.c`, `shims/gl/` |
| OpenGL (~112 imports) | **bound** (lazy cdecl→stdcall to opengl32); drawing live | `shims/gl/` + `shimsobjc.S` |
| SDL (events, video, GL swap) | init + swap wired (`SDL_GL_SwapBuffers` → SDL3's `SDL_GL_SwapWindow`); **keyboard events wired** (`SDL_PeepEvents`); **joystick API** over SDL3 | `shimssdl.c`, `shims/gl/`, `shimsinput.c`, `shimsjoy.c` |
| AudioUnit | **wired to an SDL3 backend** — the game's render callback is pulled for PCM and played | `shimsappkit.c`, `shimsaudio.c` |
| CoreFoundation, IOKit | fail-soft stubs | `shimsappkit.c` |

Per-library counts are in the import dump in `GangstarMV.log`. A Mach-O symbol is the C name with
one leading underscore; triple-underscore names are C identifiers that themselves begin with `__`.

### ABI notes (i386, macOS → Win32)

- Mostly identical cdecl; `int`/`long`/`size_t`/pointer/`float`/`double` are the same width.
- **16-byte stack alignment.** The game assumes `esp` is 16-byte aligned at every call (SSE
  `movaps`/`movapd` on stack locals). Every host function that calls into game code —
  `run_initializers`, `run_main`, `shim_NSApplicationMain`, `gmv_notif_post`, the `NSThread`/
  pthread trampolines, the input dispatch — **must** be marked
  `__attribute__((force_align_arg_pointer))`. Without it the fault shows up as a Windows "access
  violation read 0xffffffff" at a `movaps`. A force-aligned helper that gets *inlined* loses its
  realignment, so also mark it `noinline` (or align the caller instead).
- **Struct returns:** macOS i386 returns a struct through a hidden pointer the callee pops
  (`ret $4`); a shim returning a large struct needs `__attribute__((callee_pop_aggregate_return(1)))`
  (used for `CGDisplayBounds` and the `NSRect` stret stubs). **A C++ class with a non-trivial copy
  ctor is also returned this way even when it is one word** — e.g. `std::string` (a single `char*`
  with a COW copy ctor) is returned via the hidden pointer, *not* in `eax`. mingw disagrees: it
  returns a ≤8-byte aggregate in `eax` with no hidden pointer and no pop, so the attribute does
  nothing. Such returns are therefore written in asm — see `gmv_oss_str` (`ostringstream::str()`)
  in [`host/shims/shimsobjc.S`](../host/shims/shimsobjc.S): it builds the result, stores it through the hidden
  pointer, and does `ret $4`.
- `wchar_t` is 4 bytes on macOS (our wchar shims operate on `uint32_t`); `jmp_buf`, `time_t`
  (4 vs 8), `off_t` (64-bit) differ and are handled explicitly in `shimsw.c`.
- No C++ exceptions in the game (`__unwind_info` is only 0x48 bytes); ObjC 1.0; SDL 1.3.

## Settings reference

One row per key, so that each change from the Mac game says why it is there. The columns:

- **Kind:** what the change is.
  - *port fix*: a fault of running the Mac game on this system.
  - *original bug*: a fault of the game itself.
  - *speed-up*: the same results, sooner.
  - *enhancement*: something the Mac game never had.
- **Under the preset:** what `ORIGINAL_MAC_EXPERIENCE=1` does with the key, and why.
- **Item:** the numbered fix item below, or where else the key is explained.

The defaults a new `GangstarMV.ini` gets are in the README's settings table and in `settings.c`; a key left blank
takes the call site's built-in value, which for an enhancement is the Mac's (item 32).

| Key | Kind | Why it exists | Under the preset | Item |
|---|---|---|---|---|
| `NO_FLICKER` | port fix | The game draws every other tick, but the port presented every tick, so blank frames flickered between the drawn ones. | on: without it the port flickers | `gl_present.c` |
| `VTXFIX` | port fix (cause not proven) | Crash within a few steps of walking: one path of the vertex converter lacks the game's own `0xFF` check. Not seen on the Mac. A shader-key collision inside the game is suspected, but not proven. | on: without it the port can crash | 1 |
| `CRASH58_GUARD` | port fix (cause not proven) | Crash about 13 min into play: an on-demand animation reader with no file. Which of two causes it was is unknown. | on: without it the port can crash | `fix_crash58.c` header |
| `FRAME_MS`, `SWAP_INTERVAL` | port fix (the pacer) + enhancement (other rates) | The driver's vsync ran the game at the panel's rate (dt 0 / 14 ms on a 144 Hz laptop), which broke the engine sound and box climbing. The Mac ticked at 60 by its 60 Hz vblank. | 16.667 ms (60 ticks/s, 30 drawn fps) and vsync off, as on the Mac | 4 |
| `STOPLOCK_FIX` | original bug, exposed by the port | The 200 ms lock after a full stop reads the frame time, not a clock. The Mac's uneven frames, and the early driver-paced builds, hid it; the even pacer exposed it. | on: with the pacer, a car would stay dead | 5 |
| `SWAP_COPY` | port fix | After a swap the Mac's back buffer still holds the last frame. Windows' default format holds the frame before last, so cutscene fades, which draw over the last frame, landed on a stale picture. | on | 7 |
| `TEXFIX` | port fix | The death and map-menu fades never darkened the screen: the fade quad samples a texture with no coordinates, and this driver returns a transparent texel where the Mac's returned an opaque one. | on: the Mac showed these fades | 8 |
| `PHYSICS_SYNC` | enhancement (high frame rates) | At other tick rates the game's fixed 1/60 s physics steps fall unevenly on the drawn frames (judder). At 60 ticks/s the game's own step is used either way. | off: the game's own step | `sync_physics.c` header |
| `ORIGINAL_MAC_EXPERIENCE` | preset | One switch back to the Mac's behaviour without editing each key. Audited three times: every place in the game's code still changed under it comes from the keys it leaves on and the keyless rows below, the same whether the preset is on from the start or switched on in game. | — | "Running and settings" |
| `MENUFLASH_FIX` | original bug | Two half-ready frames of raw 3D flash between the title and the main menu. | off: every original-bug fix is off under the preset | 9 |
| `MENU_FADE_CLICKS` | original bug | The main menu ignores clicks while its background fades. | off | 10 |
| `RADIO_LOGO_FIX` | original bug | The HUD shows the last station played, not the car's. | off | 11 |
| `FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE` | port fix (speed) | Windows' C library asks the system on every `ftell`/`fseek` and copies backwards slowly. The Mac's library does neither, so loads froze for about 2 s and driving stuttered. Linux's library behaves as the Mac's. | on | 12 |
| `LOAD_SLEEP_SKIP` | speed-up of the game's own wait | While streamed objects load, the game sleeps 16 ms every tick for 2-4 s. | on: without it each scene change of the main menu stalls, frames of 34-50 ms for seconds; same picture and rules | 12 |
| `MSAA` | port fix (Mac's look restored) | The Mac game asks for 4x multisampling. The port's window format had none. The default is `auto` (`GPU_PROFILE`): 4 on any GPU. | 4, the Mac's | "Running and settings", 38 |
| `ANISOTROPIC_FILTERING` | enhancement | Sharper roads and walls at a distance. | 0: only what the game asks for | "Running and settings" |
| `AMBIENT_OCCLUSION` | enhancement | Soft contact shadows. | off | "Running and settings" |
| `SUN_MOON` | enhancement (iPhone feature brought back) | The Mac build half-removed the iPhone version's sun and moon. | off | "Running and settings" |
| `DRAW_DISTANCE` | enhancement | The city drawn further than the Mac's 450/900 m. Above 1 it also gives the buildings near the map's centre their collision at once (item 13). | 1 | 13, `draw_distance.c` header |
| `CAMERA_SYNC` | enhancement (high frame rates) | Above 60 ticks/s the running camera lost the Mac's sway. Installed only below 16.667 ms. | off | 4 |
| `WALK_SYNC`, `AI_SYNC` | enhancement (high frame rates) | Above 60 ticks/s walkers flicked their facing and runners were taken as stuck. Installed only below 16.667 ms. | off | 4 |
| `BLOOM` (+ `BLOOM_LIGHTS`, `BLOOM_WINDOWS`) | enhancement | Glow around lights, neon and lit windows. | off | "Running and settings" |
| `WATER_REFLECTIONS` | enhancement | Neither the Mac's sea nor the iPhone's reflects anything. | off | "Running and settings" |
| `FOG_MODE`, `FOG_HEIGHT` | enhancement | The game fogs only some materials; buildings, roads and the sea get none, on the Mac too. Fog over the whole city is new. | 0: the game's own fog | "Running and settings" |
| `HIDE_MORE_GAMES`, `HIDE_GAMELOFT_LIVE` | enhancement | Their pages and servers are long gone. | off: the Mac shows both | 14, 15 |
| `ALLOW_WEAPON_SPRINT` | enhancement (as on the iPhone) | The Mac puts the weapon away during a sprint. | off | 16 |
| `TRAFFIC_DENSITY`, `PEDESTRIAN_DENSITY`, `PARKED_CAR_DENSITY` | enhancement | More cars and people than the Mac's caps of 18 / 14 / 10. | 1, and the Mac's spawn pool | `density.c` header |
| `GAMEPATCH_TAB` | enhancement (native menu, no overlay) | Settings changed inside the game's own options menu. | stays, with only the preset's switch, which is the way back | "Running and settings" |
| `VEHICLE_REFLECTIONS` | enhancement | Shiny cars reflect the street, after the water reflections. Where the ray misses (roofs, trunks) the screen serves as a sphere map instead of leaving them bare. | off | `vehicle_reflections.c` header |
| `MISSION_REPLAY` | enhancement | Replay completed missions from the SERVICES menu. | off; its tab's hooks stay in, hidden, because the menu is built once at the start | `mission_replay.c` header |
| `PAD_ICONS` | enhancement | The game's pad pictures show only where a button is, not its Xbox or PlayStation name. | off | `padicons.c` header |
| `UC_CRUSHER` | enhancement (add-on from the player's own Urban Crime) | The Crusher truck parked by L.C.'s hotel after ON A LION HUNT. | off; with the add-on there its files are still read at the start, unused, so the switch needs no new start | `ucveh.c` header |
| `SPAWN_FADE` | enhancement | Cars and people pop in and out on the Mac. | off | `spawn_fade.c` header |
| `STATIC_VBO` | speed-up | The Mac build sends every model to the graphics card on every draw. | on: same picture, about 10% of each frame back | 12 |
| `MINIMAP_SHOPS` | enhancement | The game keeps the shops in the minimap's list but never draws them. | off | 17 |
| `CUSTOM_RADIO` | enhancement | A sixth station with the player's own music. | off | 18 |
| `SHORT_WARNING` | enhancement | The start-up warning holds for 15 s and takes no input. | off | 19 |
| `CUTSCENE_KEYS`, `TUTORIAL_KEYS` | enhancement | Cutscenes and popups need the mouse on the Mac. | off | 20, 21 |
| `PAD_LAYOUT` | enhancement | A GTA V-like pad layout. | off: the Mac's buttons | 22 |
| `PAD_MENU`, `PAD_HINTS` | enhancement | The Mac's menus are mouse-only, and its tutorials name only keys. | off | `pad_menu.c`, `pad_hints.c` headers |
| `KEY_MOVE` | enhancement | The Mac's keys move one direction at a time and snap the body round. | off | 28 |
| `SWITCH_SAVE_FIX` | original bug | A save made as Betty gave her things to Johnny. | off | 23 |
| `PLAYERSWITCH_INVENTORY` | enhancement | Betty shares Johnny's cash and weapons in free roam. | off | 24 |
| `LC_SWITCH` | enhancement | Play as L.C. once the story is done. | off | 25 |
| `RADIO_SONGS` | enhancement (iPhone and Java songs, add-on from the player's own copies) | The Mac lacks nine of the iPhone's songs; the Java version adds two rock songs. | off; files read at the start, unused | 26 |
| `DYNAMIC_SHADOWS` | enhancement | Shadows that follow the sun and the moon. | off: the game's fixed shadow | 27 |
| `LOAD_BUDGET` | speed-up of the game's own wait | The game builds 1500 collision items a tick on the way to the main menu, hitching for 43-131 ms; `DRAW_DISTANCE` makes it heavier. | 200: in the main menu at 30 fps its scene changes stalled 116-219 ms with 1500 and 31-89 ms with 200; how long a stall lasts depends on the machine, not on the game | 29 |
| `DPI_AWARE` | port fix (Windows only) | Windows display scaling above 100% blurred a window the system stretched. | on: no change to the game | the ini's description |
| `FAST_LOAD` | speed-up | The start-up Loading screen takes one step every 33 ms. | on: same loading steps, sooner | 31 |
| `FADE_EDGE_FIX` | original bug | With 4x MSAA a black fade leaves the top row and left column showing. The Mac used 4x as well, so it most likely showed the same line. | off | 33 |
| `MISSION_SAVE_FIX` | original bug | Every save forgets Reginald's five missions: 8 groups of mission flags, 7 written. | on: a save made under the preset must keep them, or `MISSION_REPLAY` would lose them once the preset is off | 34 |
| `NO_INTRO`, `SHOW_CURSOR`, `INTRO_FLIP` | convenience | Skip the intro, keep the arrow shown, and flip the intro for a decoder that shows it upside down. | `NO_INTRO` and `SHOW_CURSOR` off; `INTRO_FLIP` left as set | — |
| `LOG` | port | The port's log for bug reports. | left as set | 30 |
| no key: collision query, proxy lists, Box2D body list, HUD bars, billboard turn | speed-up | Same results, from about 70 to 113 fps in a busy scene at draw distance and crowds 2. | always on: identical results | `broadphase.c`, `b2solve.c`, `hudbars.c`, `bbfast.c` headers |
| `SHARED_RESOLVE` | speed-up (of the port's own effects) | The shadows, the sea's and the cars' reflections, the AO and the bloom each copied the screen's depth out of the 4x multisampled screen: up to five resolves a frame of the same depth. | on: the same picture, checked pixel for pixel | 35 |
| `FOG_SKIP_FAR` | speed-up (of the port's own fog) | With `FOG_MODE` 1 or 2 the low-detail far city lies wholly past the fog's clip at `DRAW_DISTANCE=2`, yet every draw of it went to the driver and was clipped away whole. | on: the same picture (the preset's `FOG_MODE=0` has no clip, so it does nothing there) | 35 |
| `QUERY_FREE` | speed-up (of the port's own effects) | The effects asked GL which framebuffer and shader were bound around each of their draws, ~60,000-90,000 queries a second in the city; then also what the game's vertex arrays point at, ~50,000-60,000 more. | on: the same values, checked against GL | 35, 37 |
| `FILE_MISS_CACHE`, `FILE_POOL` | port fix (speed) | The game looks for loose copies of assets before its packs and reopens the same packs many times a second; on Windows each failed or new open is a kernel call of ~70-80 µs (the virus scanner looks too). | on: the same bytes read | 35 |
| `STREAM_VBO` | speed-up | For each draw from the game's client arrays the driver finds and copies the vertices inside the call; on NVIDIA most of the main thread's driver time. | on: the same bytes drawn (needs `QUERY_FREE`) | 37 |
| `SHADOW_QUALITY` | enhancement setting | `DYNAMIC_SHADOWS`' 2048 map with 16 taps is the heaviest part of the shadow pass on graphics built into the processor. Default `auto` (`GPU_PROFILE`): 1 on a built-in GPU, 2 on a graphics card. | unused: the preset has no dynamic shadows | 36, 38 |
| `GPU_PROFILE` | enhancement setting | The same settings cost a GPU built into the processor (memory shared with the CPU) far more than a graphics card; one AMD built-in GPU's driver stopped with 8x MSAA and every effect on. | no effect: the preset sets `MSAA` 4 and has no dynamic shadows | 38 |
| `PRECISE_WAIT` | speed-up (of the port's pacer) | The pacer spun the last 1.5-2.5 ms of every tick in `Sleep(0)`, a third of a core at 150 ticks a second. | on: the ticks come at the same deadlines | 38 |
| `BACKGROUND_THROTTLE`, `BACKGROUND_FPS`, `MINIMIZED_FPS` | port fix (system behaviour) | The window went on drawing at the full rate behind other windows and while minimized, and a game in progress went on with the player away. | off: as the Mac port ran | 38 |
| `SPIKE_MS` | diagnostic | A player's log had frame-time spikes (55-79 ms) that only a dev build's profile could place. | unaffected (a log setting) | 38 |

## Fix records

One numbered item per fix or enhancement that needed more than its key's description. The numbers are fixed, because
`FIXES.txt` and the source cite them as "notes item N". Each item says what was wrong or missing, whose fault it was, what the port
does, and what `ORIGINAL_MAC_EXPERIENCE` does with it. Where a setting is on in a new ini but described here as an
enhancement, its built-in value (a blank key) is still the Mac's (item 32). How each feature works in detail is in
its source file's header comment.

1. **Input wiring and the vertex-converter crash (`VTXFIX`).**
   - **Keys and window:** SDL3's key events reach the game as its own SDL events, with a faithful `keysym.sym`
     (the game reads `keysym.sym` at +0x10 into its key-state table). Alt+F4 and the close button end the process as
     the Mac's close does (`-windowWillClose:` 0x19af50 → `terminate:`, no save). Alt+Enter goes through the game's
     own full-screen switch (0x19bcd0), which ignores a second switch within 500 ms; presses wait out that window.
   - **Crash on moving (port fix, cause not proven):** the vertex converter (entry 0x563c90) guards `cl == 0xFF` on
     its dest-absent path (0x5640f0) but not on its dest-present path (0x563e53). The 8 bytes at 0x563e29 jump to a
     trampoline that applies the engine's own test to that path, continuing at 0x564070 as the engine does.
     `VTXFIX=0` removes it. On under the preset.
2. **Audio.** The game drives a CoreAudio `AudioUnit`; `shimsappkit.c` captures its output format and render
   callback, and [`shimsaudio.c`](../host/shims/shimsaudio.c) pulls that callback for PCM on its own thread and queues it
   on an SDL3 stream (44.1 kHz stereo s16, about 93 ms ahead). Every vehicle and effect sample loads from the
   `_packMac` packs after a probe for a loose file (the probe's "FAIL" lines are expected). The engine sound is the
   game's own: each car has idle, onidle and offidle loops crossfaded by volume with throttle and RPM, and each
   voice's pitch factor (+0x64, 0x4000 = 1.0x, clamped at 2.0x) is ramped by `AudioSource::update` (0x5b1980). The
   port reproduces it unchanged. `NO_AUDIO=1` silences the backend.
3. **Mouse camera.** The camera reads relative motion through `CGGetLastMouseDelta` (engine poll 0x3f4a). The host
   feeds it from SDL's relative mouse while the game shows no pointer (`gmv_pointer_up`, CursorGame +4 or +5), and
   the mouse is an ordinary pointer while the game shows one. The cursor is confined to the picture while focused.
4. **Frame pacing (`FRAME_MS`, `SWAP_INTERVAL`, `CAMERA_SYNC`, `WALK_SYNC`, `AI_SYNC`).**
   - **Cause (port fix):** the game is dt-driven and was tuned on 60 Hz Macs, where `SDL_GL_SwapBuffers` waited one
     vblank a tick: 60 ticks/s, every other tick drawn (30 fps). Left to the driver's vsync, a 144 Hz laptop ran
     ~150 ticks/s with the whole-ms dt alternating 0 and 14 ms, which broke the engine sound and box climbing.
   - **Fix:** a fixed-deadline pacer with vsync off (`gl_present.c`). Measured: 60.0 ticks/s, dt 16.7 ms, jitter
     0.0-0.2 ms, 30 drawn fps, as on the Mac. Other rates are an enhancement (`FRAME_MS`, 0 = uncapped).
   - **Above 60 ticks/s:** the running camera's sway, the walkers' facing and the AI's stuck test were tuned to
     60 ticks. `CAMERA_SYNC`, `WALK_SYNC` and `AI_SYNC` keep them as at 60 and are installed only below 16.667 ms;
     `PHYSICS_SYNC` steps physics once per tick (`sync_*.c`). All four are off under the preset.
5. **Car stuck after a full stop (`STOPLOCK_FIX`, original bug exposed by the port).** A car going from forward to
   stopped with all wheels down starts a 200 ms settle lock (vehicle update 0x112bc4: flags +0x180 |= 0x60, stamp in
   +0x184). While it holds, the force routine (0xfef68) skips the drive force and brake/reverse (0x10d26e) returns.
   The stamp is the frame dt (`app+0x17ad8`), not a clock, so the lock ends only on a frame with a smaller dt than at
   the stop. The Mac's vsync jitter supplies one within a few ticks; the even pacer never does, so gas and brake died
   for good. The fix makes the stamp the tick's own clock (`now_ms` at 0x949adc, load at 0x1132b2), so the lock lasts
   the 200 ms the code asks for. On under the
   preset, as the pacer would otherwise leave cars dead.
6. **Solo-car listening (`SOLO_CAR`, dev test aid).** Mixes only the player's car and mutes the rest. The car-sound
   module plays through a 5-entry SoundManager table at 0x689dd0 (installed by 0x10aba8); the host wraps `isPlaying`
   and `play` there and resolves each handle to its render voice. Left out of a release build.
7. **Cutscene fade to black (`SWAP_COPY`, port fix).** The Mac's `flushBuffer` keeps the back buffer's contents after
   a present, and the game's fades draw over that retained frame. Windows' default `PFD_SWAP_EXCHANGE` format holds
   the frame before last, so each fade step landed on a stale picture. `ensure_gl` takes the `PFD_SWAP_COPY` twin of
   the chosen format (same colour, alpha, depth and stencil), found through a hidden helper window because SDL
   chooses formats by attribute only. On under the preset.
8. **Death and map-menu fades never darkened (`TEXFIX`, port fix).** Every fade quad (0x1a5cea, 0x1b5094, the
   cutscene fader) is drawn with `GL_TEXTURE_2D` still on for unit 0 and no texcoord array. `setTexture(stage, NULL)`
   (0x503af0) only records the NULL, and when a texcoord array is disabled the driver's current texcoord is
   (0,0,0,0) (`glMultiTexCoord4f` at 0x4fa464, 0x4ff800, 0x4ffb52), so the pipeline samples at s/q = 0/0, which is
   undefined. With MODULATE the quad is colour × texel; on this driver the texel is the HUD atlas's transparent
   (136,136,136,0), so the black lands at alpha 0. The Mac's driver evidently gave an opaque sample. `TEXFIX` draws
   untextured shapes in plain colour. On under the preset: the Mac showed these fades.
9. **Main-menu entry flash (`MENUFLASH_FIX`, original bug).** From the title (state 0x65a388) to the main menu
   (0x659f28) two frames of the raw 3D background show before the menu fades in from black, at any tick rate: on the
   first frame the cutscene fader (alpha 0x6424b4, timer 0x6424b0) is idle and on the second its 1 ms CineFadeOut
   (0xcda6c) has alpha 0. `present_hold.c` keeps the title on screen until the fade from black starts. Off under the
   preset.
10. **Main-menu clicks ignored during the background fades (`MENU_FADE_CLICKS`, original bug).** On every tick a fade
    runs, CineFadeIn (0xcd8be) and CineFadeOut (0xcda6c) call 0x283b0, which sets the input lock and zeroes every key
    and button level (0x19d72). A click only stores a level, turned into events once per tick (0x19b60), so a click
    made during a fade was wiped. The four calls go to `gmv_cine_fade_lock`, which, while the main menu (vtable
    0x659f28) is on top and the front end is not switching screens, sets the lock flag but skips the reset
    (`fix_menu.c`). Off under the preset.
11. **HUD radio logo after getting into a car (`RADIO_LOGO_FIX`, original bug).** Getting in switches the HUD first,
    so the radio refresh (0x9323a) sets the logo from the station that played last; only afterwards does the car code
    (0x10a57c) start the radio on the car's own station (0x60190), and nothing refreshes the logo again. The fix
    works out the station the car is about to start, the way 0x10a57c and 0x60190 will, and shows its logo in the
    same tick as the game's refresh, so the old logo is never drawn (`radio_logo.c`). Off under the preset.
12. **Scene loads froze for ~2 s (`FAST_FTELL`, `FAST_FSEEK`, `FAST_MEMMOVE`, `LOAD_SLEEP_SKIP`, `STATIC_VBO`).**
    - **File calls (port fix):** the game's file stream (vtable 0x6839a8) calls `GetSize` and `Tell` before every
      `fread`, ~580,000 reads a load. macOS stdio keeps the offset in the FILE; msvcrt's `ftell` enters the kernel
      twice (~3.7 µs) and its `fseek` always drops the buffer. `FAST_FTELL` keeps each read-only file's position;
      `FAST_FSEEK` gives `"rb"` streams a 64 KB buffer and serves a seek inside it. `FAST_MEMMOVE` does the game's
      overlapping backward copies with SSE2, as the Mac's library does. Off on Linux, whose library already behaves
      as the Mac's.
    - **The game's own wait (speed-up):** while streamed objects load, the game raises its queue budget at 0x641114
      to 1500 items and sleeps 16 ms a tick (0x598100) for 2-4 s. At 60 Hz the sleep hid inside the frame.
      `LOAD_SLEEP_SKIP` replaces the call at 0xd21d8 with a 5-byte NOP; the budget is `LOAD_BUDGET`'s (item 29).
    - **Models (speed-up):** the Mac build sends every model to the graphics card on every draw. `STATIC_VBO`
      (`vbostatic.c`) keeps the unchanged ones in GL buffer objects: time inside the draws 185 → 77 ms a second.
    - All of these are on under the preset: the same picture and rules.
13. **Buildings near the map's centre without collision (original bug).** `updateCells` (0x13d5a4) adds a cell's
    collision when the cell comes within range, by comparing with a rectangle around the last position, which starts
    at (0,0,0). Every cell within range of the world origin therefore counts as already added. On the Mac that is
    ±225 m; at `DRAW_DISTANCE=3` it is most of the city. `apply_origin_collision_fix` wraps the only call (0x135a5a)
    and moves the starting position 100 km away. It is applied only with `DRAW_DISTANCE` above 1, so at 1, and under
    the preset, the game is as on the Mac.
14. **"More Games!" corner (`HIDE_MORE_GAMES`, enhancement).** The page curl is `widgetMainMenu.btnMore`; it opened
    Gameloft's ads page, long gone. Two slots of the main-menu widget's vtable (0x659048: show 0x16e91c, text setup
    0x171c00) are wrapped to hide the button after the game's own code. Off under the preset.
15. **Gameloft LIVE! badge (`HIDE_GAMELOFT_LIVE`, enhancement).** `widgetMainMenu.btnGLLive` opened the long-gone
    sign-in. The same wrappers hide it, and the click handler's two `btnGLLive` matches (0x172bb5, 0x172aaa) are
    dropped. Independent of item 14. Off under the preset.
16. **Weapon kept while sprinting (`ALLOW_WEAPON_SPRINT`, enhancement, as on the iPhone).** The Sprint handler
    (0x11e002) marks the character as sprinting (+0x666) and selects slot 1, the fists, then restores the saved slot
    on release. The patch keeps the slot in hand. Off under the preset.
17. **Shops on the minimap (`MINIMAP_SHOPS`, enhancement).** The minimap keeps the shop checkpoints (category 0x15)
    and teleports (0x16) in its icon list, but its draw loop (0x4679d in 0x4623a) skips both categories. The patch
    lets the shops through. Off under the preset.
18. **Custom radio station (`CUSTOM_RADIO`, enhancement).** The Mac build kept the iPhone's `MusicPlayer` as empty
    stubs. [`customradio.c`](../host/games/gangstar/features/customradio.c) plays the `custom radio` folder (mp3, ogg,
    flac and wav through the port's own decoders; m4a, aac and wma through Media Foundation on Windows and `ffmpeg` on
    Linux) as a sixth station after the game's five, shuffled, decoded about 3 s ahead. The SOUND page gains Custom
    Radio Volume and Custom Radio rows, and the HUD shows the file's PLAY LIST / MY MUSIC logo. Off under the preset.
19. **Start-up warning for 3 s, skippable (`SHORT_WARNING`, enhancement).** The warning state (vtable 0x65a488)
    holds for 14999 ms and takes no input, and nothing loads meanwhile. Its update slot goes to `gmv_sw_update`, which
    moves on after 3 s or at a key, click or pad button. Off under the preset.
20. **Cutscenes with PC keys (`CUTSCENE_KEYS`, enhancement).** During a cutscene `widgetSkipScene` (menu widget 43)
    shows the touch SKIP button, the dialog arrows and the game's pointer. With the setting, none of them shows; Esc or
    Enter skips the cutscene and Space advances the dialog. Off under the preset.
21. **Tutorial popups with PC keys (`TUTORIAL_KEYS`, enhancement).** `widgetTutoBlock` (menu widget 25, vtable
    0x658e28) takes only a click on OK. While it shows, Esc, Enter or Space press OK, and in a question popup Enter or
    Space mean YES and Esc NO. Off under the preset.
22. **GTA V-like pad layout (`PAD_LAYOUT`, enhancement).** Pad buttons reach each control mode's handler as events
    `{vtable, 0x1e, button, state}` from the poller 0x191a0 and are mapped to actions through the handler's map at
    +0x1c (foot, car, flying, sniper). The setting rewrites those maps to a GTA V-like layout, with the stick wheelie
    limited to motorbikes. Off under the preset: the Mac's buttons.
23. **A save made as Betty gave her things to Johnny (`SWITCH_SAVE_FIX`, original bug).** After the script command
    PlayerSwitch (0xd00a8 → 0xf6b48) the save writer stores the player in control's cash and weapons as Johnny's. The
    fix wraps the writer's ten calls and lends the main player's four fields for the length of the write; the position
    stays where you quit. `MISSION_REPLAY` wraps the same calls, and each accepts a call already wrapped by the other.
    Off under the preset.
24. **Betty shares Johnny's cash and weapons (`PLAYERSWITCH_INVENTORY`, enhancement).** Each Player is built
    (0xf9e94) with 10000 cash and its own WeaponInfos (+0x7f4: 16 slots, 11 ammo counts, the slot in hand at +0x6c). In
    free roam with no mission on, the first look copies Johnny's cash, ammo and weapons to the player in control and
    each later one copies them back, so there is one wallet. Missions keep the game's own loadout. Off under the
    preset.
25. **Play as L.C. after the story (`LC_SWITCH`, enhancement).** L.C. is the level object `LC_00`, a Player like
    `Johnny_00` and `Betty_00`. The switch is built like the game's own switch to Betty (scripts 62 and 63): a marker at
    `Checkpoint_LC_Hotel_00`, placed on the hotel's walk with a 150-unit radius, swaps the player in control with the
    game's own cut, control handlers and camera target, and frees the input locks the one left behind holds. Off under
    the preset.
26. **The iPhone version's extra radio songs (`RADIO_SONGS`, enhancement).** The Mac lacks nine of the iPhone's songs;
    the Java version adds two rock songs. They come from `add-on\radio.pak`, made from the player's own copies
    (`tools/radio_gain.py` brings them to the Mac's level, about 3 dB louder than the iPhone's files), and are added to
    the sound list and the stations' lists after the Mac's, so the ids before stay. Off under the preset; with the
    add-on there its files are still read at the start, so the switch needs no new start.
27. **Shadows from the sun and moon (`DYNAMIC_SHADOWS`, enhancement).** How it works is under "Running and settings".
    One fault was seen after switching the preset in game (`gmv_walk_ray` reading a freed walker, inferred); since
    then the pool copies the switch made are kept until the game ends instead of being freed, as the Mac frees nothing
    of its pool before a load (`density.c`). Off under the preset: the game's fixed shadow.
28. **Two movement keys swung the body (`KEY_MOVE`, port bug in the enhancement).** Each move call (0x685d2) turns the
    body by half its error from a facing renewed only when a frame is drawn, so calls between two draws add up (0.5,
    1, 1.5, 2, 3 of the error for 1, 2, 3, 4, 6 calls); above 2 the error grows. The handlers sent a call for each
    held key; now only one held key sends the move. Off under the preset.
29. **Slow frames on the way to the main menu (`LOAD_BUDGET`).** A `LOAD_PROFILE` run found three causes. The
    game's own load before the title is left alone (the Mac runs the same). The port's sun/moon glow blur (`glow_box`)
    took a tick of 162.6 ms; it now blurs four channels together with SSE2, 53.8 → 1.3 ms a texture. The game builds
    1500 collision items a tick while it waits for streaming, hitching 43-131 ms; `LOAD_BUDGET` takes 200 a tick
    instead (0 keeps 1500). On under the preset: how long a stall lasts depends on the machine, not on the game.
30. **A release build has no debug mode.** Every trace, survey, capture, profiler and test aid depends on
    `GMV_DEV`, so the compiler leaves it out of a release build: no console, no `[Diagnostics]` or `[Testing]` key, no
    staged flag, no `GMV_*` environment override (`gmv_envkey` is NULL there), and no heap addresses, game paths or
    song names in the log. The log's `LOG` key sits in a `[Log]` section. The fixed-timestep experiment (`FIXEDSTEP`,
    `FIXED_MS`) was removed from every build. Checked by compiling every file both ways under `-Wall -Wextra` and by
    scanning the release exe for debug tags and private strings.
31. **The start-up Loading screen, one step a frame (`FAST_LOAD`, speed-up).** The loading state (vtable 0x65a288,
    update 0x1b589e) calls its step function (0x1abc92) once every 33 ms until step 16. The patch runs as many steps a
    frame as fit in a budget; the steps and what they load are the game's own. From the end of the warning to the
    title: 2.08 s → 0.73 s. On under the preset: the same loading steps, sooner.
32. **The defaults.** A new `GangstarMV.ini` turns the enhancements on (`FRAME_MS=8.3333`, every graphics effect,
    `DRAW_DISTANCE=2`, the densities 3, every gameplay, control and menu key, the add-ons). Only the written defaults
    changed: a key left blank or missing still means the call site's built-in value, which for an enhancement is the
    Mac's, so an older ini keeps what it says. Without the add-on files the radio keeps its five stations and the
    Crusher is left out, with a log line each.
33. **A line of picture along the top and left of a black fade (`FADE_EDGE_FIX`, original bug).** The game's 2D
    projection is moved by 0.375 of a pixel, so a rectangle from (0,0) starts inside the top-left corner. Without
    samples that still covers the first row's and column's pixel centres; with 4x MSAA, the Mac's own setting, some of
    their samples stay uncovered. The fix starts such a rectangle at (-1,-1) (`fix_fade_edge.c`, site 0x4c5db8). Off
    under the preset.
34. **Reginald's missions forgotten by every save (`MISSION_SAVE_FIX`, original bug).** The mission manager keeps one
    completion bit per mission in 8 words, one per group (mgr+0x90); group 8 is Reginald's five missions. The save
    writer's MMSK loop (0x98699-0x986da) stops at the group count, 7, though the reader takes indices up to 7. The loop's
    test at 0x986d4 becomes `cmp edi, 8; jl`, so all 8 words are written, and the save still loads in the original
    game. On under the preset: a save made under it must keep them, or `MISSION_REPLAY` would lose them once the
    preset is off.
35. **Speed-ups of the port's own effects and file access (`SHARED_RESOLVE`, `FOG_SKIP_FAR`, `QUERY_FREE`,
    `FILE_MISS_CACHE`, `FILE_POOL`).** Each gives the same result, is on by default and under the preset, and `=0`
    brings the old way back. Measured with the `bench-city` scenario and `tools/playtest/bench.ps1`.
    - `SHARED_RESOLVE`: the effects that read the screen's depth share one resolve of the multisampled screen a frame
      instead of up to five. Checked pixel for pixel.
    - `FOG_SKIP_FAR`: with `FOG_MODE` 1 or 2, draws of the far city wholly past the fog's clip are not sent.
    - `QUERY_FREE`: the effects remember which framebuffer and shader they left bound instead of asking GL around each
      draw (~60,000-90,000 queries a second in the city). Checked against GL.
    - `FILE_MISS_CACHE`, `FILE_POOL`: the game's probes for loose files outside the packs are answered from a list,
      and its pack files stay open for a while after it closes them; each failed or new open on Windows is a kernel
      call of ~70-80 µs.
36. **`SHADOW_QUALITY`.** On a GPU built into the processor the shadow map's 16 lookups a pixel into a 2048 x 2048 map
    were the heaviest part of the shadow pass. `2` keeps that; `1` makes a 1024 map and takes 8 of the same 16 taps in
    a draughts-board pattern, so the filter covers the same texels with softer, blockier edges. The slope bias is in
    map texels, so it follows the map's size. Default `auto` (item 38). The HUD's UI lookups were measured in the
    same pass and left as they are.
37. **The client arrays kept by the host, and `STREAM_VBO`.** The game sets its array state only through the GL
    calls the host wraps, so `QUERY_FREE` also keeps what its vertex arrays point at (`gl_arrays.c`) instead of
    ~50,000-60,000 queries a second. `STREAM_VBO` (`gl_stream.c`) copies the vertices of each draw from client arrays
    into a buffer of the graphics card's before the draw, the copy the driver otherwise makes inside the call; on
    NVIDIA that was most of the main thread's driver time. The same bytes are drawn. On under the preset; needs
    `QUERY_FREE`.
38. **`GPU_PROFILE`, `PRECISE_WAIT`, `BACKGROUND_THROTTLE`, `SPIKE_MS`.** Exposed by a player's full log on an RTX
    3050 Ti (73,740 ticks, clean, but frame-time spikes of 79.1 and 55.0 ms that a release build could not place).
    - **`GPU_PROFILE`** (`gl_gpu.c`, generic): once GL's names are known, a thread of its own asks Direct3D 11 whether
      the adapter OpenGL draws on (matched by vendor, then by name) shares the CPU's memory
      (`D3D11_FEATURE_D3D11_OPTIONS2.UnifiedMemoryArchitecture`). NVIDIA is not asked. Without an answer, as on Linux,
      the renderer's name decides. The settings left on `auto` follow it: `SHADOW_QUALITY` 1 on a built-in GPU and 2
      on a graphics card, `MSAA` 4 on both. A number in the ini or the menu always wins; `igpu` and `full` force a
      kind, `0` gives every GPU the graphics card's values. No effect under the preset, which sets `MSAA` 4 and has no
      dynamic shadows.
    - **`PRECISE_WAIT`** (`gl_present.c`): the pacer used to spin the last 1.5-2.5 ms of each tick in `Sleep(0)`. It
      now sleeps on a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`; `nanosleep` on Linux) to
      a margin before the deadline and yields for the rest. The margin is learnt: a running 90th percentile of how late
      the timer wakes, plus 0.05 ms, because on the test laptop the timer woke 0.25-0.5 ms late and a fixed 50 µs
      margin left most ticks late. One core at 150 ticks/s: 56-62 % → 39-43 %, with ~1 % of ticks late. The ticks
      come at the same deadlines, so it stays on under the preset.
    - **`BACKGROUND_THROTTLE`** (`gl_present.c`, with the game hooks `gmv_game_tick_period` and
      `gmv_game_minimized`): behind other windows the game draws at most `BACKGROUND_FPS` (30) frames a second and
      keeps running; minimized it ticks for `MINIMIZED_FPS` (10), flushes instead of presenting, releases the 1 ms
      timer resolution, and pauses a game in progress with the game's own Esc when the player is in play. One core:
      ~57-62 % → ~20 % behind other windows and 1-4 % minimized; no audio underruns. Off under the preset.
    - **`SPIKE_MS`**: logs each tick longer than this many ms with its number, its work and its present, at most ten
      lines a second; in release builds too. Default 0 (off).
    - **Privacy:** `-ffile-prefix-map` now covers every build, the plain one included, and the release exes were
      scanned in 8-bit and UTF-16 for local paths and names.
39. **Smoother edges: `ALPHA_TO_COVERAGE` and `POST_AA` (enhancements).** With `MSAA` on, the world's alpha-tested
    cut-outs (leaves, fences, railings) kept one hard edge per pixel: the alpha test passes or fails all of a pixel's
    samples together, and the engine never turns on `GL_SAMPLE_ALPHA_TO_COVERAGE` (0x809e). At `MSAA=0` nothing was
    smoothed.
    - `ALPHA_TO_COVERAGE` (`draw_hooks.c`, `a2c_begin` / `a2c_end`): 0x809e is on for one draw when `MSAA` is 2 or
      more, the game's alpha test is on, unit 0 is textured, blend is off, depth writes are on, the screen FBO is
      bound and the projection is a perspective one. The alpha test stays, so a cut-out keeps its shape; only its edge
      pixels get partial coverage. The HUD fails the perspective test.
    - `POST_AA` (`post_aa.c`): FXAA 3.11 written from its description, one full-screen pass at the end of `ao_run`,
      after bloom and before the HUD, with alpha masked. It reads bloom's resolved scene copy when there is one
      (`post_color()`), else a blit of its own. `1` = always, `auto` = only while `MSAA=0`, `0` = off. `paa_init()`
      runs in `ao_run`'s gate, before any shader call; a first build called `ActiveTexture` before the entry points
      were loaded when FXAA was the only effect on.
    - A draw-call probe (dev, `GMV_DRAW_PROBE=1`) found that the engine already filters its state calls and gives each
      object its own matrix, so no draw merging was added.
    - Off under the preset.
40. **`TIME_OF_DAY` and `RANDOM_TIME_OF_DAY` (enhancements).** The game's clock is one object at 0x68a100: the period
    at +4 (morning, day, sunset, night), the clock in ms at +8, a paused flag at +0x1c. Its update 0x141ff4 runs it at
    120 times real time (a 12-minute day), and 0x142892 works out the period, colours, fog, sky, sun and moon from the
    clock every tick. setTime is 0x14172c (tod, hour, minute). The clock is not in `Gangstar2.sav`, so neither key
    changes a save.
    - TIME OF DAY tab: SERVICES slot 5 (`btnMenu5`, shown through the hook at 0x165297) opens a second ServiceMissions
      page registered as `ServiceTime`, with MISSION REPLAY's header widget moved to the map's middle. Its arrows set
      the hour one back or forward and the minutes to 0 with 0x14172c, then run the update once with dt 0, so the
      world changes with the menu open. Hidden during a mission or a replay.
    - Random start: only CONTINUE's load call (0x1692bd) goes to `gmv_tod_load`, which picks a period at even odds and
      a minute in it with its own generator, so the game's `rand()` sequence is not moved. NEW GAME, respawns and
      replays are left alone.
    - Both keys read 1 when blank or missing (`absent` in `settings.c`, `fill_absent` in `core/config.c`), so an older
      ini gets them. Off under the preset.
41. **Static collision lost with `DRAW_DISTANCE` above 1 (port fault in item 13's fix).** While the city loads after
    CONTINUE the camera sits at (0,0,0) for two frames. Item 13's `gmv_update_cells` took any (0,0,0) last position at
    +0x1a4 for a new grid and moved it, which also skipped that update's drop pass. The cells round the centre stayed
    marked as loaded while 0x134fc0 dropped their queued collision, so houses, fences, hedges and trees there could be
    walked or driven through for the rest of the session. `oc_grids` (`origin_collision.c`) now keeps each grid's last
    camera position, and (0,0,0) counts as a new grid only for a grid not seen before or last updated elsewhere. No
    work a tick is added; `DRAW_DISTANCE=1` and the preset are as before. A dev probe (`GMV_COLLISION_PROBE=1`) lists
    the static objects near the camera that have no body.
42. **`VirtualQuery` in `ms_rd` and in the walk-ray report (host cost).** `ms_rd` (`game_read.c`) checked every read
    with a `VirtualQuery` call, and `pd_ws_report` (`density.c`) ran after each walk ray in plain and dev builds;
    together they took 8-13 % of the main thread. `ms_rd` now reads addresses inside the game image (0x11000 to
    0xc68000, all mapped readable) directly, and the report waits for `GMV_POOL_PROBE=1`. The values read are the
    same, so the preset is unchanged. The lower frame rate reported on some laptops was the hybrid GPU's choice, not
    the code: the exe exports no `NvOptimusEnablement`, so Windows may run it on either GPU, and NVIDIA's GL driver
    costs several times more CPU per draw than the AMD built-in GPU's.
43. **AI drivers weave on straight roads (`AI_STEERING_FIX`, original bug).** "Drive to point" 0x6ea88 steers in
    proportion to the 3D angle between the vehicle's forward axis and the point. A point above or below the car keeps
    that angle from reaching zero, and the side swaps each time the car crosses the line to it, so the wheel turned
    one way and then the other several times a second. Clive's van in ON A LION HUNT, whose path points stand about 2
    m above it, weaved through the chase at 60 and at 150 ticks/s. At 0x6ecc1 the load of `setSteer` (vtable +0x11c)
    becomes a call through `ast_entry` to `gmv_ai_steer`, which takes the side and the angle on the ground plane with
    the game's own gain and then calls the vehicle's `setSteer`. Off under the preset; on when blank or missing. Dev
    aids: `GMV_STEER_PROBE=1`, `GMV_TEST_CHASE=1`.
44. **Clive treads water over his yacht's seat (`BOAT_SEAT_FIX`, original bug).** Watched to its end, the cutscene in
    ON A LION HUNT runs Clive off the dock and swims him to the yacht. `ObjectMoveTo` then seats him (0xcf25d, `call
    0x72914`), but his behaviour tree is still in its swimming state, and on the next tick it plays the swimmers' idle
    over the seated pose. Skipping the cutscene puts him on the yacht at once, so the tree never swims. At 0xcf25d the
    call goes to `gmv_boat_seat`: for a character in deep water by the game's own test it clears his run target and
    steps his behaviour tree once, then seats him, so the seated pose is the last one played. Off under the preset; on
    when blank or missing. Dev aids: `GMV_BOAT_PROBE=1`, `GMV_TEST_BOAT=1`.
45. **GL state kept by the host, and the shadow and car-reflection marks batched (host cost on NVIDIA).** With
    NVIDIA's threaded optimization each `glGet` waits for the driver thread to catch up, and `ds_mark`, `car_mark` and
    `sea_mark` switched framebuffers, pushed attributes and looked up uniforms for every caster.
    - The host tracks the state the effects used to ask for: scissor, stencil, cull, polygon fill, colour mask, depth
      func and clear values in `g_glt`; texture sizes by name (`gmv_gl_tex_size`); the modelview and projection stacks
      (`ao_matrix()`, from the matrix intercepts in `sun_moon_glow.c`). The marks set and restore their own state
      instead of `glPushAttrib`. The effects make no GL queries in play.
    - `ds_mark` and `car_mark` queue each draw through `mark_take` (`draw_hooks.c`), which copies client arrays and
      indices into a staging buffer a frame and keeps the game's VBOs by name. `ds_flush` and `vr_flush` replay the
      queues in one pass a frame at the top of `ds_pass` and `vr_pass`.
    - `vr_flush` sets `glDepthMask(GL_TRUE)` before its clear: inside `ao_run` depth writes are off, and without it
      the old depth stayed and every car reflection failed its depth test.
    - It also sets `glColorMask` to all four channels. `ds_pass` and `ssr_pass` run before `vr_pass` and leave alpha
      writes off, so in a frame with shadows or sea reflections the marks had no alpha, the pass dropped every car
      pixel, and the old alpha outlived the clear as stray marks. Johnny's car had no reflection in the driving and
      close-up shots of the MIAMI BOUND! cutscene.
    - `vr_pass` reads near and far from the projection by the sign of `P[11]`. The game's projection looks down +z
      (`P[11]` = 1), and the old formula, written for GL's -z, swapped them: near about 90000 and far 50, so rays
      ended after 100 units and most pixels fell back to the sphere map.
    - The sphere-map fallback uses the classic sphere map about the view axis, `d.xy / (2 sqrt(2 + 2f))` with f the
      forward part of the reflected direction, in place of `d.xy / 2`. The old lookup sent a ray and its mirror
      through the view plane to the same texel, which folded the picture into a mirrored seam where the rays turn
      towards the camera. The map pinches at straight back, so rays within about 66 degrees of it fade out, fully
      within 18; the Fresnel term is low there anyway.
    - `ssr_pass` reads near and far the same way. With the old formula the sea's `reach` was 100 units, so its march
      found almost nothing and the reflection came from the vanishing-point lookup alone.
    - `ao_run` (`bloom.c`) loads the shared GL entry points (`ao_procs`) before its first `glActiveTexture`. Sea marks do not load
      them, so a frame where the sea was the only effect (ambient occlusion and bloom off, no car or shadow mark yet)
      called address 0 and crashed.
    - Same picture. On NVIDIA a busy city runs about half as fast again; the AMD built-in GPU gains less. No change
      under the preset, which has none of these effects.
46. **Spawn pacing, stuck traffic and the chased car (`density.c`, with the density keys).**
    - Caps and pacing: the spawn check 0x080272 takes the point's own cap or the built-in 18 / 10 / 14, the profile's
      limit, and a total under 48. The port's caps are now exactly the Mac's times the setting, the record's waits are
      no longer divided by it, and a scaled kind spawns at most once a tick. One spawn costs about 4 ms, and a refill
      used to put up to 9 into one tick.
    - Stuck traffic (original: the game has no stuck recovery; it shows at higher densities): the vehicle step at
      0x1a1f80 goes through `gmv_tj_step`. With `TRAFFIC_DENSITY` above 1 a traffic car (spawn kind 3, no chase flag)
      blocked for 3.5 s in a box 15 m ahead reverses and steers for 1.2 s; one stuck for 9 s and more than 30 m from
      the player is removed through the despawn at 0x1a1baa (`gmv_tj_keep`). A blocker driving the same way does not
      count, so queues are left alone. A traffic spawn is refused within 12 m of a stopped or crawling car
      (`gmv_tj_clear` at 0x7fe99).
    - The chased car: a vehicle whose driver has the chase flag (+0x584 & 4) and a script-set speed (+0x180 &
      0x20000000), and whose target is not the player. While it touches traffic and is not braking or reversing, its
      forward speed is put back to the speed before the contact for up to 1 s, and the traffic car it hit drives on
      (or backs up) for 1.75 s, steering away from its path. Not during races.
    - Traffic below a helicopter: the spawn centres follow the camera in 3D. A spot must lie within 3 m in height of
      the vehicles' centre A (0x688cc0, the test at 0x7fdb2 in 0x7fcea), and the despawn at 0x1a93aa takes the 3D
      distance to centre B (0x688cf0), so from a helicopter no traffic spawned and the cars below were taken away.
      While the player flies one (vtable 0x6568c8) with `TRAFFIC_DENSITY` above 0, `gmv_tj_spot` (the calls at
      0x82b8c and 0x82bbe) gives the vehicle centres a traffic spot's height, and `gmv_tj_keep` gives centre B a car's
      height, both only within 150 m, so the distances are 2D. Pedestrian spots are refused while flying.
    - Off under the preset: the densities are 1 and the hooks are out.
47. **Checkpoint markers kept out of the bloom (`BLOOM`, enhancement).** At night the cyan checkpoint domes are bright
    additive draws, so the bloom's bright pass and, with `BLOOM=2`, the glow layer spread them over the road and the
    traffic ahead. Their `gloweffects` texture is shared with car and police lights, so the texture cannot tell them
    apart; the scene node can. Every 3D additive draw comes through the transparent pass 0xb96a8, which calls
    `node->render` at 0xb9a2f; a node's name is at +0xc and its parent at +0xd8.
    - The 12 bytes at 0xb9a26 become a call to `gmv_transparent_render` (`fog_distance.c`), which sets `g_cp_draw`
      while a node, or one of up to 3 ancestors, is named `checkpoint_` followed by `race`, `mission`, `drive`,
      `store`, `elevator` or `arrow`. Money and briefcase pickups keep their glow.
    - Such a draw leaves the glow layer alone (`bloom_glow.c`) and marks its pixels with stencil bit 0x20
      (`bg_cp_mark`; 0x80 is the sea's, 0x40 the car bodies'). `bl_pass` (`bloom.c`) blits that stencil with the scene
      copy and blacks out the marked pixels before the bright pass. The marker itself is drawn as before; lights seen
      through it lose their bright-pass bloom there.
    - Installed only with `BLOOM` above 0 or the GAMEPATCH tab live; out under the preset.
48. **Bloom colours and threshold (`BLOOM`, enhancement).** The bloom laid a haze over the whole picture in the
    colours of the scene. The bright pass judged the world by its brightest channel, and its knee was half the
    threshold wide, so at the menu's HIGH (`BLOOM_THRESHOLD=0.5`) it started at 0.25: daylit walls and pavement, the
    blue sea, green grass and, at night, the bluish pavement all passed in part. The composite cut the glow per
    channel at 1, which turned a strong orange glow yellow.
    - `bl_fs_pre` (`bloom.c`) takes, on the world, the luminance (0.2126, 0.7152, 0.0722) plus 0.35 of the spread
      between the strongest and the weakest channel, at most the strongest channel; the sky keeps its dimmest channel.
      Grey walls and pavement are judged near their luminance, and a saturated neon sign or tail light still passes.
    - The cut is 0.74 + 0.26 x `BLOOM_THRESHOLD`, 0.17 lower at night (`bg_night()`, the same curve as
      `BLOOM_WINDOWS`), and the knee is 0.65 of 1 minus the cut on each side. At HIGH that is 0.87 by day and 0.70 at
      night, so the knee starts at 0.79 and 0.51; at 0.95 the cut stays below 1.
    - `bl_fs_up`: each upsample adds the coarser level at 1.08, so the widest level weighs about 1.5 times what it
      did. In the final pass, where the largest channel m passes 1, the glow becomes c / m moved 0.2 - 0.2 / m of the
      way toward white, in place of `min(c, 1)`. The screen is 8-bit, so anything left above 1 is cut per channel,
      which is what turned orange yellow.
    - `BG_LIGHT_GAIN` (`bloom_glow.c`) is 2.45, up from 2, so the light sprites glow more at `BLOOM=2`. The windows'
      gain is divided by it, so they are as before.
    - The game's clock is now read whenever `BLOOM` is above 0 (`bloom_clock_on`, formerly `bloom_windows_on`, which
      asked for `BLOOM=2` with windows). The checkpoint stencil (item 47) is unchanged; the preset has no bloom.
    - Lit shop fronts at night are dimmer on screen than sunlit pavement, so a single cut could not keep the one and
      drop the other; hence the night offset. Night pavement is bluish: with the channel spread it reaches 0.44 at
      p99, against 0.64 at p90 for the shop fronts, and the night knee starts between the two.

49. **Helicopter rotor wear above 60 ticks/s (`PHYSICS_SYNC`, port fault).** `ContactListener::Persist` (0x48bb0)
    calls the rotor check 0x129594 at 0x48ca9 once per Box2D step, and each contact inside the rotor disc costs the
    helicopter 1 health, so one step per tick wore it down faster than the Mac's 1/60 s steps. `gmv_rotor_persist`
    (`sync_physics.c`) lets that call through on at most one step per 1/60 s of physics time; it is installed and
    removed with the physics sync, so the preset keeps the Mac's.

## Diagnostics

The diagnostics are `[Diagnostics]` and `[Testing]` keys of the ini. They work in the plain and `-Dev`
builds only: a release build does not contain them (item 30). 

- `GMV_DUMP_FRAME=<n>` captures back-buffer frame *n* to `gmv_frame.bmp` (written in the game's cwd,
  the project root).
- `GMV_FLICKER_CAP=1` (`flicker_cap` in `render/present_hold.c`) watches every presented frame for 60 s from the
  rating splash or the title / main menu state and logs `[flickcap]` per-frame stats. It saves frames
  that differ from both neighbours (A-B-A) or flash dark/bright, with their neighbours, as
  `gmv_flick_e<n>_{1,2,3}_f<frame>.bmp` (plus every 30th frame as `gmv_flick_ctx_*`).
- `GMV_CAM_TRACE=1` logs `[camtrace]` camera-follow stats every 2 s (frame dt, the camera's trail
  behind the character, character and eye speeds).

**Error box.** Every way the game or the host stops on an error goes through `gmv_fatal` in
[`host/core/log.c`](../host/core/log.c): it writes the crash dump, closes the log, silences the sound, lets go of the
mouse, shows a message box with the reason and ends the process with the same exit code as before. The
box runs on a thread of its own, because the stopped thread may own the game's window, and a box there
would run that window's messages through the game's WndProc. A second thread that stops meanwhile waits.

| Code | Stop |
|---|---|
| 1 | The static initializers or `main` could not start |
| 3 | A missing import was called |
| 4 | Missing data was touched, or `abort`, `__stack_chk_fail`, a failed `assert` |
| 5 | C++ runtime: pure virtual call, `bad_alloc`, `length_error` and the like |
| 6 | An access violation (the vectored handler in [`host/core/veh.c`](../host/core/veh.c)) |
| 7, 8 | An unhandled Objective-C message; an Objective-C exception nothing caught |
| 9 | Any other unhandled exception, such as an illegal instruction (`last_chance` in `veh.c`) |
| 10 | The window, pixel format or OpenGL context could not be made |

Problems the port survives do not show a box: the guards such as `CRASH58_GUARD` and an enhancement that
falls back only log a line. `TEST_FATAL` (plain and `-Dev` builds) stops the game about 10 s in, to try it.

**Crash dump.** On every code but 1 and 10, where the game never got to run, `gmv_fatal` first writes
`cache\GangstarMV.dmp` ([`host/core/crashdump.c`](../host/core/crashdump.c)). A helper thread does it, with dbghelp
from System32 loaded only then, and gets 20 s; a file it could not finish or clean is deleted, and the box
names only the log. The exception record comes from the first fault (`gmv_dump_note`), so the dump opens on
the faulting thread; a stop with no exception shows every thread where it stands. Every build uses
`MiniDumpNormal | MiniDumpFilterModulePaths`. `MiniDumpWithIndirectlyReferencedMemory` was tried and dropped:
it brings in the heap, where the C library's copy of the environment (the user's name, anything kept there) and
freed paths broken into pieces cannot all be found again. `log_scrub_bytes` then blanks every copy of the log's
hidden folders with the rest of the path after them, and every value of the environment, in either slash and any
ASCII case, as UTF-8, the ANSI code page and UTF-16, keeping the length so the file stays valid. A bare file name
can remain, which is why the README warns about it. A log line that held a hidden folder leaves the rest of the
path behind the shorter text in its stack buffer, where nothing could find it again, so `out` wipes the buffer.
Game code sits at its own fixed addresses, so a dump's stack can be read against the Mach-O with no symbols.

On Linux the same file is an ELF core ([`host/core/crashdump_linux.c`](../host/core/crashdump_linux.c)), which gdb
opens with the program: `gdb GangstarMV GangstarMV.dmp`. The faulting thread writes it inside its signal handler,
so it uses only system calls and static buffers, and it sends memory to the file with `write()`, which answers
EFAULT for a page it cannot read rather than faulting. The other threads, listed from `/proc/self/task`, get a
real-time signal, keep their registers and wait in the handler until the file is written. Mesa's threads block
every signal, so they are left out (6 of 40 threads with llvmpipe). The file holds an NT_PRSTATUS for each thread
(the faulting one first, with NT_PRPSINFO, NT_SIGINFO and NT_FILE), and each thread's stack from esp up, at most
1 MB. NT_FILE has the mapped files by name only, so gdb warns that it cannot open them. On Linux, `log_scrub_bytes`
blanks the hidden folders (each also with its links resolved), every value of the environment and the machine's
name, longest first: blanking the user's name first left the game's folder, OLDPWD and PATH in the file, the user's
name gone from them but the rest no longer matching. `TEST_FATAL` 1-4 ran in WSL: each wrote the file,
gdb showed the faulting line and the threads, and the file held no user name, machine name or folder.
`LOG=0` means no dump, and each start deletes the previous one, since the new log would not match it. The
playtest's `crash-*` scenarios try all of this in the game (`dumpclean`).

**AZERTY keyboards.** The game has its own answer. `0x19f0b0` compares the layout's id with
`com.apple.keylayout.French`, and the four input handlers pass the result to `0x18ac4e`, which binds Z and Q to
forward and left, A and W to what Q and Z do on other layouts. `0x18214` also adds the top row's unshifted symbols
as digits, which the port does not need: it sends digits by their virtual key, and those stay in the table.
`TISGetInputSourceProperty` answers with that id when the key at W's place is Z and the key at Q's place is A,
so Belgian counts too, which the Mac's test left out; any other layout gets NULL as before. Keys reach the game by
their label through `gmv_vk_to_sdl`, as they do through the Mac's SDL, so a scancode table would swap them twice,
and QWERTZ needs nothing: W, A, S and D stand where they do on QWERTY. The bindings are kept in
`Gangstar2.preferences`, so a preferences file from a QWERTY start keeps W: the `azerty` scenario needs `noprefs`.
It is the Mac's own behaviour, so it has no key and stays on under `ORIGINAL_MAC_EXPERIENCE`.

**Build check.** [`.github/workflows/build.yml`](../.github/workflows/build.yml) compiles the `-Release` and `-Dev`
builds on every push and pull request, with no game files and no exe kept. They compile different code (`GMV_DEV`).
A compile error or any warning fails it.

**Playtest.** [`tools/playtest/playtest.ps1`](../tools/playtest/playtest.ps1) plays the scenarios in
`tools/playtest/scenarios` and says PASS or FAIL for each. A run has a folder of its own, `build\playtest\run`,
with a copy of the exe and only the keys the scenario names, so `build\savegame` is never touched; what it leaves
goes to `build\playtest\results\<time>\<scenario>`. Input is real (`SendInput`), since parts of the port read key
states and raw mouse, so the window needs the foreground: if it loses it, the scenario stops with FOCUS LOST and
sends nothing more. Screenshots are copied from the screen, so a release exe can be run too (`-Exe`; scenarios
marked `dev` are skipped). Any `[fault]`, `[MISSING]` or `[crash58]` line, unexpected box, unexpected exit or hung
window is a FAIL. `selftest-must-fail` holds only wrong expectations: it must say FAIL on each. `moved` and `still`
compare two screenshots, so walking is checked without a person; `layout` and `holdscan` press keys by their place
on the keyboard under a layout that is only loaded for the run. The main menu's scenes change by themselves, so `moved`
says nothing while the menu is up, and the log has no line for reaching the city: `leftmenu` compares where the menu's
buttons are. `pad` and `padfor` hold what a dev build's virtual pad (`PAD_TEST`) presses, through the same SDL3 calls as a
real pad; it says nothing of real drivers. `camera` and `uncapped` are for comparing two builds with `-Exe`: how far a known
mouse move turns the camera, and the frame rate with no pacer. `drag` sizes the window by Windows' own sizing loop,
started by `WM_SYSCOMMAND`, as the game keeps the cursor inside its picture and a press on the border never gets there;
the corner moves as far as the mouse does from where it stands, and only as far as the picture lets the cursor go, so
`resize` and `resize-option` place it first or allow for it. A person's drag at the border is not tested by this.
`drag <dx> <dy> <shot>` also takes a shot with the mouse still down.

**The probe loop still applies** for any new `[MISSING]`/`[objc]`/`[fault]` wall: implement the one
named symbol, rebuild, repeat. The fault handler dumps registers, the faulting module and a game-range
stack scan. Keep every change building clean under `-Wall -Wextra` (36 host C translation units + 3 asm)
and stage 1 passing.
