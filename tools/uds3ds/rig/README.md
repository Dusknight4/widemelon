# rig3ds: scripted MultiMelon 3DS consoles

MultiMelon itself is a windowed program with no scripting interface. `rig3ds.exe` is how its 3DS side is driven by
scripts: a console with no visible window, built from the same code as MultiMelon's "3DS wifi test 3":

- Azahar 2126.2's core, with the same OpenGL renderer MultiMelon uses;
- MultiMelon's "Wifi as Local Multiplayer" link (`UdsLink` / `ThreeDSNet`, patch `0001-uds-link`);
- the same log filter, so a test build's `svcOutputDebugString` lines (`MPACT`, `MPSYNC`, ...) are logged.

Two to four of them linked on 127.0.0.1 are consoles playing local wireless together. You drive them through command
files and see them through PNG screenshots. Nothing takes the keyboard or focus, so it can run while someone uses the
PC.

**Speed:** they run at a 3DS's full speed (60 frames a second) on the graphics card. A PC without OpenGL 4.3 falls
back to Azahar's software renderer, at about 2.5 frames a second.

## What's needed
- Windows 10 or 11, 64-bit. `rig3ds.exe` is a single static exe: no installer, no DLLs beyond Windows' own.
- A graphics card or chip with OpenGL 4.3: any from the last ten years. Tested on Intel Arc (laptop) graphics.
- Windows PowerShell 5.1 (built into Windows) for the scripts.
- Disk space: the installed game (~2 GB for PSMD) once, on an NTFS drive (the other consoles get it through hard
  links), plus a little per console.
- The game as a decrypted `.cia`, and a save folder as Azahar keeps it.

## Setting up
1. Unzip the kit into any folder, for example `C:\MultiMelonRig\kit`. Keep its files together: `rig3ds.exe`,
   `rig_pair.ps1`, `rig_cmd.ps1`, `psmd_town_demo.ps1` and this README.
2. Files that came from another PC are marked as downloaded. Unblock them once, in PowerShell in that folder:
   ```powershell
   Get-ChildItem | Unblock-File
   ```
3. Windows blocks PowerShell scripts by default. Either allow them for the current PowerShell window:
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass
   ```
   or start each script as `powershell -ExecutionPolicy Bypass -File .\rig_pair.ps1 ...`.
4. The consoles' own files go under `-Root`, by default `%LOCALAPPDATA%\MultiMelonRig`, kept out of OneDrive
   because of the game's size. Nothing is written into the kit folder.

## Quick start: the worked example
```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\psmd_town_demo.ps1 -Cia <path>\delta_psmd_v1-alpha2.cia -Save <path>\save_test_postgame_RF2_Lucario_Lv60
```
It brings up two linked consoles and goes through Main Menu > Multiplayer > Local Wireless. A hosts and B joins, both
Check link and Start game, and B walks in town. It prints a screenshot's path at each step and takes about
90 seconds once the game is installed. `-Consoles 3` (or 4) adds joiners. The consoles are left running in town for
you to carry on; `.\rig_pair.ps1 -Down` stops them. Read the script: it's the simplest template for your own tests.

## rig_pair.ps1: start and stop the consoles
```powershell
.\rig_pair.ps1 -Up -Cia <file.cia> -Save <save folder> [-Consoles 2|3|4] [-SaveB <folder>] [-SaveC ...] [-SaveD ...]
               [-Speed 100] [-Software] [-Root <folder>] [-Minutes 120] [-Reinstall]
