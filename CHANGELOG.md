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
- **Use either stick** (Config > Input and hotkeys, next to the joystick
  picker, on by default). A joystick binding to a stick direction also works
  with the same direction on the other analog stick, so on a Nintendo Switch
  Pro controller Axis 1 and Axis 3 are interchangeable, and so are Axis 2 and
  Axis 4: bind the D-pad to either stick and move with either one. Which axes
  belong to which stick comes from the controller's SDL mapping, so it works
  with controllers SDL recognizes (Switch Pro, Xbox, PlayStation, ...) and
  leaves other controllers alone. A stick direction that has a binding of its
  own isn't mirrored. Setting: `JoystickTwinSticks` (per instance).
- **Single-file Windows build.** `widemelon.exe` can be built as one static
  executable of about 50 MB with MSYS2 UCRT64 and static Qt 6, with no DLLs
  or plugin folders next to it (see BUILD.md). It keeps its settings and
  saves in its own folder. Cameras use Qt's Windows Media Foundation backend
  instead of FFmpeg.

### Fixed

- Thin black lines and dots between terrain tiles in the classic OpenGL
  renderer (see *Close gaps between polygons*).
- Only one renderer could be chosen in widescreen mode.
- Occasional pauses and jumps while walking or running in the Pokémon games
  (see *Reduce in-game slowdown*). They were the game's own lag frames, also
  present without motion smoothing.

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
