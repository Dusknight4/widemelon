# Patches to Azahar for MultiMelon

Apply to Azahar's unified source (release 2126.2) before building its core libraries
(`patch -p1 < 0001-uds-link.patch` from the source's top folder).

- `0001-uds-link.patch`: lets a frontend carry the UDS service's 802.11 frames itself, in place of
  Azahar's rooms (`core/hle/service/nwm/uds_link.h`). MultiMelon's "Wifi as Local Multiplayer" uses it
  for 3DS games (`src/frontend/qt_sdl/UdsLink.*`, `ThreeDSNet.*`).
