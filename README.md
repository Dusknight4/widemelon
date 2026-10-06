<p align="center">
  <img src="assets/widemelon.png" alt="WideMelon logo" width="220">
</p>

<h1 align="center">WideMelon</h1>

<p align="center">
 DS games in widescreen — with an optional phone touchscreen and controller. Enjoy DS games like never before.
</p>

<p align="center">
  <a href="https://github.com/Dusknight4/widemelon/releases/latest"><img src="https://img.shields.io/github/v/release/Dusknight4/widemelon?display_name=tag" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/platform-Windows-5b7fa3" alt="Windows">
  <a href="https://github.com/pruefsumme/widemelon"><img src="https://img.shields.io/badge/based%20on-WideMelon%201.0.4-8a6fb3" alt="Based on WideMelon 1.0.4"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg" alt="GPL-3.0-or-later"></a>
</p>

> [!NOTE]
> This is Dusknight4's fork of [WideMelon](https://github.com/pruefsumme/widemelon).
> It adds motion smoothing, closes the black seams between terrain tiles,
> removes in-game slowdown, and ships as a single Windows executable. See
> [CHANGELOG.md](CHANGELOG.md) for everything that changed.

WideMelon is a DS emulator built from melonDS that gives supported
games a genuinely wider 3D view. It reveals more of the game world at the
sides while keeping menus, sprites, videos, and the touchscreen at their
original proportions.

<p align="center">
  <img src="assets/widemelon-preview.png" alt="WideMelon" width="1220">
</p>

For a more console-like setup, WideMelon can also send the bottom screen to
your phone and use it as a touch controller. No separate mobile app is needed:
scan the QR code and play from your phone's browser.

## Highlights

- Motion smoothing: in-between frames make 30 fps games such as the Pokémon
  overworlds play like 60 fps, and add extra frames on 120 Hz displays.
- Overlay layout: the top screen fills the whole window and the touchscreen
  sits in the top-right corner, beside the picture a DS would show.
- No more black lines between terrain tiles in the classic OpenGL renderer.
- Reduce in-game slowdown: no more pauses and jumps while games load data,
  for example when walking through towns in the Pokémon games.
- Either analog stick moves you: on a Switch Pro, Xbox or PlayStation
  controller, a binding to one stick also works with the other.
- One self-contained Windows `.exe`: nothing to install, no DLLs.
- True widescreen 3D views from native 4:3 through 32:9.
- Unstretched 2D interfaces, menus, sprites, videos, and touchscreen content.
- Optional phone bottom screen, touch input, and customizable DS controls.
- Render scales from 1× to 8×, fullscreen, and integer scaling.
- Recent-ROM home screen, save states, drag and drop, and familiar melonDS tools.
- Separate configuration and saves from a standard melonDS installation.

## Download

