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
90 seconds once the game is installed. `-Consoles 3` (or 4) adds joiners; `-Loss`, `-Burst`, `-Delay` and `-Jitter` run it on a rough link. The consoles are left running in town for
you to carry on; `.\rig_pair.ps1 -Down` stops them. Read the script: it's the simplest template for your own tests.

## rig_pair.ps1: start and stop the consoles
```powershell
.\rig_pair.ps1 -Up -Cia <file.cia> -Save <save folder> [-Consoles 2|3|4] [-SaveB <folder>] [-SaveC ...] [-SaveD ...]
               [-Speed 100] [-Software] [-Root <folder>] [-Minutes 120] [-Reinstall]
               [-Loss 5] [-Burst 3] [-Delay 30] [-Jitter 30] [-Dup 0] [-Reorder] [-Seed 0]
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
- `-Loss`, `-Burst`, `-Delay`, `-Jitter`, `-Dup`, `-Reorder`, `-Seed` make the link rough (see "A rough link").

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
.\rig_cmd.ps1 -I A|B|C|D [-Link "..."] [-Keys "..."] [-Touch "X Y FRAMES"] [-Shot NAME] [-Quit] [-Root ...] [-TimeoutSec 300]
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
- **`-Link "..."`** changes that console's rough link from that point (see "A rough link").

Commands run in order on the console's own frames. They can be queued while earlier ones still run: a console picks
up appended lines four times a second. To have several consoles act side by side, send each one's commands without
`-Shot` first and put the `-Shot` on the last one, as the demo does.

**The command file itself**, for writing it from your own tools: `<Root>\A.cmd`, one command per line, ASCII.

| Line | Does |
|---|---|
| `down*3,*20,a*3,*240` | a keys script, as above |
| `touch 160 120 6` | touch the bottom screen at (160, 120) for 6 frames |
| `link loss=5 delay=30` | change this console's rough link (see "A rough link"); `link cut=1` takes it out of range |
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
  frames; 606 the other way). Nothing is lost on 127.0.0.1, which is why the rig can make the link rough on
  purpose (next section).

## A rough link: loss, lag, outages
Real 3DS wireless loses frames (often several in a row), delivers them late, and drops out when a console goes out
of range or to sleep. A clean link hides the bugs that only show then: on the DS side of this project it hid three
"was this message delivered?" bugs at once. Each console can make what it sends rough:

| Setting | `rig_pair.ps1` (all consoles, from the start) | `link` command (one console, any time) |
|---|---|---|
| lose this percent of datagrams | `-Loss 5` | `loss=5` |
| ...in bursts of this many on average (1 = each on its own) | `-Burst 3` | `burst=3` |
| deliver each frame this late | `-Delay 30` (ms) | `delay=30` |
| ...plus up to this much more, at random | `-Jitter 30` (ms) | `jitter=30` |
| let frames overtake each other | `-Reorder` | `reorder=1` |
| send this percent of frames twice | `-Dup 1` | `dup=1` |
| repeatable dice | `-Seed 7` | `seed=7` |
| **out of range**: nothing sent, nothing heard | | `cut=1`, back with `cut=0` |
| all off | (the default) | `off` |

```powershell
.\psmd_town_demo.ps1 -Cia ... -Save ... -Loss 5 -Burst 3 -Delay 30 -Jitter 30     # the demo on a rough link
.\rig_cmd.ps1 -I B -Link "loss=10 burst=4 delay=60 jitter=40"                     # rougher, from now on
.\rig_cmd.ps1 -I B -Link "cut=1" -Keys "*1200"                                    # B out of range for 20 s...
.\rig_cmd.ps1 -I B -Link "cut=0" -Keys "*600" -Shot b_back                        # ...and back
```
- `link` changes only the settings it names; the others keep their value. The change takes effect in order with
  the console's other commands (after the keys before it), and `A.out` logs it.
- Loss, delay and the rest apply to what that console *sends*, so set them on every console for both directions
  (`rig_pair.ps1` does). `cut` is both ways.
- With a rough link, `A.out`'s progress lines add "dropped N, doubled M".
- **Outages:** a console that hears nothing for 15 s drops the other. A host frees that player's node; a joiner
  whose host has gone gets ConnectionLost (4). `cut=1` for more than 15 s tests exactly that. `loss=100` instead
  silences one direction only: the others drop that console, but it keeps hearing them.
- **In order** (the default) means a frame never overtakes an earlier one. So when frames are frequent their delay
  sits near delay + jitter rather than spread evenly. `-Reorder` spreads them evenly and lets them overtake.

**Measured** (`linkcheck.exe`, numbered frames bounced off a second link endpoint, about 8,000 frames each):
10% loss gave 10.4% lost (runs of 1.12 frames); 10% in bursts of 4 gave 10.5% (runs of 3.87); delay 40 +
jitter 20 gave round trips of 40-62 ms; 5% doubled gave 5.0%; `cut` dropped everything, hellos included.

**PSMD on a rough link (v1-alpha2, 2 consoles, 2026-11-15):**
- 5% loss in bursts of 3, 30-60 ms lag: the whole demo worked (Host/Join, Check link, Start game, walking in town).
- **B out of range for 21 s (`cut=1`):** after 15 s each console dropped the other. On both screens the partner's
  Lucario just disappeared; there was no message, and both carried on alone in town. After `cut=0`, nothing
  reconnected: B's game had stopped sending (connection lost) and stayed alone. Whether that's what PSMD should
  do is the game's call.
- **B silent one way for 20 s (`loss=100`):** the host dropped B and its Lucario disappeared. When B spoke again,
  it reappeared on the host and play went on. **That recovery is probably an emulator leniency:** Azahar's host
  accepts data from a console whose node it has freed (`nwm_uds.cpp`, `HandleSecureDataPacket` doesn't check the
  sender). A real 3DS host most likely ignores a console that's no longer connected to it, so don't rely on that
  recovery.

## Azahar rooms instead of the link
MultiMelon also has Azahar's multiplayer rooms for playing over the internet: System > Multiplayer > *Create Room
(3DS games)* and *Direct Connect to Room*. One player runs a room (UDP port 24872 by default), and the others
connect to it. It's Azahar's own room code and protocol, so Azahar players can join the same room. The rig does
it too:

```powershell
.\rig_pair.ps1 -Up -Room -Cia ... -Save ...                      # A opens a room on 127.0.0.1:24872, all join it
.\rig_pair.ps1 -Up -Room -RoomPassword secret1 -Consoles 3 ...   # with a password, three players
.\psmd_town_demo.ps1 -Room -Cia ... -Save ...                    # the worked example through a room
```
- The consoles join as "Console A", "Console B", .... Joins, departures and errors show in `A.out` as `room: ...`
  lines, and the progress lines say `room: joined, N members`.
- In a room the game's local wireless goes through it, not the link, so the rough-link options don't apply.
- **Tested (2026-11-23, v1-alpha2):**
  - 2 consoles: Host/Join, Check link, Start game, and walking in town.
  - 3 consoles in a password room: "Players: 3".
  - A wrong password is refused ("Wrong password for this room."), and so is a nickname already in use.
- **By hand:** `RIG_ROOM_HOST=24872` opens a room (on `RIG_ROOM_BIND`, default 127.0.0.1) and joins it;
  `RIG_ROOM_JOIN=address:port` joins one; `RIG_ROOM_NICK` (4 to 20 letters, digits, spaces, `.`, `_`, `-`) and
  `RIG_ROOM_PASSWORD` set your nickname and the password. To join a real MultiMelon or Azahar room elsewhere, use its
  address (that hasn't been tried).

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
| `RIG_RENDERER` | `opengl` (default; falls back to software without OpenGL 4.3), `software`, or `vulkan` (offscreen, as MultiMelon on macOS; only in a rig built from an Azahar build with Vulkan: `AZ_BUILD=... build.sh`) |
| `RIG_SPEED` | percent of a 3DS's speed (default 100; 0 = no limit) |
| `RIG_LINK_PORT`, `RIG_LINK_PORTS` | the link's UDP port (default 7072), and how many ports from there it may take (default 1; MultiMelon uses 4) |
| `RIG_LINK_PEERS` | `ip:port,...`: hellos go there instead of by broadcast |
| `RIG_LINK_LOOPBACK=1` | bind to 127.0.0.1 only |
| `RIG_LINK_LOSS`, `RIG_LINK_BURST`, `RIG_LINK_DELAY_MS`, `RIG_LINK_JITTER_MS`, `RIG_LINK_DUP`, `RIG_LINK_REORDER=1`, `RIG_LINK_SEED` | a rough link from the start (see "A rough link") |
| `RIG_ROOM_HOST`, `RIG_ROOM_JOIN`, `RIG_ROOM_BIND`, `RIG_ROOM_NICK`, `RIG_ROOM_PASSWORD` | an Azahar room instead of the link (see "Azahar rooms instead of the link") |
| `RIG_ROOM_DSPING=1` | (test) in a room, a DS-game message through it every second, and those received counted (`ds out N in M` in the progress lines): DS games share the room with 3DS games |
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
