<!-- Copyright (C) 2026 WideMelon contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Changelog

## Unreleased (based on WideMelon 1.0.4)

### Added

- **Close gaps between polygons** (Config > Video settings, on by default).
  The classic OpenGL renderer widens the edges of solid polygons by one
  pixel so neighbouring tiles overlap. This removes the thin black lines and
  dots between terrain tiles, for example in the Pokémon games. The DS
  rasterizer covers pixels that an edge only touches, while OpenGL only
  covers pixels whose centre is inside the polygon; the overlap makes up for
  that difference. Translucent, shadow, line and clamp-textured (menu)
  polygons are unchanged. Setting: `3D.GL.CloseSeams`.
- **OpenGL (Compute shader) renderer in widescreen.** It can now be
  selected in widescreen mode. It reproduces the DS rasterizer on the GPU and
  needs OpenGL 4.3; without it, WideMelon falls back to classic OpenGL and
  shows a message. The Software renderer still can't produce a widescreen
  image and stays disabled in widescreen.
- **Motion smoothing** (Config > Video settings, off by default). Generates
  in-between frames like the motion smoothing on TVs: WideMelon estimates how
  each part of the picture moved between two real frames and draws the
  frames in between along that motion. Setting: `Screen.MotionSmoothing`.
  - 30 fps games, such as the Pokémon overworlds, play like 60 fps on a
    60 Hz display.
  - On a 120 Hz or faster display, every game gets extra frames as long as
    emulation is fast enough (for example with the JIT recompiler). Otherwise
    WideMelon shows one frame per emulated frame, held for two refreshes, so
    the game keeps full speed.
  - Repeating textures such as tall grass follow the camera's motion instead
    of jumping to a look-alike position one pattern repeat away.
  - Fades, scene changes, menus, text boxes and submenus (for example
    SUMMARY / SWITCH / ITEM / CANCEL on the party screen) show the real frames
    instead of in-between frames, so they never appear garbled. Walking
    characters and animated icons on a still screen are still smoothed.
  - It adds a short delay (one frame for 30 fps games at 60 Hz, half a frame
    for 60 fps games at 120 Hz), needs the OpenGL display, and pauses during
    fast-forward and slow motion. The phone screen keeps receiving the real
    frames.
- **Reduce in-game slowdown** (Config > Emu settings, on by default). Some
  games occasionally miss a frame of their own while loading, for example the
  Pokémon games while streaming map data as you walk through a town. The
  picture then pauses for one or two frames and jumps, which is most visible
  on building edges and roofs, even though the emulator's frame rate stays at
  60. This setting runs the ARM9 at twice the DS clock (133 MHz, as a DSi can)
  and reads the game card at one bus cycle per byte instead of five or eight,
  so those frames arrive on time. Walking and running through Jubilife City
  went from 11 late frames in 1,300 to none, with or without the JIT, at
  about 0.5 ms of extra emulation time per frame. Not hardware-accurate; turn
  it off if a game misbehaves. Savestates load correctly whichever way this
  was set when they were made. Setting: `Emu.ReduceSlowdown`.
