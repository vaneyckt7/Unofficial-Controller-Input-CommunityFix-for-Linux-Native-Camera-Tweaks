# Controller input parity analysis

Reference revisions:

- Windows: `ersh1/BG3_NativeCameraTweaks` at `862222b6e68d863841724cb86b2492a9071255a9` (2.4.5)
- Linux: `0x1496FD0/Baldur-s-Gate-3-Linux-Native-Camera-Tweaks` at `18ee4b53b15e2b5304bc2df76fe70e18718b09c9` (1.0.21)

## Root causes

### Premature SDL queue termination

The original Linux interposer returned `0` from `SDL_PollEvent` whenever it
consumed a right-stick-Y or mouse-wheel event. Applications conventionally use
`while (SDL_PollEvent(&event))`; therefore BG3 was told that the entire queue was
empty even when more events were waiting behind the consumed event. Frequent
axis-motion events could be left for later frames and accumulate into perceived
input lag.

The patched interposer keeps polling internally after a consumed event. It only
returns `0` when the real SDL queue is empty, or returns the next event that BG3
must receive.

### Different horizontal and vertical response models

The original Linux controller pitch used:

```text
pitch delta per camera call = raw axis / 32767 * 2 degrees
deadzone                    = 4000 / 32767 = 12.21%
```

That model did not rescale the useful stick range and did not use frame time. At
full deflection it produced approximately 120 degrees/second at 60 FPS, 240 at
120 FPS, and 480 at 240 FPS. Horizontal rotation remained in BG3's native,
time-scaled camera pipeline, so the axes could not feel equal across frame rates.

Windows 2.4.5 first removes and rescales its configured 15% deadzone:

```text
u = sign(x) * (abs(x) - 0.15) / (1 - 0.15)
```

It then calculates controller pitch as:

```text
pitch delta = u * ControllerCameraRotationMult
                * deltaTime * camera.rotationSpeed * ControllerPitchMult
```

With the Windows defaults (`2.0` and `0.5`), this simplifies to:

```text
pitch delta = u * deltaTime * camera.rotationSpeed
```

The patch uses this same base formula. Earlier versions read `rotationSpeed`
from camera-object offset `0xC4`, matching the Windows 2.4.5 camera layout;
see "Area-independent pitch speed" below for why it now uses a fixed reference
value instead. Linux already uses the same layout offsets for zoom (`0x58`) and
current pitch (`0x164`). A monotonic per-camera timer supplies frame time and
clamps pauses to 100 ms to prevent a large jump after a stall.

In-game testing showed that this Windows-derived vertical base speed was still
several times faster than the native, unmodified Linux horizontal path. Patch
`input-fix.2` therefore adds `controller_pitch_sensitivity`, defaulting to
`0.25`. This multiplier is applied after the base formula and can be adjusted in
`~/.config/bg3-native-camera-tweaks.conf` without recompiling.

## Additional corrections

- Left-stick click (L3) + right-stick Y zoom is normalized and time-scaled. Its
  15 units/second default preserves the original full-stick speed at 60 FPS
  while removing FPS dependence. A normal L3 click is deferred until release;
  it is forwarded to BG3 if the click was not used as the zoom modifier.
- Controller movement no longer forces SDL relative-mouse mode.
- Multiple mouse-motion and wheel events are accumulated instead of overwriting
  all but the last event.
- Controller state is cleared on device removal or window focus loss.
- Binding access is guarded when the input config could not be loaded.

### Input Fix 3 state corrections

- The Windows camera layout identifies `currentZoomA`, `currentZoomB`, and
  `desiredZoom` at offsets `0x54`, `0x58`, and `0x5C`. Each custom zoom step
  synchronizes all three values, including recovery from an already-mismatched
  state.
- The magnitude of `rotationSpeed` is used for controller pitch. Stick direction
  and `invert_controller_pitch` are now the only sources of pitch direction, so
  a camera-mode sign change cannot invert the axis unexpectedly. (Superseded by
  the fixed reference speed below.)
- Mouse Y motion is collected only while the configured mouse-rotate binding is
  active. A held mouse binding takes priority over stale controller-axis state.
- SDL relative-mouse mode is restored only if this mod enabled it. Pre-existing
  BG3 ownership is preserved, preventing changed mouse behavior after switching
  from controller to mouse and keyboard.

### Input Fix 4 portable release corrections

- Per-user paths no longer assume a particular profile name. The mod checks an
  explicit `BG3_INPUT_CONFIG_PATH`, then `XDG_DATA_HOME`, then the standard
  `$HOME/.local/share` BG3 profile tree.
