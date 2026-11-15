# rig3ds: headless MultiMelon 3DS consoles for scripted checks

`rig3ds.exe` runs a 3DS game with no window, using the same code as MultiMelon:

- Azahar 2126.2's core;
- MultiMelon's "Wifi as Local Multiplayer" link (`UdsLink` / `ThreeDSNet`, patch `0001-uds-link`);
- the same log filter, so a test build's `svcOutputDebugString` lines appear (`Debug.Emulated`).

Two of them linked on 127.0.0.1 are two consoles playing local wireless together. You drive them through
command files and see them through PNG screenshots. Nothing takes the keyboard or focus, so it can run while
someone plays.

It's a single 64-bit Windows exe (static, Windows' own DLLs only). Source: MultiMelon branch `three-ds`,
`tools/uds3ds/rig/rig3ds.cpp`.

## Quick start (two linked PSMD consoles)
From PowerShell, in this folder:

```powershell
.\rig_pair.ps1 -Up -Cia <path>\delta_psmd_mp33.cia -Save <path>\save_test_postgame_RF2_Lucario_Lv60
# both boot: about a minute to the title screen
.\rig_cmd.ps1 -I A -Keys "start*3,*25,*30,*40" -Shot a_menu      # prints the PNG's path when it's taken
.\rig_cmd.ps1 -I B -Keys "start*3,*25,*30,*40" -Shot b_menu
.\rig_pair.ps1 -Status
.\rig_pair.ps1 -Down
```

**What `-Up` does:**
- Installs the CIA into console A: the first time, and again whenever `-Cia` is another file or a newer build.
  `-Reinstall` forces a reinstall.
- Gives console B the same game through hard links, so there's no second ~2 GB copy.
- Puts a **fresh copy** of the save into both. Your save folder is only read.
- Starts both, hidden: A on port 7701, B on port 7702.

**Where things go:** everything lives under `-Root`. The default is `%LOCALAPPDATA%\MultiMelonRig`, kept out of
OneDrive because of the game's size.