.\rig_pair.ps1 -Status
.\rig_pair.ps1 -Down
```
**What `-Up` does:**
- Installs the CIA into console A: the first time, and again whenever `-Cia` is another file or a newer build.
  `-Reinstall` forces it. The install takes a minute or two.
- Gives the other consoles the same game through hard links, so there's no second ~2 GB copy.
- Puts a **fresh copy** of the save into each console, every time, so each run starts from the same save. `-Save` is
  used for every console unless `-SaveB`, `-SaveC` or `-SaveD` gives that one another. Your save folders are only
  read.
- Starts them hidden. A uses UDP port 7701, B 7702, C 7703, D 7704, on 127.0.0.1 only: nothing goes on the network,
  and Windows doesn't ask about the firewall.
- `-Speed` is the percent of a 3DS's speed (default 100). `-Speed 0` removes the limit (about 450 frames a second on
  a light scene here). For multiplayer tests keep 100: consoles at different speeds don't keep pace with each other
  the way 3DSes do.
- `-Software` uses the software renderer even with OpenGL (slow; only for comparison).
- `-Minutes` is how long they run before quitting by themselves (default 120).

`-Status` prints each console's last progress line. `-Down` quits them all.

**Where things go**, under `-Root`:

| Path | What |
|---|---|
| `A\`, `B\`, `C\`, `D\` | the consoles' own folders (Azahar user folders: `sdmc\`, `nand\`, `log\`, `mac.txt`) |
| `A\log\azahar_log.txt` | console A's log: Azahar's lines plus the game's debug output (`Debug.Emulated`) |
| `A\sdmc\Nintendo 3DS\0...0\0...0\title\00040000\0f174600\data\00000001\` | console A's working copy of the PSMD save |
| `shots\A\NAME.png` | screenshots: top screen above, bottom screen centred below (400x480) |
| `A.out` | the renderer, then progress every 5 s: frames run and frames a second, consoles heard on the link, link frames sent and received |
| `A.err` | errors, if any |
| `A.cmd` | the command file `rig_cmd.ps1` appends to |

The save: use the folder of save files as Azahar keeps them (for PSMD: `dungeon`, `game_data`, `game_header`,
`game_system`). If the folder has no `00000001.metadata`, the script writes PSMD's format info. Another game needs
its own `.metadata`: copy it into the save folder.

## rig_cmd.ps1: input and screenshots
```powershell
.\rig_cmd.ps1 -I A|B|C|D [-Keys "..."] [-Touch "X Y FRAMES"] [-Shot NAME] [-Quit] [-Root ...] [-TimeoutSec 300]
```
- **`-Keys`**, a keys script: `step,step,...`, where a step is `keys*frames`.
  - Key names: `a b x y l r start select`, the D-pad `up down left right`, and the Circle Pad
    `cup cdown cleft cright`.
  - **Holding a key:** `a*3` holds A for 3 frames; `right*45` holds right for 45 frames (0.75 s). Hold a button
    3 frames for a press the game sees.
  - **Several at once:** join them with `+`, as in `a+up*5`.
  - **Waiting:** `*frames` alone presses nothing. `*60` is a second.
  - Example: `down*3,*20,a*3,*240` taps down, waits a third of a second, taps A, then waits 4 seconds.
  - Not mapped: ZL/ZR, the C-Stick, HOME.
- **`-Touch "X Y FRAMES"`** touches the bottom screen at X 0–319, Y 0–239 for that many frames.
- **`-Shot NAME`** takes a screenshot *after* the keys/touch before it have run. The script waits for the PNG and
  prints its path.
- **`-Quit`** stops that console.

Commands run in order on the console's own frames. They can be queued while earlier ones still run: a console picks
up appended lines four times a second. To have several consoles act side by side, send each one's commands without
`-Shot` first and put the `-Shot` on the last one, as the demo does.

**The command file itself**, for writing it from your own tools: `<Root>\A.cmd`, one command per line, ASCII.

| Line | Does |
|---|---|
| `down*3,*20,a*3,*240` | a keys script, as above |
| `touch 160 120 6` | touch the bottom screen at (160, 120) for 6 frames |
| `shot NAME` | a PNG `NAME.png` in the console's shots folder, logged in `A.out` as `shot NAME` |
| `quit` | stop |
| `# ...` | a comment |

Only ever append to it: rewriting it while a console runs makes it skip lines. A line counts once it ends with a
newline.

**Timing:** frames are the 3DS's frames, 60 a second at full speed. Loading screens and the game's own waits take
real time, so a wait needs about as long as a person would wait. If a screenshot shows a step was missed, add frames
to the wait before it. The consoles don't start at the same instant, so a test that needs two consoles to act
together should check both with screenshots before the next step.

## The link
- **How they find each other:** while the game's local wireless is in use, each console sends a hello once a second
  to the others' ports (`link: N nearby` in `A.out` counts the consoles heard). Frames then go straight to the
  console their destination MAC names, or to all of them. It's the same link as two MultiMelons on one PC.
- **Lost consoles:** a console that goes quiet for 15 s (killed, crashed) is dropped from the others' network. A host
  frees its node; a joiner whose host has gone gets ConnectionLost (4). A killed console that's restarted comes back
  as a new session and is dropped at once, so it can join again.
- **Measured, 2 consoles at full speed in town:** over 35 s the joiner received exactly what the host sent (5,125
  frames; 606 the other way). Nothing is lost on 127.0.0.1.

## A tested path through PSMD (v1-alpha2, the test save, 2026-11-15, full speed)
This is what `psmd_town_demo.ps1` does, step by step:

