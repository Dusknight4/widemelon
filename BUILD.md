<!-- Copyright (C) 2026 WideMelon contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Building WideMelon

Clone `https://github.com/pruefsumme/widemelon.git` or extract the complete
application source archive from a release. No game files are needed to build
or run the automated tests. C++17, CMake, Ninja, and Git are required.

## Linux

On Ubuntu 24.04, on either x86_64 or ARM64:

```sh
sudo apt install build-essential cmake ninja-build git pkg-config nodejs \
  extra-cmake-modules libcurl4-gnutls-dev libpcap0.8-dev libsdl2-dev \
  libarchive-dev libzstd-dev libegl1-mesa-dev libgl1-mesa-dev libwayland-dev \
  qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libqt6svg6-dev \
  qt6-websockets-dev
./scripts/build.sh
./widemelon
```

The script builds pinned FAAD2 2.11.2 and ENet 1.3.18 from `.deps/`, then builds
the emulator and runs its tests. It accepts `WIDEMELON_BUILD_JOBS` (default 6).
Use `WIDEMELON_USE_QT6=0` with Qt 5.15 development libraries to build with Qt 5.
Linux release AppImages use Qt 5 on native Ubuntu 22.04 x86_64 and ARM64 runners
for broader compatibility.

A community-tested Fedora 44 ARM64 build used Clang and the matching system
development packages:

```sh
sudo dnf install cmake ninja-build git pkgconf clang llvm lld nodejs \
  libcurl-devel libpcap-devel SDL2-devel libarchive-devel libzstd-devel \
  faad2-devel enet-devel extra-cmake-modules mesa-libEGL-devel \
  mesa-libGL-devel wayland-devel qt6-qtbase-devel \
  qt6-qtbase-private-devel qt6-qtmultimedia-devel qt6-qtsvg-devel \
  qt6-qtwebsockets-devel
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_AR=/usr/bin/llvm-ar \
  -DCMAKE_RANLIB=/usr/bin/llvm-ranlib \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld"
cmake --build build
cmake -S tests -B build/tests -G Ninja -DUSE_QT6=ON
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

## Windows and macOS release builds

Native releases statically link dependencies built from the registry and
overlays pinned in `vcpkg.json`. Qt WebSockets, Concurrent, and JPEG support
are required for the phone bridge. The release workflow builds on Windows
x64, Intel macOS, and Apple Silicon macOS separately.

On Windows, install Visual Studio 2022 or newer with the C++ desktop workload,
Clang/LLVM, CMake, Ninja, Git, Python 3.11+, and Node.js 22. Run from a developer
shell with `clang.exe`, `clang++.exe`, and `llvm-rc.exe` on PATH. The workflow
uses Git Bash after activating Visual Studio. On macOS, install Xcode's command
line tools and `brew install cmake ninja autoconf automake autoconf-archive
libtool pkg-config python node`.

Prepare the pinned dependency manager (use Git Bash on Windows):

```sh
git clone https://github.com/microsoft/vcpkg.git .deps/vcpkg
git -C .deps/vcpkg checkout 9b965a116838c6cdcd36bca60d1b81b030c8ab8d
```

Select the preset and triplet for the host:

| System | Preset | Triplet |
| --- | --- | --- |
| Windows x64 | `release-windows-x86_64` | `x64-windows-static-release` |
| macOS Intel | `release-mac-x86_64` | `x64-osx-13-release` |
| macOS Apple Silicon | `release-mac-arm64` | `arm64-osx-13-release` |

For example, on Apple Silicon:

```sh
cmake --preset release-mac-arm64 -B build/native \
  -DVCPKG_ROOT="$PWD/.deps/vcpkg" -DUSE_RECOMMENDED_TRIPLETS=OFF \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-13-release \
  -DVCPKG_HOST_TRIPLET=arm64-osx-13-release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DENABLE_LTO_RELEASE=OFF
cmake --build build/native --parallel 3
```

Use the corresponding preset and triplet on other hosts; omit the macOS
deployment argument on Windows. The output is `build/native/widemelon.exe`
or `build/native/WideMelon.app`. The first dependency build can take an hour.
The selected Qt version requires Windows 10 1809+ or macOS 13+; see
[Qt's platform requirements](https://doc.qt.io/qt-6/supported-platforms.html).

Configure tests using the same static dependencies (replace `TRIPLET`):

```sh
cmake -S tests -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSE_QT6=ON -DVCPKG_MANIFEST_MODE=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/.deps/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_INSTALLED_DIR="$PWD/build/native/vcpkg_installed" \
  -DVCPKG_TARGET_TRIPLET=TRIPLET