| Path under -Root | What |
|---|---|
| `A\`, `B\` | the consoles' own folders (Azahar user folders) |
| `A\log\azahar_log.txt` | console A's log: Azahar's lines plus the game's debug output (`MPACT`, `MPSYNC`, ...) |
| `shots\A\NAME.png` | screenshots: top screen above, bottom screen centred below (400x480) |
| `A.out` | progress every 5 s: frames run, consoles heard on the link, frames sent/received |
| `A.cmd`, `B.cmd` | the command files `rig_cmd.ps1` appends to |

**The save:** use the folder of save files as Azahar keeps them (for PSMD: `dungeon`, `game_data`,
`game_header`, `game_system`). If the folder has no `00000001.metadata`, the script writes PSMD's format info
(0x40000 bytes, 8 folders, 8 files). Another game needs its own `.metadata`: copy it into the save folder.

## rig_cmd.ps1
```powershell
.\rig_cmd.ps1 -I A|B [-Keys "..."] [-Touch "X Y FRAMES"] [-Shot NAME] [-Quit] [-Root ...]
```
- **`-Keys`**, a keys script: `keys*frames,keys*frames,...`.
  - Keys: `a b x y l r start select`, the D-pad `up down left right`, and the Circle Pad
    `cup cdown cleft cright`.
  - Join several with `+` (`a+up*5`). `*frames` alone presses nothing (a wait).
  - Not mapped: ZL/ZR, the C-Stick, HOME.
- **`-Touch "X Y FRAMES"`** touches the bottom screen (X 0–319, Y 0–239) for that many frames.
- **`-Shot NAME`** takes a screenshot *after* the keys/touch before it have run. The script waits for the PNG and
  prints its path.
- **`-Quit`** stops that console.

Commands run in order, one per line, on the console's own frames. They can be queued while earlier ones still
run: the console picks up appended lines four times a second. Only append to `A.cmd` / `B.cmd`; rewriting them
while a console runs makes it skip lines.

**Timing:** the consoles use Azahar's software renderer, so they run at roughly **2.5 game frames a second**
once a 3D scene is up (full speed only on the boot logos). `*25` is therefore about 10 s. Hold a button
**3 frames** so the game sees it, then wait before the next one. Keep that in mind for anything timing-sensitive:
the rig checks behaviour, not speed.

## A tested path through PSMD (v1-alpha2, the test save, 2026-11-15)
```powershell
.\rig_pair.ps1 -Up -Cia ..\..\delta_psmd_v1-alpha2.cia -Save ..\..\handoff_v1_alpha\save_test_postgame_RF2_Lucario_Lv60
Start-Sleep 45
# both: title -> Main Menu
.\rig_cmd.ps1 -I B -Keys "start*3,*25,*30,*40" | Out-Null
.\rig_cmd.ps1 -I A -Keys "start*3,*25,*30,*40,*40" -Shot menu
# both: gear icon (Multiplayer) -> Local Wireless -> "Host a game or join one?"
#   (the book icon left of the gear is Delete Save: never press A there)
.\rig_cmd.ps1 -I B -Keys "down*3,*8,right*3,*8,a*3,*45,a*3,*60" | Out-Null
.\rig_cmd.ps1 -I A -Keys "down*3,*8,right*3,*8,a*3,*45,a*3,*60" -Shot joinhost
# A hosts, B joins
.\rig_cmd.ps1 -I A -Keys "*10,down*3,*6,a*3,*40" | Out-Null
.\rig_cmd.ps1 -I B -Keys "*10,a*3,*120" -Shot joined
# both pick Start game (the joiner must too), then town
.\rig_cmd.ps1 -I A -Keys "a*3,*170" | Out-Null
.\rig_cmd.ps1 -I B -Keys "*30,a*3,*230" -Shot b_town
.\rig_cmd.ps1 -I A -Keys "*1" -Shot a_town
# the joiner walks right; the host should see it move
.\rig_cmd.ps1 -I B -Keys "right*12,*30" -Shot b_walked
.\rig_cmd.ps1 -I A -Keys "*1" -Shot a_sees_walk
.\rig_pair.ps1 -Down
```
Frame counts that worked here may need a little more on a slower PC: take a shot, look, and add waits.

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
| `RIG_LINK_PORT`, `RIG_LINK_PORTS` | the link's UDP port (default 7072), and how many ports from there it may take (default 1; MultiMelon uses 4) |
| `RIG_LINK_PEERS` | `ip:port,...`: hellos go there instead of by broadcast |
| `RIG_LINK_LOOPBACK=1` | bind to 127.0.0.1 only. Use it on one PC: nothing on the network, and no Windows firewall question |
| `RIG_NO_LINK=1` | no link: the game's local wireless goes nowhere |
| `RIG_NO_INSTALL=1` | a `.cia` TITLE is already installed: just start it |
| `RIG_CMD` | the command file: lines of `keys`, `shot NAME`, `touch X Y FRAMES`, `quit`, picked up as they're appended |
| `RIG_SHOTS`, `RIG_SHOT_EVERY` | where shots go (default USERDIR\shots); a shot every N seconds by itself |
| `RIG_LOG_FILTER` | Azahar's log filter (default `*:Info Debug.Emulated:Debug`) |
| `RIG_PLUGIN` | a `.3gx` loaded by Azahar's Luma plugin loader (`sdmc:/luma/plugins/default.3gx`) |
| `RIG_FAST_EXIT=1` | skip the slow tidy-up at exit |

## Limits
- **Speed:** ~2.5 fps with the software renderer. It's for checking what happens, not how fast.
- **Input:** no ZL/ZR, C-Stick or HOME. No audio.
- **Link:** loopback only, as `rig_pair.ps1` sets it. For a rig talking to MultiMelon on another PC, run
  `rig3ds.exe` directly without `RIG_LINK_LOOPBACK`, and allow it through the firewall.
- **Rejoin tests:** a console that vanishes (killed) is dropped from the other's network after 15 s, or as soon
  as it comes back (a new session). Restarting one with `-Down`/`-Up` while the other runs isn't supported by
  `rig_pair.ps1`: stop and start a single `rig3ds.exe` by hand for that.