```powershell
.\rig_pair.ps1 -Up -Cia ..\delta_psmd_v1-alpha2.cia -Save ..\handoff_v1_alpha\save_test_postgame_RF2_Lucario_Lv60
# boot to the title screen (~25 s), Start, then Main Menu > gear icon (Multiplayer) > Local Wireless
#   (the book icon left of the gear is Delete Save: never press A there)
.\rig_cmd.ps1 -I B -Keys "*1500,start*3,*420,down*3,*20,right*3,*20,a*3,*240,a*3,*360" | Out-Null
.\rig_cmd.ps1 -I A -Keys "*1500,start*3,*420,down*3,*20,right*3,*20,a*3,*240,a*3,*360" -Shot host_or_join
# A hosts (the second item), B joins: "Joined the host. You are player 2"
.\rig_cmd.ps1 -I A -Keys "down*3,*20,a*3,*120" | Out-Null
.\rig_cmd.ps1 -I B -Keys "*60,a*3,*600" -Shot joined
# Check link on both: host "Players: 2 (sent 3617, received 509)", joiner "sent 509, received 3120"
.\rig_cmd.ps1 -I B -Keys "down*3,*20,a*3,*300" | Out-Null
.\rig_cmd.ps1 -I A -Keys "down*3,*20,a*3,*300" -Shot check_link
# Start game: the host, then the joiner picks it too; both arrive in town together
.\rig_cmd.ps1 -I A -Keys "a*3,*900" | Out-Null
.\rig_cmd.ps1 -I B -Keys "*60,a*3,*1200" -Shot b_town
.\rig_cmd.ps1 -I A -Keys "*1" -Shot a_town
# the joiner walks right; the host sees it move
.\rig_cmd.ps1 -I B -Keys "right*45,*90" -Shot b_walked
.\rig_cmd.ps1 -I A -Keys "*1" -Shot a_sees_walk
.\rig_pair.ps1 -Down
```
**With three consoles** (`-Consoles 3`, C joining a second after B): C was "player 3", the host's Check link said
"Players: 3", all three started the game, and the host saw both joiners walk. Not checked: whether the joiners see
each other.

**Not tried in the rig yet:** dungeons, battles and four players.

## Using rig3ds.exe directly
```
rig3ds.exe USERDIR TITLE SECONDS [KEYS]
```
- **USERDIR** is the console's folder.
- **TITLE** is a `.cia` (installed first, unless `RIG_NO_INSTALL=1`), `.3ds`, `.cxi` or `.3dsx`.
- **SECONDS** is how long it runs.
- **KEYS** is a keys script to start with.

Environment variables:

| Variable | Meaning |
|---|---|
| `RIG_RENDERER` | `opengl` (default; falls back to software without OpenGL 4.3) or `software` |
| `RIG_SPEED` | percent of a 3DS's speed (default 100; 0 = no limit) |
| `RIG_LINK_PORT`, `RIG_LINK_PORTS` | the link's UDP port (default 7072), and how many ports from there it may take (default 1; MultiMelon uses 4) |
| `RIG_LINK_PEERS` | `ip:port,...`: hellos go there instead of by broadcast |
| `RIG_LINK_LOOPBACK=1` | bind to 127.0.0.1 only |
| `RIG_NO_LINK=1` | no link: the game's local wireless goes nowhere |
| `RIG_NO_INSTALL=1` | a `.cia` TITLE is already installed: just start it |
| `RIG_CMD` | the command file (see above) |
| `RIG_SHOTS`, `RIG_SHOT_EVERY` | where shots go (default USERDIR\shots); a shot every N seconds by itself |
| `RIG_LOG_FILTER` | Azahar's log filter (default `*:Info Debug.Emulated:Debug`) |
| `RIG_PLUGIN` | a `.3gx` loaded by Azahar's Luma plugin loader (`sdmc:/luma/plugins/default.3gx`) |
| `RIG_FAST_EXIT=1` | skip the slow tidy-up at exit |

To restart one console by hand while the others run (a rejoin test), give it the same variables `rig_pair.ps1` sets
(see the end of that script). A rig talking to MultiMelon on another PC should work run directly without
`RIG_LINK_LOOPBACK`, with `RIG_LINK_PORT=7072` and `RIG_LINK_PORTS=4`, once Windows' firewall allows it. That
hasn't been tried.

## Limits
- **Input:** no ZL/ZR, C-Stick or HOME. No audio.
- **Screenshots** are at the 3DS's own resolution (400x240 top, 320x240 bottom).
- **The first time** a console meets a new effect it compiles a shader, which can stutter for a moment. Shaders are
  kept in the console's folder, so later runs are smoother.

## Troubleshooting
| You see | Do |
|---|---|
| "running scripts is disabled on this system" | `Set-ExecutionPolicy -Scope Process Bypass` in that window, or start the script with `powershell -ExecutionPolicy Bypass -File ...` |
| `renderer: no OpenGL 4.3 here` in `A.out` | the graphics driver lacks OpenGL 4.3 (a remote-desktop session can cause it too): update the driver. It runs in software meanwhile |
| "consoles are running" | `.\rig_pair.ps1 -Down` first |
| "no shot after 300 s" | the console has stopped or is very slow: `.\rig_pair.ps1 -Status`, then `A.err` and `A\log\azahar_log.txt` |
| a screenshot shows the screen before a step | the step was missed: add frames to the wait before it |
| "install failed" | see `<Root>\A_install.log`; the CIA must be decrypted |
