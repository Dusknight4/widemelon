# Patches to Azahar for MultiMelon

Apply to Azahar's unified source (release 2126.2) before building its core libraries
(`patch -p1 < 0001-uds-link.patch` from the source's top folder).

- `0001-uds-link.patch`: lets a frontend carry the UDS service's 802.11 frames itself, in place of
  Azahar's rooms (`core/hle/service/nwm/uds_link.h`). MultiMelon's "Wifi as Local Multiplayer" uses it
  for 3DS games (`src/frontend/qt_sdl/UdsLink.*`, `ThreeDSNet.*`). `DropUdsPeer` drops a console that has gone
  from the link (quiet, or restarted): a host frees its node, a client loses its gone host. Azahar on its own
  keeps a dropped client's slot for good, so a restarted console could never rejoin.
  It also makes joining safe when several consoles join at once, or on a lossy link (2026-11-21):
  - **Same node for two joiners:** the host broadcasts each joiner's EAPoL-Logoff, and a console still joining
    took any logoff as its own, so two consoles joining together both got node 2 (and the second never got a node
    of its own). The host now names the joiner in the logoff (`client_mac_address`, which Azahar left empty), and a
    joining console takes only its own; an empty field (Azahar, older MultiMelons) is taken as before.
  - **Joins retry:** a joining console asks again (Authentication SEQ1) every 250 ms of emulated time, up to 4 s,
    while no answer comes, as a 3DS's radio resends a lost frame. The host answers a repeat: a console whose join
    never finished starts over, and one already in gets the same node's logoff again.
  - Log lines (`Service.NWM`, Info): the node the host gives each MAC, the node a joiner got, an ignored logoff for
    another joiner, and each retry.
- `0002-rig-3dsx-program-id.patch` (tests only): with `AZAHAR_3DSX_PROGRAM_ID=<hex>` set, a .3dsx runs under that
  title id. An application id (0004000x...) makes the plugin loader load a Luma plugin into homebrew, so rig3ds
  can run udstest.3dsx with the MultiMelon Wifi plugin serving its UDS. Without the variable nothing changes.