cmake --build build/tests --parallel 3
ctest --test-dir build/tests --output-on-failure
```

On Windows also pass `-DCMAKE_CXX_COMPILER=clang++` and
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` when configuring tests. On macOS
pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0`. The bridge tests use Qt's offscreen
platform and generated frames; they do not require a ROM. The normal bridge test
uses loopback. To exercise the production listener on a real private adapter,
run it with `WIDEMELON_PHONE_TEST_ADDRESS` set to the host's active IPv4 address;
the same HTTP, WebSocket, subnet, and pairing checks then run over that address.

### Single-file Windows build with MSYS2

A standalone `widemelon.exe` that needs no DLLs can also be built with MSYS2's
UCRT64 toolchain and static Qt. In a UCRT64 shell, install
`mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-static,SDL2,libarchive,zstd,faad2,enet}`
and run:

```sh
cmake -S . -B build/static -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_STATIC=ON -DUSE_QT6=ON -DPORTABLE=ON \
  -DCMAKE_PREFIX_PATH=/ucrt64/qt6-static \
  -DPKG_CONFIG_ARGN=--static -DCMAKE_DISABLE_FIND_PACKAGE_harfbuzz=ON
cmake --build build/static
```

`PKG_CONFIG_ARGN` and disabling harfbuzz's CMake package make Qt link
HarfBuzz statically through pkg-config instead of through its DLL. This build
uses Qt's Windows Media Foundation backend for cameras instead of FFmpeg. The
portable executable keeps its settings and saves in its own folder.

## Phone screen and controller

The phone bridge is frontend-only and starts only after an explicit action. It
serves the native `256 × 192` bottom screen as JPEG at up to 30 FPS and accepts
touch and button input over the same TCP port. The desktop keeps its bottom
screen while the phone is disconnected or a capture fails.

The browser must authenticate with the session-only QR secret or manual pairing
code and must connect from the selected local subnet. Unauthenticated clients
receive neither layout nor screen frames and cannot submit controls. Credentials
are regenerated whenever the bridge starts or pairing is revoked.

The controller layout editor supports moving and resizing controls, undo and
redo, custom emulator-action buttons, D-pad or analog-stick input, and optional
status, FPS, and frame text.

The phone page polls the browser Gamepad API for a controller with a standard
mapping. It merges the controller's buttons and left-stick directions with the
existing phone input snapshot. While such a controller is present, virtual DS
buttons are hidden by default and can be shown with the page's **Show controls**
button. Stylus touch remains available on the bottom screen. Disconnecting the
controller or hiding the page releases held hardware buttons and restores the
virtual controls.

The wrench panel provides an interactive controller diagram. Selecting a DS
control arms capture; after all inputs return to neutral, the next standard
button or stick direction becomes its binding. Hotkey rows use the same capture
flow and can target any phone-supported melonDS action. The normal panel shows
each hotkey as one compact key/action button; tapping it opens its focused editor.
The default maps DS L/R
to trigger buttons 6/7, avoiding shoulder buttons 4/5 that some browsers use for
navigation. The left stick can also drive D-pad directions. A validated mapping
is kept in browser local storage; blocked storage falls back to the defaults.
Opening the panel temporarily releases gamepad input to avoid changing the game
while editing.

The saved object also records which DS bindings were explicitly customized.
Those controls and configured hotkeys render green after reload; untouched
defaults remain neutral grey. The configured phone base port is persistent, so
the browser origin and its mapping storage remain stable across normal restarts.

Some Android Chrome releases reserve standard buttons 4/5 (LB/RB) for switching
tabs at the browser level. These actions are not cancellable through the Gamepad
API. The mapper still supports the inputs for unaffected browsers and future
Chrome fixes; trigger buttons 6/7 remain the default DS L/R binding.


The bridge is intended for a trusted private LAN. Pairing prevents ordinary
devices from connecting, but the connection is not encrypted. Do not expose the
port to the internet or use it on public, guest, school, or workplace networks.
The bridge does not stream audio.

### Firewall and network troubleshooting

The webpage and controls share the displayed TCP port, which may need to be
allowed by the host firewall. The **Firewall setup guide…** stays open while
the bridge runs. On Linux it detects common firewall tools and provides
commands for firewalld, UFW, or general system guidance. Generated rules are
limited to the selected private IPv4 address, subnet, interface, and port.
WideMelon never runs administrator commands or changes firewall rules itself.

Automatic firewalld checks only detect installation because even read-only
queries can trigger PolicyKit. UFW checks use unprivileged status or boot
configuration and report an unknown result when access is unavailable.
Windows and macOS receive native system-settings guidance; the macOS check is
read-only and never changes firewall settings. If the phone already connects,
no new rule is needed. Revisit a generated rule if the network, address, or
port changes.

If the page does not open, check the following:

- Do not select a loopback address such as `127.0.0.1`; it is reachable only by
  the computer running WideMelon.
- Prefer the private LAN address on the physical Wi-Fi or Ethernet interface;
  VPNs, container bridges, and other virtual adapters may not be reachable from
  the phone.
- Put both devices on the same non-guest network and subnet. Wi-Fi client
  isolation can block them even when both devices have internet access.
- Check the advanced bridge log. `Served /` means the phone reached the HTTP
  server; a subnet rejection identifies a network mismatch.
- If no private address is available, join the same Wi-Fi network on both
  devices or create a hotspot. WideMelon does not change system network
  settings.

Browser screen wake-lock support normally requires HTTPS, so the phone's
  auto-lock setting may need to be adjusted during a session.

## Technical overview

A Nintendo DS screen is normally 256 × 192 pixels. WideMelon creates a wider 3D
target and adjusts the projection to reveal extra geometry on both sides:

```text
normal:     [       256 pixels       ]
widescreen: [ extra ][ 256 native ][ extra ]
```

The original view keeps its scale and center. Native 2D layers are placed over
the middle 256 pixels, so the interface and touchscreen are not widened.

The viewport width is fixed when the process starts. This keeps CPU geometry,
OpenGL buffers, shaders, and compositing on the same dimensions, which is why
profile changes require a restart.

The phone bridge crops the centered physical bottom layer from the OpenGL
output, downsamples it to native resolution, and uses a bounded asynchronous
readback and encoder pipeline. Up to eight frames may be in flight so streaming
does not wait for a network round trip after every frame. Beyond that limit,
only the latest unsent frame is kept. The browser also replaces pending frames
while decoding, and its acknowledgement retires any earlier skipped frames.
Timeouts never add send credit to a stalled stream.

Only one heartbeat probe waits for a reply at a time, so a delayed reply remains
valid. Remote buttons and touch are released after one second without an input
snapshot, independently of video acknowledgements and heartbeats. Three seconds
of silence or an unacknowledged frame closes the connection and restores the
desktop fallback. The phone reconnects automatically.

## Browser tests and diagnostics

An optional browser smoke test uses installed Chromium and Node.js 22 or newer.
It checks simultaneous button holds and continuous touch through the production
bridge while streaming a generated test pattern; no ROM is needed:

```sh
node tests/phone_browser_smoke.js build/tests/phone_bridge_test /usr/bin/chromium
```

Add `--gamepad` to exercise browser gamepad input, automatic virtual-control
hiding, the toggle, and controller disconnect with a simulated standard gamepad.
This checks the production WebSocket path but cannot prove that a particular
phone, browser, or Backbone model exposes the controller to the Gamepad API.

Add `--benchmark --dialog` to measure sustained streaming during idle,
continuous touch, and simultaneous button holds with the settings dialog open.
The test reports per-stage FPS and decode/delivery timing and fails below 28.5
displayed FPS. Enable it explicitly with
`-DWIDEMELON_ENABLE_STREAM_BENCHMARK=ON` so ordinary builds do not depend on
local browser DevTools configuration.
Combine these options with `--gamepad` to also benchmark a held gamepad button
and verify its release after disconnecting the simulated controller.

The ordinary test suite includes delayed frame acknowledgements (180 ms),
delayed heartbeat replies (900 ms), stalled decoding, bounded buffering,
recovery to the newest pending frame, and independent input release:

```sh
ctest --test-dir build/tests -R 'phone_delayed' --output-on-failure
```

For a browser-only A/B comparison, set `WIDEMELON_BENCH_REVISION` to a commit
hash; the test substitutes that revision's browser script while keeping the
same bridge. `WIDEMELON_BENCH_QUALITY=100` and `WIDEMELON_BENCH_CPU=8` select
JPEG quality and Chromium CPU throttling. Synthetic and loopback measurements
do not prove Wi-Fi performance or gameplay GPU capture performance.
Set `WIDEMELON_BENCH_ACK_DELAY_MS=180` and
`WIDEMELON_BENCH_PONG_DELAY_MS=900` to exercise delayed replies in the real
browser benchmark. These simulate response latency, not bandwidth loss; the
benchmark also reports and rejects unexpected connection closures.

The phone configurator also provides a generated test pattern, live bridge
logs, frame/encode/drop/RTT metrics, optional rotating file logs, synchronous
GPU readback for driver diagnosis, and a sanitized JSON diagnostics export.
The export retains up to 60 recent timing samples, including capture, encode,
send, acknowledgement, decode, input, GUI timer, and queued-socket metrics.
Samples also report frames in flight and heartbeat round-trip time. Disconnect
logs include the reason and close code at the default log level.
Export during or immediately after an FPS drop, before restarting the bridge.

These environment overrides change diagnostics only and never start the network
listener:

```sh
WIDEMELON_PHONE_LOG_LEVEL=debug WIDEMELON_PHONE_LOG_FILE=1 ./widemelon
```

## Release packaging

`.github/workflows/release.yml` runs by manual dispatch. It builds direct
Windows and macOS downloads plus Linux x86_64 and ARM64 AppImages and Debian
packages, runs tests, and packages exact dependency sources. A normal dispatch
uploads development artifacts only. A dispatch from `main` with
`publish_release` enabled builds all platforms once, creates `v1.0.4` on the
exact tested commit only after every package succeeds, publishes the GitHub
Release, and then updates AUR. The tag must not already exist, and
`WIDEMELON_VERSION` must match the intended release. `RELEASE_NOTES.md` is
published verbatim and contains only the version heading and changelog;
download instructions remain in `README.md`. `SHA256SUMS` covers every
published asset. Ordinary branch pushes run x86_64 and ARM64 tests with both
supported Qt versions.

After the GitHub release is published, the workflow renders and clean-builds
the `widemelon`, `widemelon-git`, and `widemelon-bin` recipes, then updates their
AUR repositories sequentially. Publication requires a dedicated Ed25519 private
key in the repository secret `AUR_SSH_PRIVATE_KEY`; its public key must be
registered on the maintainer's AUR account. The AUR host key is checked against
the fingerprint documented in [packaging/aur/README.md](packaging/aur/README.md),
and reruns skip repositories whose generated files are already current.

Run `./scripts/build.sh` before committing release changes. Once committed:

```sh
./scripts/package-source.sh 1.0.4
```

The native packaging command, after building and testing on that host, is:

```sh
python scripts/package-native.py 1.0.4 macos-arm64 arm64-osx-13-release
```

Use the matching platform/triplet for Windows or Intel macOS. Windows packaging
creates a directly downloadable `.exe`; it does not add an inner ZIP. macOS
packaging creates both `dist/WideMelon.app` for development artifacts and a DMG
for releases. Keep the vcpkg download cache: it supplies the exact dependency
source input. Native release CI disables separate binary caching and restores
installed libraries only together with their downloaded sources. The publish
job combines all platform source inputs with the application source into one
deduplicated `Complete-Source.tar.zst`. See `SOURCE.md` for rebuilding from it.

Developer ID signing and notarization are optional. To enable them, provide all
six GitHub Actions secrets: `MACOS_CERTIFICATE_P12` (base64-encoded Developer
ID Application certificate), `MACOS_CERTIFICATE_PASSWORD`,
`MACOS_SIGNING_IDENTITY`, `MACOS_NOTARY_KEY_ID`, `MACOS_NOTARY_ISSUER_ID`, and
`MACOS_NOTARY_PRIVATE_KEY` (the App Store Connect API key in `.p8` format).
Without those secrets, branch, manual, and tagged macOS runs publish a DMG
containing an ad-hoc-signed app that may require approval in Privacy & Security.
Windows executables remain unsigned and can trigger SmartScreen. Never advise users to
disable platform security checks globally. The application About dialog and
release page provide license and source access; platform packages retain the
applicable installed notices. Never add ROMs, BIOS/firmware dumps, saves, game
assets, or generated build directories to a release.

Automated tests cannot prove gameplay, GPU output, or behavior on every driver.
Before publishing, smoke-test the native setup dialog, ROM selection, 4:3 and
expanded OpenGL profiles, and phone pairing with a locally available game.

## Development

WideMelon is maintained as a GitHub fork of melonDS. The complete modified
emulator source is checked in under `src/`; there is no patch-generation step or
nested source checkout.

Run the complete build and test workflow before committing engine, shader, Qt,
dependency, or packaging changes:

```sh
./scripts/build.sh
```

Automated or headless runs can supply a profile through environment variables:

```sh
WIDEMELON_VIEW_WIDTH=448 WIDEMELON_SCALE=4 \
  ./build/widemelon /path/to/game.nds
```

The supported overrides and ranges are documented in the source configuration:

- `WIDEMELON_VIEW_WIDTH`: even values from `256` through `768`.
- `WIDEMELON_SCALE`: values from `1` through `8`.
- `WIDEMELON_WINDOW_WIDTH`: `640` through `7680`.
- `WIDEMELON_WINDOW_HEIGHT`: `480` through `4320`.
- `WIDEMELON_INTEGER`: boolean-like integer setting.

To compare against upstream melonDS, add
`https://github.com/melonDS-emu/melonDS.git` as an `upstream` remote and merge
selected commits on a dedicated update branch. Upstream updates are explicit;
run the full renderer, frontend, and packaging checks after merging them.