- **Overlay screen layout** (the new default; View > Screen layout >
  Overlay, or the Screen layout setting in WideMelon's settings). The top
  screen fills the whole window, centered and scaled to the window's height
  without stretching, and the touchscreen sits in the top-right corner, in
  the strip beside the picture a DS would show, so it never covers any of
  that picture. At startup WideMelon widens the world view to at least the
  window's shape and the screen's, so the top screen covers the window
  edge to edge (the viewport you chose is kept as a minimum). Clicks on the
  corner touchscreen touch the DS screen; swapping screens puts the
  touchscreen in the middle and the top screen in the corner, and with a
  phone connected only the top screen is shown. The touchscreen is smaller
  than in the side-by-side layout on screens narrower than 16:9.
- **Bigger touchscreen in Pokémon battles** (Overlay layout, on by default;
  WideMelon settings). During a battle in Pokémon Platinum the top screen
  slides so the DS picture starts at the left edge, and the touchscreen
  doubles in width and height in the space that frees up, still without
  covering the DS picture. Battles are detected from the game's battle
  engine (ARM9 overlay 16) being in memory: it loads while the screen is
  black between the encounter effect and the battle intro, and goes when
  the overworld code comes back during the fade out, so the layout slides
  over (in about a third of a second) while both screens are black.
  Setting: `WideMelon.BattleLayout`.
  - **Pokémon HeartGold and SoulSilver** get the same battle layout, plus
    layouts of their own for the screens that need the touchscreen, so the
    games play well on a widescreen monitor or TV:
    - The menu (X) and the PC: the touchscreen grows to 2.5 times its width
      and height in its corner, over the top screen, which stays where it
      is. The menu's bar over the bottom of the top screen is left out.
    - The PC's boxes and a Pokémon's summary (from the boxes or the party):
      the touchscreen becomes the big screen at the left, and the top
      screen's DS picture fills the space beside it, edge to edge.
    - The bag and the party screen: the touchscreen covers the top screen,
      all but the strip at the bottom with the item's description.
    The screens slide back when you leave. Each screen is detected from what
    the game has in memory (the code it loads for that screen, the app it
    runs, or the script of the PC); nothing in the game is changed.
- **Use either stick** (Config > Input and hotkeys, next to the joystick
  picker, on by default). A joystick binding to a stick direction also works
  with the same direction on the other analog stick, so on a Nintendo Switch
  Pro controller Axis 1 and Axis 3 are interchangeable, and so are Axis 2 and
  Axis 4: bind the D-pad to either stick and move with either one. Which axes
  belong to which stick comes from the controller's SDL mapping, so it works
  with controllers SDL recognizes (Switch Pro, Xbox, PlayStation, ...) and
  leaves other controllers alone. A stick direction that has a binding of its
  own isn't mirrored. Setting: `JoystickTwinSticks` (per instance).
- **Second save location** (Config > Path settings, off by default). Keeps a
  game's save in two places, for example WideMelon's folder and the game
  folder on an R4 card's SD card, so you can move between the emulator and a
  real console. When a game loads, the newer of the two saves is used and
  copied over the older one; every save is written to both. If the second
  location isn't there (the card isn't inserted), saves just go to the first.
  Saves that get replaced are kept in a Backups folder, newest first as
  `<name> Bak1.sav` up to `Bak5`. Settings: `WideMelon.SaveSync`,
  `WideMelon.SecondSaveFolder`.
- **Faster saving in the Pokémon games.** The games' save driver waits a few
  milliseconds after every flash page it writes, for a chip that is never
  busy in emulation, which added about five seconds to each save. Like
  nds-bootstrap, WideMelon finds that wait in the game's code and skips it.
- **Weather in the expanded view.** Rain (and other weather made of small
  sprites) now falls across the whole widescreen picture instead of stopping
  at the edges of the DS screen.
- **Single-file Windows build.** `widemelon.exe` can be built as one static
  executable of about 50 MB with MSYS2 UCRT64 and static Qt 6, with no DLLs
  or plugin folders next to it (see BUILD.md). It keeps its settings and
  saves in its own folder. Cameras use Qt's Windows Media Foundation backend
  instead of FFmpeg.