Download the latest build from the
[Releases page](https://github.com/Dusknight4/widemelon/releases/latest).

| Platform | Download |
| --- | --- |
| Windows | Download the x64 `.exe` and open it. It's a single file with nothing to install; WideMelon keeps its settings and saves in the folder you put it in. |
| macOS and Linux | This fork doesn't publish builds for them. Build it from source (see [BUILD.md](BUILD.md)), or use [upstream WideMelon](https://github.com/pruefsumme/widemelon#download), which doesn't include this fork's changes. |

The `.exe` isn't code-signed, so Windows SmartScreen may warn the first time
you open it: choose **More info > Run anyway**. Each release lists SHA-256
checksums of its files in `SHA256SUMS`.

## Quick start

1. Open WideMelon. Use the display button on the home screen when you want to
   change the viewport, window resolution, or render scale for the next launch.
2. Open a legally obtained `.nds` ROM with the folder button or **File > Open ROM**.
3. The next time you start WideMelon, double-click the game in **Recent ROMs**.
4. For phone play, start the phone server on the home screen and scan its QR code.

You can also drag a ROM directly onto the WideMelon window. WideMelon does not
include games, ROMs, commercial BIOS or firmware files, or saves.

## Phone screen and controller

WideMelon can move the physical bottom screen and DS controls to a phone while
the wide top screen stays on your computer.

1. Select **Start phone server** on the home screen and scan the QR code that appears.
2. If you need to choose another private Wi-Fi or Ethernet address, select the
   phone button or open **Config > Phone screen & controller…**.
3. Start the connection from that dialog, then scan the QR code on the home screen
   or in the dialog.
4. Use **Edit controller layout…** to move, resize, or customize the controls.

If a gamepad is connected to the phone, its standard face buttons, shoulders,
Start/Select, D-pad, and left stick control the DS. The on-screen DS buttons
hide automatically when the browser recognizes the gamepad. Tap **Show controls**
to bring them back, or **Hide controls** to clear the screen again. The bottom
screen remains touchable in either mode. If the controller does not appear,
press one of its buttons while the phone page is open; some browsers reveal
gamepads only after an input gesture.

Tap the small wrench on the phone page, select a DS control in the controller
diagram, then press the physical controller input to bind it. Extra buttons can
run melonDS actions such as fast forward, pause, fullscreen or screen swap. Add
as many hotkey rows as needed. L/R use the controller triggers by default, since
some mobile browsers reserve the shoulder buttons for page navigation. Mapping
is saved locally in the phone browser across WideMelon sessions; **Restore**
resets it. Newly customized controls turn green while unchanged defaults remain
grey.

Some Android Chrome versions incorrectly use LB/RB for switching browser tabs.
The page cannot cancel that browser-level action. Until the Chromium regression
is fixed, use LT/RT, remap the controller in Android's Game Controller settings,
or use a browser where the shoulder buttons are not reserved.

The bridge is off by default and requires session-only pairing. Keep the phone
and computer on the same trusted, non-guest network. If the phone disconnects,
the bottom screen automatically returns to the desktop window. The phone bridge
does not stream audio.

See the [phone setup and troubleshooting guide](BUILD.md#phone-screen-and-controller)
for firewall help, diagnostics, and network details.

## How widescreen works

A Nintendo DS screen is normally 256 × 192 pixels. WideMelon expands the 3D
render target and adjusts the projection so compatible games reveal additional
geometry on both sides. The original view remains centered and keeps the same
scale; native 2D layers are composited over the middle without stretching.

Results depend on how each game draws its scene. Some games expose a great deal
of additional world detail, while others may cull objects outside the original
view. Menus, battles, videos, and special effects can remain 4:3 by design. The
expanded profiles require an OpenGL renderer; native 4:3 remains available as
the compatibility profile.

The classic OpenGL renderer closes the thin black lines and dots that can
appear between terrain tiles, for example in the Pokémon games. Turn off
**Close gaps between polygons** in **Config > Video settings** to compare with
upstream melonDS. The same dialog also offers **OpenGL (Compute shader)**, which
reproduces the DS rasterizer on the GPU and needs OpenGL 4.3.

## Motion smoothing

**Config > Video settings > Motion smoothing** generates in-between frames,
like the motion smoothing on TVs. WideMelon estimates how each part of the
picture moved between two real frames and draws the frames in between along
that motion; menus and text that stay still stay sharp. Repeating textures
such as tall grass follow the camera's motion instead of jumping to a
look-alike position one pattern repeat away.

When the picture changes rather than moves (a fade, a new scene, a menu, text
box or submenu opening or closing), WideMelon shows the real frames there
instead of in-between frames. It detects this on the GPU from the share of the
picture that has no good match. While the camera is still, a smaller share
is enough, so a submenu popping up is caught while a walking character or an
animated icon is still smoothed.

- 30 fps games, such as the Pokémon overworlds, play like 60 fps on a 60 Hz
  display.
- On a 120 Hz or faster display, every game gets extra frames when emulation is
  fast enough (for example with the JIT recompiler). Otherwise WideMelon shows
  one frame per emulated frame, held for two refreshes, so the game keeps full
  speed. Set the refresh rate in your operating system's display settings.
- The next real frame has to exist before the frames leading up to it can be
  drawn, so this adds a short delay: about one frame for 30 fps games at 60 Hz
  and half a frame for 60 fps games at 120 Hz.
- It needs the OpenGL display and pauses during fast-forward and slow motion.
  The phone screen keeps receiving the real frames.
- With VSync on, each emulated frame has to fit in one refresh (16.7 ms at
  60 Hz). If you see stutter, enable **Config > Emu settings > CPU emulation >
  Enable JIT recompiler**; the interpreter is several times slower.

## Reduce in-game slowdown

Some games occasionally take longer than usual to draw a frame, for example
the Pokémon games while loading map data as you walk through a town. The
picture then pauses briefly and jumps, even though the emulator itself keeps
running at full speed. **Config > Emu settings > Reduce in-game slowdown** (on
by default) runs the DS's main CPU at twice its speed, as a DSi can, and reads
the game card faster, so these frames arrive on time. This is not
hardware-accurate; turn it off if a game misbehaves. Savestates work across
both settings.

> [!CAUTION]
> **Compatibility note:** Widescreen support is game-dependent. It may not work correctly or provide much benefit in every game, because results depend on how that game renders its 3D scene.

## Build from source

WideMelon is a C++17 and CMake project. The complete source, pinned dependencies,
platform prerequisites, build commands, test workflow, and packaging notes are
documented in [BUILD.md](BUILD.md).

On a supported Linux development system, the normal workflow is:

```sh
./scripts/build.sh
./widemelon
```

## Project and credits

WideMelon is based on [melonDS](https://github.com/melonDS-emu/melonDS) and is
maintained as an independent project. Thanks to the melonDS contributors and
everyone testing WideMelon, reporting compatibility results, and improving the
experience.

## License

WideMelon is free software released under the
[GNU General Public License v3.0 or later](LICENSE). Third-party components and
their licenses are documented in [THIRD_PARTY.md](THIRD_PARTY.md).
