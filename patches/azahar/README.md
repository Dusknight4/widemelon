# Patches to Azahar for MultiMelon

Apply to Azahar's unified source (release 2126.2) before building its core libraries
(`patch -p1 < 0001-uds-link.patch` from the source's top folder).

- `0001-uds-link.patch`: lets a frontend carry the UDS service's 802.11 frames itself, in place of
  Azahar's rooms (`core/hle/service/nwm/uds_link.h`). MultiMelon's "Wifi as Local Multiplayer" uses it
  for 3DS games (`src/frontend/qt_sdl/UdsLink.*`, `ThreeDSNet.*`). `DropUdsPeer` drops a console that has gone
  from the link (quiet, or restarted): a host frees its node, a client loses its gone host. Azahar on its own
  keeps a dropped client's slot for good, so a restarted console could never rejoin.
- `0002-rig-3dsx-program-id.patch` (tests only): with `AZAHAR_3DSX_PROGRAM_ID=<hex>` set, a .3dsx runs under that
  title id. An application id (0004000x...) makes the plugin loader load a Luma plugin into homebrew, so rig3ds
  can run udstest.3dsx with the MultiMelon Wifi plugin serving its UDS. Without the variable nothing changes.