- **Local wireless over WiFi, with a real DS or another MultiMelon.** System >
  Multiplayer > *Wifi as Local Multiplayer* (called *Connect a real DS (WiFi
  bridge)* before). A DSi, or a 3DS running DS games in DSi
  mode through TWiLight Menu++, joins your WiFi with the modified
  nds-bootstrap (github.com/Dusknight4/nds-bootstrap, release v2.17.4) and
  plays local wireless with MultiMelon on the same network: the Union Room,
  trades and battles. Tested with Pokémon Platinum and HeartGold (US), either
  console starting the talk. MultiMelon listens on UDP port 7071 and finds the
  others by itself; allow it through the firewall on private networks.
  Setting: `MP.DSBridge`.
  - Up to four players (more where the game allows it, such as the
    Underground's eight), as on the DS: everyone on the network with the
    option on hears everyone, DSes and MultiMelons alike, and the game itself
    picks whom it plays with. So DS and DS, DS and MultiMelon, and MultiMelon
    and MultiMelon all play the same way, and nobody is locked out by having
    paired with the wrong player. As on the DS, the link uses the network
    only while the game has its wireless on (the Union Room...) and for 20 s
    after; it says hello once a second, by broadcast and to each player it
    knows, and forgets a player it hasn't heard from for 10 s.
  - A player who drops out for a few seconds (a weak WiFi signal, a busy PC)
    carries on where they left off, as on the DS: the link holds for as long
    as the game allows (10 s in the Pokémon games) instead of ending after
    2 s, and while one player lags, the others' link rounds don't wait for
    them.
  - WideMelon never waits for the DS, so it stays at full speed even on a
    slow or busy network; the link data just arrives a frame or two later.
    The game's wireless runs on the same WM HLE as on the DS (as in Online as
    Local) instead of the emulated wireless chip, which had to wait for the
    DS's reply in every round it hosted. Turn the option on before the game
    uses its wireless (it's remembered, so normally from the start); while it
    is on, the game's Nintendo WFC (online) features don't work.
    `WIDEMELON_DS_LINK=0` brings back the emulated-chip bridge.
- **Online as Local.** System > Multiplayer > *Online as Local...*:
  local wireless with a friend's WideMelon over the internet, joined by a room
  name and password.
- **HeartGold/SoulSilver: no blue screen after cancelling a link trade or
  battle.** These games keep at most 32 jobs for the next VBlank; returning to
  the Union Room map they can queue more than that at once, and with a link up
  that failed check is the blue "An error has occurred" screen (it showed up
  with the WiFi bridge). WideMelon gives that list room for 64 when the game
  boots (direct boot only; `WIDEMELON_HG_VBLANK_QUEUE=0` turns it off). Start
  the game fresh: a savestate made before keeps the old size.

### Fixed

- Thin black lines and dots between terrain tiles in the classic OpenGL
  renderer (see *Close gaps between polygons*).
- Only one renderer could be chosen in widescreen mode.
- Occasional pauses and jumps while walking or running in the Pokémon games
  (see *Reduce in-game slowdown*). They were the game's own lag frames, also
  present without motion smoothing.
- A wavy look while the camera scrolled: parts of the 3D scene moved
  unevenly against each other, left/right because positions left of the
  screen centre were rounded the other way, and up/down because vertical
  positions were rounded to whole pixels. Positions are now rounded the same
  way everywhere and kept to 1/32 of a pixel.
- In Pokémon battles, the 3D Pokémon sat a pixel to the left of the 2D
  sprite the game swaps it with, so it jumped sideways at the swap.
- Motion smoothing dropped to 30 fps for about half a second whenever the
  area-name sign slid in or out (walking between a town and a route in the
  Pokémon games). The sign moves every frame while the world moves every
  other frame, and the whole screen was taken to be 60 fps content. Small
  changes between a 30 fps game's images now update the newest image instead.
- *Reduce in-game slowdown* now runs the ARM9 at twice the DSi's clock
  (266 MHz). At the DSi clock, Platinum still dropped a frame setting up the
  next map when walking from a route back into a town.
  `WIDEMELON_ARM9_SHIFT=2` gives the DSi clock for comparison.
- A stall in the OpenGL 2D renderer: each screen pass changed a buffer the
  GPU could still be reading, so the driver waited for the GPU (over 1 ms a
  frame when the GPU was busy, and up to 160 ms in a frame where a game
  changes settings on every line). The passes now stream into a ring buffer.

- With the JIT recompiler on Windows, the ARM9's DTCM (where DS games keep
  their stack) was never mapped for direct access, though marked as mapped:
  every stack access the JIT generated faulted once and was turned into a
  slow call (about 45,000 a frame in the Pokémon overworld). It is now mapped.
- Further OpenGL renderer buffers (layer, sprite, compositor, final pass and
  capture settings, sprite and 3D vertices) are re-specified each time instead
  of updated in place, so the driver never waits for the GPU; the per-line
  settings use a persistently mapped ring buffer.