- A missing input config uses middle mouse as the camera-rotate fallback instead
  of aborting initialization.
- Executable mapping failure is checked before hashing.
- Compatibility symbol selection and executable mapping lower the binary's
  required glibc version from 2.34 to 2.17.

### Input Fix 5 live-state corrections

- Controller events are transition notifications. If a centering or L3-release
  event is lost during a focus/input-mode transition, an event-only cache can
  retain the previous right-stick value indefinitely. The camera hook now
  reconciles the cached values with SDL's live controller axis and button state.
- A zero mouse-wheel delta previously synchronized all three zoom fields on
  every camera call. That could cancel BG3's own in-progress interpolation even
  when the mod had no zoom input. Zoom fields are now changed only for a real
  custom zoom step.

### Community integration corrections

- Runtime tracing isolated the persistent micro-jitter to the disabled native
  `movss xmm3, 0x58(r15)` zoom-state write. With that write NOPed, the values at
  `0x54`, `0x58`, and `0x5C` diverged after zooming. Restoring the exact native
  instruction kept `0x54` and `0x58` equal throughout the test and eliminated
  the reproducible jitter. This also restores BG3's native near/far limits.
- Stick up now zooms in by default. `invert_controller_zoom=true` provides the
  opposite direction without changing the binding.
- The original author's requirement that newly assigned mouse-rotate bindings
  work is retained without the version-specific `SaveToInputConfigFile` hook.
  The active `inputconfig_p1.json` is checked every 500 ms and reloaded
  transactionally: an incomplete save keeps the last known-good bindings, and
  atomic file replacement is detected by device/inode as well as timestamp and
  size.
- Known executable hashes are reported but no longer form a hard allowlist.
  Newer builds may initialize only if each pattern is unique, the target call is
  a valid relative call, and the pitch-store opcode and `0x164` displacement
  match exactly.

### Controller Input Fix release

- The experimental SDL tactical handoff was discarded because passing raw
  right-stick events back to BG3 broke the L3 modifier behavior.
- Controller pitch, L3 zoom, zoom-state synchronization, and device switching
  therefore remain identical to the tested Input Fix 5 implementation.
- Mouse vertical pitch is no longer hard-coded to `2.0`; the new
  `mouse_pitch_sensitivity` setting defaults to `1.5`.

### Right stick in the map, books and other UI

Every version up to 1.0.22 hid all right-stick-Y events from BG3, so that the
game would not zoom the world camera while the stick drives pitch. That also hid
the stick from UI: it could no longer zoom the world map or scroll books and
scrolls.

The right stick's vertical axis now reaches BG3 again. Instead, the mod hooks
BG3's world-camera input handler, the same function the Windows mod hooks as
`HandleCameraInput`. In the world, BG3 turns the stick into `ZoomIn` (`0x68`)
and `ZoomOut` (`0x69`) input events. While the stick is outside the mod's 15%
deadzone, the hook sets the event's value (`+0x18`) to 0 for the call, restores
it afterwards, and returns the handler's result unchanged. BG3 treats any value
at or below its 0.65 threshold as no input and sets the camera's zoom delta
(`0xA4`) to 0, as the Windows mod does. Like the Windows mod, this relies on
the map and book UI handling the stick through their own input path rather than
this camera handler.

On Linux the handler is a virtual method with no direct callers. The mod finds
it by a unique `.text` signature, checks the event-load instructions at
`+0x40d`, requires exactly one pointer to it in `.data.rel.ro`, and swaps that
vtable slot. The slot is in the RELRO segment; its page is made writable only
for the swap and then returned to its original protection. If any step fails,
the mod warns and keeps hiding right-stick-Y from BG3 as before, so the world
camera still works and only UI use of the stick is lost.

The mod must also stop pitching the world camera while UI uses the stick. BG3
answers every deflected right-stick-Y event in the world with a zoom event to
this handler, within a frame; in a Steam Deck log, 131 stick events produced
132 zoom events. While the map or a book is open the handler receives none,
even with the stick moving continuously. So when a deflected stick event has
waited more than 100 ms without a zoom event reaching the handler, the mod
treats the stick as the UI's and stops controller pitch and L3 zoom. The next
zoom event hands the stick back to the camera. Opening UI while the stick is
already moving can still pitch the camera for up to those 100 ms. The debug
log reports the longest wait seen (`zoom_wait_max_ms`) and each change of owner.

