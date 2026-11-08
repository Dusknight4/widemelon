# 3DS local wireless (UDS) tools

- `udstest/`: a 3DS homebrew self-test (devkitARM + libctru: `make` in devkitPro's shell). It scans for its
  network, joins or hosts, exchanges packets for 20 s and writes `sdmc:/udstest.txt` ending in RESULT PASS/FAIL.
  It runs the same in the emulator and on a real 3DS.
- `rig/rig3ds.cpp`: runs a 3DS title headless (Azahar's core, software renderer, no window) with MultiMelon's
  "Wifi as Local Multiplayer" link. `build.sh` builds it against Azahar's core build (paths inside).
  Two of them on loopback:

      RIG_LINK_PORT=7701 RIG_LINK_PEERS=127.0.0.1:7702 RIG_LINK_LOOPBACK=1 rig3ds A udstest.3dsx 40 &
      sleep 4; RIG_LINK_PORT=7702 RIG_LINK_PEERS=127.0.0.1:7701 RIG_LINK_LOOPBACK=1 rig3ds B udstest.3dsx 36

  then `A/sdmc/udstest.txt` and `B/sdmc/udstest.txt`.