### Changed

- **Renamed to MultiMelon** for its cross-platform wireless multiplayer
  (the WiFi bridge to a real DS and Online as Local). The window title,
  About box, menus and the Windows and macOS programs
  (`multimelon.exe`, `MultiMelon.app`) carry the new name. Settings, saves
  and the settings folder are unchanged, so nothing has to be moved. Linux
  packages keep the `widemelon` name for now.
- Faster emulation: about twice the frame rate of 1.0.4-mod.3 when not
  limited to 60 fps (Pokémon Platinum overworld, JIT on, 4x scale, motion
  smoothing: about 170 fps before, 360 fps now). Results are identical to
  before: checked frame for frame on battles, walking and rain scenes.
- **OpenGL on its own thread** (main window). The emulator records its OpenGL
  calls into a queue that a render thread carries out, like Mesa's glthread:
  calls that only pass values return at once, data is copied into the queue,
  and only calls that return something wait. The emulation thread no longer
  spends time in the graphics driver, and presenting a frame overlaps the
  next one. Together with the changes below, about 2.5x the frame rate of
  1.0.4-mod.3 when not limited to 60 fps (Pokémon Platinum overworld: about
  170 fps before, 430 fps now), with identical pictures. Opening a second
  window turns it off for the session; `WIDEMELON_GLTHREAD=0` turns it off.
  The stubs are generated from glad.h by `tools/gen_glthread_stubs.py`.
- While both CPUs wait for an interrupt and nothing else is running, the
  emulator moves straight on to the next point where something can happen (an
  event or a timer overflow) instead of stepping there in short slices; that
  is 50-65% of all slices in the Pokémon games. `WIDEMELON_NOHALTSKIP=1` turns
  it off.
- The JIT recompiler goes from one compiled block to the next in generated
  code, without returning to the emulator loop between blocks (as Dolphin's
  and Citra's dispatchers do); it returns only for interrupts, halts, the end
  of the time slice or code that isn't compiled yet. About 80% fewer trips
  through the loop, with emulation results identical to before.
  `WIDEMELON_JIT_NOCHAIN=1` turns it off.

### In progress

- In the Overlay layout's battle view, filling the area beside the DS screen
  with the battle scenery (and continuing the info bar) is still being
  worked on. It is off, so that area stays black; set the environment
  variable `WIDEMELON_BATTLE_BACKDROP=1` to try the work in progress.
- `WIDEMELON_MP_*` environment variables run local multiplayer over a
  simulated network with delay and jitter, for testing (see
  `src/net/SimMP.h`). Without them, multiplayer is unchanged.

### Fixed during motion smoothing testing

These affected earlier test builds of motion smoothing only:

- Tall grass shimmered and produced heavy artifacts while walking, with
  smaller artifacts around the player and the following Pokémon. Motion is
  now estimated with a camera-motion vote that only trusts distinctive parts
  of the picture, plus temporal and neighbourhood predictions.
- Opening or closing menus, fading to or from black, and closing the party
  screen's submenu produced garbled in-between frames. Such changes are now
  detected on the GPU from the share of the picture that has no good match
  (20%, or 5% while the camera is still), and the real frames are shown.
- Walking looked less smooth on slower PCs because in-between frames were
  skipped whenever a frame ran past 75% of its time. That rule was removed;
  it couldn't prevent a missed refresh anyway.
- On 120 Hz displays the game dropped to around 48–50 fps with the JIT
  recompiler off. Each of the two presents per emulated frame waited for its
  own refresh, so the next frame's emulation was left with a single 8.3 ms
  refresh. WideMelon now presents twice per frame only while emulation keeps
  up, and otherwise once per frame with a doubled swap interval, holding a
  steady 60 fps.

### Notes

- Frame drops with the JIT recompiler off come from CPU emulation, not from
  the renderer or motion smoothing. Turning on Config > Emu settings >
  CPU emulation > Enable JIT recompiler makes emulation about four times
  faster and brings back the extra frames at 120 Hz.