Mouse-wheel events are still hidden from BG3 and applied by the mod.

Known limitations: the hook blocks zoom events by action, not by device, so a
keyboard zoom key does nothing while the right stick is deflected (for example,
a stick drifting past the 15% deadzone). A stick flicked and re-centred within
one frame can let a single zoom event through before the mod's stick state
catches up.

### Area-independent pitch speed

Some areas lower the camera's `rotationSpeed` at `0xC4`. In the Blighted
Village it drops from 47.444 to 33.339 over a few seconds, so pitch that
follows it slows by 30% (11.9 to 8.3 degrees/second at the default
sensitivity of 0.25). The Windows mod behaves the same way. Controller pitch now uses a
fixed reference speed of 47.444, BG3's normal value, so the same stick
deflection gives the same pitch speed everywhere. Horizontal rotation stays in
BG3's own pipeline and still follows the area's speed.

The default `controller_pitch_sensitivity` stays at 0.25, which now gives
11.9 degrees/second at full stick in every area, the speed it previously gave
only in areas with the normal rotation speed. Players who want a faster tilt
can raise it; 0.75 (about 35.6 degrees/second) felt good on a Steam Deck.

### Camera wobble after tilting

After tilting, the camera could wobble up and down by one to two degrees
indefinitely, even on open ground and with the stick at rest. Each camera
update, BG3 steps the pitch at `0x164` toward its own zoom-based target by at
most rate x frame time, stores it, and builds the camera orientation from the
stepped value still in `xmm0`. The mod NOPed only the store, so `0x164` kept the
mod's pitch but the view was built from pitch + step, and the step changes with
every frame time. At BG3's default pitch the target equals the pitch and the
step is below BG3's 0.01 degree threshold, which is why the wobble only
appeared after tilting or while a zoom was changing the target.

The debug log confirmed it on the Steam Deck at about 25 camera updates per
second. With the mod writing nothing, `0x164` stayed at 8.68 degrees while the
view pitch derived from the camera direction at `0x74` swung between 9.85 and
10.25 degrees. It was always offset toward BG3's target, by 1.2 to 2.0 degrees.

The patch now turns the store into a load of the same register
(`movss [rbp+0x164], xmm0`, `F3 0F 11`, becomes `movss xmm0, [rbp+0x164]`,
`F3 0F 10`), so BG3 builds the view from exactly the mod's pitch. The patch
is refused unless the next instruction converts the same register, since
only then does the load steer the view.

Known limitation: BG3 skips rebuilding the orientation while the pitch is
within 0.01 degrees of its target. If a tilt stops inside that window, the view
keeps the previous frame's pitch, at most one frame of tilt away (about 1
degree at full stick and 25 updates per second). It stays still and corrects on
the next tilt or zoom.

## Verification

`tests/controller_input_test.c` verifies deadzone endpoints and equal movement
over one second at 60 and 120 FPS. `tests/vtable_hook_test.c` verifies the
unique-slot search and swapping a pointer on a real read-only page.
`tests/camera_input_hook_test.c` builds `src/main.c` against a stand-in camera
input handler with the same signature bytes, reached through a vtable in
read-only `.data.rel.ro`. It installs the real hook, checks which events are
blocked and restored, checks that right-stick-Y reaches BG3 only when hooked,
checks that pitch speed ignores the camera's rotation speed, and checks that
controller pitch stops once stick events go unanswered by the camera handler and
resumes on the next zoom event. It also runs a stand-in for BG3's pitch store
and checks that, once patched, it loads the mod's pitch instead of storing
BG3's. `tests/stick_owner_test.c` covers the ownership
timing on its own. Full validation still requires the supported
native BG3 build because the camera function and object are runtime hooks.

Recommended in-game A/B checks:

1. Hold the right stick at a fixed diagonal and compare the apparent horizontal
   and vertical speed.
2. Repeat at 60, 120, and the display's uncapped/high-refresh frame rate.
3. Flick Y repeatedly while also pressing buttons; button delivery must remain
   immediate instead of being delayed behind axis events.
4. Test left-stick-click + right-stick-Y zoom, a normal L3 click without zoom,
   and controller disconnect/reconnect.
5. Right stick up/down in the world must pitch without zooming. The same stick
   must zoom the world map and scroll books, and the world camera behind the
   map or book must not tilt.
6. Compare pitch speed in the Blighted Village with another area.
7. Tilt low, zoom in and out, then leave the stick alone: the camera must hold
   perfectly still.
