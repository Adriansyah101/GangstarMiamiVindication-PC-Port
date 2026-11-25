# Playtest scenarios

`tools/playtest/playtest.ps1` plays scripted tests of GangstarMV.exe and reports PASS or FAIL for each.

```
tools\playtest\playtest.ps1                     every scenario in tools\playtest\scenarios but selftest-*
tools\playtest\playtest.ps1 crash-*,boot        the ones named (wildcards work)
tools\playtest\playtest.ps1 boot -Exe <path>    another exe, such as a release build (dev-only scenarios are skipped)
tools\playtest\playtest.ps1 -List
```

Each run gets its own folder, `build\playtest\run`, with a copy of the exe; `build\savegame` is never touched. Only the keys a scenario names are set, the rest are defaults. Results go to `build\playtest\results\<time>\<scenario>`: `result.txt`, the log, the screenshots and any crash dump.

Input is real (SendInput): the window takes the focus, mouse and keyboard while it runs. If the window loses the focus the scenario stops with FOCUS LOST and no more input is sent.

## Scenario files

One step a line; `#` starts a comment. The first line is the description `-List` shows.

```
Before the first other step:
  ini KEY=VALUE        | save <file>  (copies build\savegame\<file> and Gangstar2.preferences into the run)
  noprefs              (after save: without the preferences file, as on a first start)
  env KEY=VALUE        (an environment variable for the game, such as a dev build's GMV_* switch)
  noini                (no GangstarMV.ini: the game writes its own with the defaults; ini lines are then ignored)
  addon                (copies build\add-on into the run; a run has none otherwise)
  songs                (copies build\playtest\songs into the run's custom radio folder; fails if it is not there)
  dev                  (this scenario needs a dev build's keys or log lines)
  allow <regex>        (a log line this scenario expects to break the "no fault" rule)
Then:
  wait <s>             | waitlog <regex> [timeout s]    the log gets a matching line
  click <fx> <fy>      | rclick <fx> <fy> | move <fx> <fy>    fractions of the client area
  key <name> [ms]      | hold <name> | release <name>    A..Z 0..9 ENTER ESC SPACE TAB SHIFT CTRL ALT UP DOWN LEFT RIGHT F1..F12
  holdscan <hex> | releasescan <hex>    a key by its place on the keyboard, which the layout then names
  layout <id>          the game's window uses this keyboard layout, such as 0000040C (French); put back at the end
  look <dx> <dy> [n]   relative mouse motion, in n steps, for the camera
  drag <dx> <dy> [shot] the window's bottom-right corner moved this far, by the mouse; as the game keeps the cursor inside its
                       window, this starts Windows' sizing as the system menu does, and a person's drag ends in the same loop
  size <w> <h> [px]    the client area is this big, within px (default 0); fails if not. A `drag` can end a few pixels short
  maximize | restore   the window's own buttons, by the commands they send
  cpu <name> <s>       the game's CPU time over s seconds, printed as a share of one core
  minimize <s> [cpu]   | unfocus <s> [cpu]    minimized, or behind a small window of the script's, for s seconds; then
                       in front again (cpu: the game's CPU share meanwhile, from 2 s in)
  pad [state]          needs ini PAD_TEST=xbox|ds4|ds5|raw (a dev build's virtual pad): what it holds from now on, nothing
                       for all released: X A B Y LB RB LT RT BACK START LS RS PS TOUCHPAD UP DOWN LEFT RIGHT lx= ly= rx= ry=
  padfor <ms> <state>  holds that for so long, then all released
  shot <name> [full]   the window's picture, to <name>.png: 960 pixels wide, or full = at the window's own size
  moved <a> <b> [min]  | still <a> <b> [max]    two shots differ by at least / at most this much (default 12, of 255)
  edge <a> [max]       shot a (taken full) has no line along its top row or left column: neither is brighter than the
                       row or column two pixels in by more than max (default 3, of 255)
  leftmenu <menu> <b>  shot b no longer shows the main menu's buttons that shot menu shows; its scenes change by themselves,
                       so a `moved` in the menu proves nothing and the log has no line for reaching the city
  expectlog <regex>    | nolog <regex>
  expectfile <path>    | nofile <path>    relative to the run's folder
  inihas <regex>       a line of the run's savegame\GangstarMV.ini matches
  inisec <section> <key>...   each key's line in the run's GangstarMV.ini stands in that [section]
  expectbox <regex> [timeout s]    the error box appears with this text; it is closed
  expectexit <code> [timeout s]    the process ends with this exit code
  dumpclean            the crash dump holds none of: the user's name, the computer's, the user's folder, the project's
A step written dev: <step> or release: <step> runs only with that kind of build.
Every scenario fails on a [fault] or [MISSING] line, an error box or an exit it did not expect, or a window that hangs.
```
