# Linux Native Camera Tweaks (Community Fixes)

A native Linux camera mod for **Baldur’s Gate 3**, originally created by **Biiinks78 / 0x1496FD0** and expanded through community contributions.

![Version](https://img.shields.io/badge/version-1.0.22-blue)
![Platform](https://img.shields.io/badge/platform-Linux-green)
![Architecture](https://img.shields.io/badge/architecture-x86--64-lightgrey)

## Features

* Extended vertical camera rotation with mouse and controller.
* Frame-rate-independent and configurable controller pitch, at the same speed in every area.
* The right stick still zooms the world map and scrolls books and scrolls,
  without tilting the camera behind them.
* L3 + right-stick vertical movement for smooth zoom control.
* Normal L3 clicks remain available when L3 is not used for zooming.
* Stable switching between controller and mouse/keyboard.
* Live controller-state reconciliation prevents stuck or inverted input.
* Automatic mouse-rotation binding reload without restarting the game.
* Portable profile and configuration paths.
* Safe pattern and instruction validation for compatible future game builds.

The update also fixes controller input delay caused by prematurely terminating SDL event polling and resolves persistent camera jitter after zooming.

Technical details are available in [`CONTROLLER_INPUT_ANALYSIS.md`](CONTROLLER_INPUT_ANALYSIS.md).

## Game compatibility

Known BG3 builds:

* `4.1.1.7209685`
* `4.1.1.7398727`

On other builds, the mod initializes only when every required pattern is unique and the expected camera instructions can be validated. If validation fails, the camera modifications remain inactive.

## Installation

1. Download `linux_native_camera_tweaks.so`.

2. Place it in a permanent location, for example:

   ```text
   ~/Mods/BG3/linux_native_camera_tweaks.so
   ```

3. Add its absolute path to BG3’s Steam launch options:

   ```text
   LD_PRELOAD="~/Mods/BG3/linux_native_camera_tweaks.so" %command%
   ```

4. Launch the native Linux version of BG3.

No installer or administrator access is required.

## Configuration

On first launch, the mod creates:

```text
~/.config/bg3-native-camera-tweaks.conf
```

Default settings:

```ini
controller_pitch_sensitivity=0.25
controller_zoom_speed=15.00
mouse_pitch_sensitivity=1.50
invert_controller_pitch=false
invert_controller_zoom=false
```

* `controller_pitch_sensitivity`: vertical controller camera speed (0.25 is
  about 12 degrees per second at full stick). An existing file keeps its value,
  so edit it there to change the speed.
* `controller_zoom_speed`: L3 + right-stick zoom speed
* `mouse_pitch_sensitivity`: vertical mouse camera speed
* `invert_controller_pitch`: reverses vertical controller rotation
* `invert_controller_zoom`: reverses controller zoom direction

With the default configuration, pushing the stick upward zooms in.

Configuration-file changes require a game restart. Changes to BG3’s mouse-rotation binding are detected automatically while the game is running.

## Current limitations

### Native zoom limits

BG3’s native minimum and maximum zoom limits remain enabled.

Earlier versions disabled an internal zoom-state write to provide a much larger zoom range. Runtime diagnostics showed that this caused the internal zoom values to diverge, producing persistent micro-jitter after zooming. Restoring the native update eliminated the jitter but also restored the normal zoom limits.

A larger stable zoom range may be explored separately in a future experimental update.

### Tactical Camera

Dedicated controller-compatible Tactical Camera behavior is not included in this release. Extended top-down camera angles remain available, but tactical outlines may not persist after switching from mouse/keyboard to controller.

This feature is intentionally deferred to avoid adding another unverified game-action hook to the stable release.

## Building and testing

Requirements:

* CMake 3.16 or newer
* SDL2 development headers
* A C99-compatible compiler

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The resulting shared object requires GLIBC 2.17 or newer and has no direct SDL runtime dependency.

Without a Linux machine, `./docker-build.sh` builds and tests the same x86-64 shared object in a container, including on Apple Silicon Macs. The result is written to `build-docker/linux_native_camera_tweaks.so`.

Runtime camera-hook validation still requires the native Linux version of BG3.

## Diagnostics

Add `debug_log=true` to `~/.config/bg3-native-camera-tweaks.conf`, or `LNCT_DEBUG=1` to the launch options, to write a diagnostic log:

```text
LNCT_DEBUG=1 LD_PRELOAD="~/Mods/BG3/linux_native_camera_tweaks.so" %command%
```

`LNCT_DEBUG=0` disables the log even when the config file enables it. The log is written to `/tmp/lnct-debug.log` (override with `LNCT_DEBUG_LOG=/path/to/file`). Twice a second it records, per camera object, the frame time, the camera's rotation speed, the pitch change applied by the mod, and any pitch change made by the game itself. It also records right-stick-Y events, hidden mouse-wheel events, the camera zoom events that reached BG3's camera handler and how many the mod blocked, and how long ago the world camera last updated.

## Credits

* **Biiinks78 / 0x1496FD0** — original Linux mod and camera hooks
* **JustLetMeInFSS** — controller input latency fix and some zoom, portability, integration work
* **Joegoldin** — SDL event-queue, safety and binding-fallback contributions
* **ersh1** — Windows implementation used as a controller-response reference

## Permissions

Bug fixes and feature improvements are allowed with appropriate credit to the original creator.

The original restrictions remain in effect:

* Do not upload the mod to other sites.
* Do not convert it for other games.
* Do not sell it or use it in paid mods.
* Do not earn donation points from it.

[Nexus Mods page](https://www.nexusmods.com/baldursgate3/mods/23896)
