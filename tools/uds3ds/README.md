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
- `ctr_code.py FILE OUT`: a decrypted title's code (ExeFS .code, unpacked), read in place from a .cia/.3ds/.cxi.
- `mk3gx.py ELF SETTINGS OUT`: packs a Luma3DS plugin (3GX v2) as 3gxtool v1.3 does, without its yaml-cpp/dynalo
  dependencies.
- `plugin/`: the MultiMelon Wifi plugin (CTRPluginFramework 0.8.0). Build CTRPF from source first: its source release,
  plus libcwav (github.com/PabloMK7/libcwav, with libncsnd) in Library/libcwav; build those, then the library with
  `make DEFINES=...` giving the version and `-Wno-error=unused-but-set-variable` (newer devkitARM GCC). Then `make`
  here (devkitPro's bash). In Azahar (rig3ds: RIG_PLUGIN=...3gx) it loads into any application as default.3gx.
