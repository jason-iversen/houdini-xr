# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

An XR viewport for SideFX Houdini, targeting Meta Quest 2 and later over PCVR
(Link / Air Link). Houdini's own Scene Viewer is not involved: this renders the
LOP stage with **Hydra** (Storm by default) and presents it to an **OpenXR**
compositor, either from a standalone test harness or from inside Houdini as an
HDK LOP plugin.

Quest hardware is ARM/Android and Houdini is x86 desktop-only, so this is
inherently PCVR — Houdini runs on the PC, frames are streamed to the headset.

---

## Commands

```bash
./scripts/build.cmd                  # configure + build all three targets
./scripts/run.cmd --probe            # OpenXR runtime extensions + registered delegates
./scripts/run.cmd                    # offscreen: render one frame to hxr_frame.bmp
./scripts/run.cmd --xr               # stereo session to the headset (Ctrl+C to stop)
./scripts/run.cmd --desktop          # capture the monitor as the headset panel would, to a BMP
```

Build a single target — needed often, because Houdini holds a lock on the
plugin DLL and `build.cmd` then fails at link while `hxr` itself is fine:

```bash
cmake --build build --config Release --target hxr
```

The build uses the **newest** Houdini under `C:\Program Files\Side Effects
Software` automatically — point releases delete the old directory, so a pinned
version breaks on every update (this happened: 22.0.432 → 22.0.436). To pin
one, pass it explicitly; `build.cmd` re-passes `-DHFS` on every configure,
which also refreshes a stale cached value:

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DHFS="C:/Program Files/Side Effects Software/Houdini 22.0.436"
```

After a Houdini update, rebuild the plugin before launching — the deployed DLL
is linked against the previous install's libraries.

`run.cmd` flags: `--stage file.usd`, `--renderer PluginId`, `--max-res WxH`,
`--converge S`, `--frozen`, `--mono`, `--pick`, `--reticle`, `--desktop`, `--frames N`,
`--size WxH`, `--dist M`, `--height M`, `--out image.bmp`.

The offscreen camera is fixed at (0, 1.5, 6) looking at the origin — it does
not use the stage's RenderSettings camera — so scenes that aren't a few metres
across and centred on the origin won't be well framed. Write test output to
the scratchpad rather than the project root; `.gitignore` only covers
`hxr_frame*`.

There is **no linter, formatter, or test framework** configured — see
[Verification](#verification) for what stands in for a test suite.

**Use the `.cmd` wrappers, not the `.ps1` files directly.** This project lives
on `G:\My Drive`, a Google Drive virtual mount that `fsutil` reports as FAT32.
FAT32 has no alternate data streams, so PowerShell cannot read a Zone.Identifier
there and treats the whole volume as untrusted — `RemoteSigned` blocks the
`.ps1` files outright. `.cmd` files aren't subject to PowerShell's execution
policy, so the wrappers sidestep it without weakening any security setting.

---

## Layout

```
src/            # hxr_core — pure USD/Hydra/OpenXR/WGL, no Houdini HDK
  GLContext.*     Win32 window + WGL context (also supplies HDC/HGLRC to OpenXR)
  HydraRenderer.* UsdImagingGLEngine wrapper; selectable delegate, per-eye
                  render, AOV readback, CPU->GL upload for non-GL delegates
  XrPlatform.h    Correct include order for OpenXR's GL headers
  XrSession.*     OpenXR instance/system/session lifecycle, input, frame loop
  XrPresenterGL.* Per-eye swapchains; blits the AOV texture in
  XrMath.h        XrPosef/XrFovf -> GfMatrix4d, via GfFrustum
  RenderCamera.*  Resolves the stage's RenderSettings -> camera path/transform
  Reticle.*       Draws the reticle into the bound framebuffer (headset + offscreen)
  DesktopCapture.* Mirrors a monitor into a GL texture (DXGI duplication +
                  WGL_NV_DX_interop2) for the in-headset desktop panel
  HxrRuntime.*    Owns the render thread; the seam both consumers share
  main.cpp        Standalone harness (offscreen / --xr / --probe / --desktop)
plugin/         # LOP_HoudiniXR — the Houdini side
  LOP_XrOutput.* The "XR Output" LOP node (hxr_output)
scripts/        # build/run wrappers
```

Three CMake targets: `hxr_core` (OBJECT library), `hxr` (exe), and
`LOP_HoudiniXR` (the DSO). `hxr_core` deliberately knows nothing about the HDK
— its only job is "render a `UsdStageRefPtr` to a headset", however that stage
arrives. That separation is why the plugin reuses the standalone-validated
render path unchanged, and it's worth preserving: anything Houdini-specific
belongs in `plugin/`, reached from `hxr_core` by callback if need be.

---

## Architecture

### Cook cadence is decoupled from present cadence

This is the central design point and everything else follows from it.

- `LOP_XrOutput::cookMyLop` runs on Houdini's main/cook thread, occasionally
  (on parameter or upstream changes, and every frame while Live is on).
- `HxrRuntime` owns a separate thread running the OpenXR session and Hydra at
  headset refresh (72–120Hz), continuously, regardless of cook activity.
- The two communicate only through mutex/atomic-guarded setters
  (`SetStage`, `SetRendererPlugin`, `SetTimeCode`, `SetAnchor`,
  `SetInteractive`, `SetMoveSpeed`, `RequestResyncCamera`, …). Every one of
  them returns immediately — none opens a session, creates a GL context, or
  blocks on headset I/O, because stalling Houdini's cook thread is not
  acceptable. The render thread copies pending values out under the lock and
  applies them *after* releasing it, since applying a new stage or delegate
  rebuilds the whole Hydra engine.

The render thread creates and owns its **own** GL context. It never shares or
touches Houdini's viewport contexts.

The node is a passthrough (`cookModifyInput`, then edits only when placing a
camera), so it can sit anywhere in a LOP chain. It must be on the active cook
path (display flag, or something downstream depending on it) or it will not
cook and nothing updates.

**On upstream change** the stage is reflattened and the engine rebuilt so the
delegate picks up new content automatically. The camera anchor is deliberately
*not* re-latched by this — only `Resync Camera` or a session restart does that.

**Time sync:** `flags().setTimeDep(live)` forces a recook every playbar frame,
and `context.getFloatFrame()` feeds `UsdImagingGLRenderParams::frame`. HUSD
authors USD time samples using the Houdini frame number, not seconds.

### Delegate selection

Storm by default; Karma CPU/XPU and anything else registered are enumerated
from `HdRendererPluginRegistry`. The LOP menu filters them through
`HUSD_RendererInfo` so it matches Houdini's own viewport menu — that's what
hides the env-gated Hydra debugger and Houdini's native VK viewport delegate
(`isNativeRenderer()`).

The menu is built from `HdRendererPluginRegistry::GetPluginDescs()`, **not**
`UsdImagingGLEngine::GetRendererPlugins()`: the latter probes the current GL
context through each plugin's `IsSupported()`, and there is none current when
a parameter menu opens. Menu entries must use `PRM_Name::setTokenAndLabel()`
(deep copy) — the `PRM_Name(const char*)` constructor only references its
strings, and these come from temporaries.

### Progressive delegates get a held pose, not the live one

A progressive delegate restarts accumulation on every camera change, so
rendering at the live head pose each frame means it never gets past its first
sample. Instead `HxrRuntime` *holds* a pose: it keeps rendering at the held
pose while submitting that pose (not the live one) with the image, so the
OpenXR compositor reprojects the improving frame to wherever the head actually
is. The held frame looks like a picture fixed in space, not stuck to the face.

- `Convergence Time` — hold until converged or the time is up, then re-capture.
- `Freeze Pose` — hold indefinitely, render to convergence, then stop rendering
  and keep presenting.
- `Refreeze Pose` — re-capture now.

Storm reports converged after one pass, so under either setting it re-captures
every frame and is indistinguishable from a live viewport.

**A progressive delegate also needs one engine per eye**, since two eyes
alternating through one engine would reset it every frame regardless of the
held pose. `HydraRenderer` detects this rather than assuming: the first render
on a single shared engine reports `IsConverged()`; false means progressive,
and per-view engines are built sharing one `Hgi` via `HdDriver`. Storm stays
on one engine and pays nothing.

**Single-eye ("mono") rendering** — the node's `Stereo` toggle, off by
default — renders the configured delegate once instead of per eye: half the
work, so Karma converges twice as fast, at the cost of all depth (no
parallax). Interactive placement is always stereo. The one view is
`CyclopeanView`: midway between the eyes, eye 0's orientation (a Quest's
eyes are parallel), and the **union** of both eyes' FOVs — they're
asymmetric, each reaching further to its own side, so rendering one real eye
and showing it to both would leave the other eye a black strip on its outer
edge. It's rendered at the union's size relative to one eye (`MonoScale`,
~1.14× wider on a Quest 2) to keep pixel density; that factor is latched once
per session, since a render size that flickered by a pixel would restart a
progressive delegate forever. Only `held[0]` renders, and convergence is
judged on it alone.

**The single image is shown on a billboard, not as a projection layer.** It
first went to both eyes as a projection layer with the cyclopean pose — which
the compositor presents at infinity (identical images, zero disparity) with
no motion parallax as the head moves, while the content reads as near. That
was reported as uncomfortable. Now it's an `XrCompositionLayerQuad` fixed in
the room: standing along the captured view at the depth of whatever was
under the reticle at capture (a fresh gaze pick; 3m if nothing, clamped
0.5–100m), sized and offset to exactly fill the captured (asymmetric)
frustum at that depth. From the capture point it lines up with the scene;
the looked-at object stays at its true depth; everything else is flattened
onto that plane, like a photo standing in the room. While it's visible the
session submits no projection layer at all (the eye callbacks still run for
head tracking and the anchor latch, but their images aren't presented). The
single view is rendered in `HxrRuntime` *before* `RenderFrame`, from the
previous frame's located views — a world-locked quad makes the one-frame-old
capture invisible, and its placement must be known before the frame is
submitted. So mono only engages once views have been located
(`haveLiveHead`); the very first frames are stereo. It's redrawn every frame
(the image converges, the reticle moves), with mips like the desktop panel;
both share `XrPresenterGL`'s quad swapchains (`Quad::Panel`,
`Quad::Billboard`). Switching mono ↔ stereo forces a
re-capture (the held pose means something different in each). Changing the
**Stereo setting** while Renderer is displaying goes further and rebuilds
the engines (`HydraRenderer::Restart`) — a render from scratch, nothing
accumulated or paused carried across modes. That's keyed to the setting, not
the effective mono state, which also flips whenever a grip is held;
rebuilding there would re-sync the scene on every grip. Any other mono
switch pauses the other eyes' own engines (`HydraRenderer::PauseView`) — a progressive delegate
keeps accumulating in the background after its last render, so an idle
second-eye Karma would otherwise hold the GPU until it converged. The next
`RenderEye` for that view resumes it.

Anything that changes what a held pose would render — new stage, new delegate,
time code, anchor, render size, mono ↔ stereo — un-converges the held frames, so a frozen view
never keeps showing a stale image, or (after an engine rebuild) a texture that
no longer exists. That invalidation lives in `HxrRuntime`'s loop; keep it in
sync if you add another input that affects the render.

### Anchoring and locomotion

The stage is anchored to the **head pose at latch time**, never to the
reference-space origin. (An earlier version anchored to the origin, which in
`STAGE` space is on the floor at the play-area centre facing the room's
calibrated forward — so the render camera ended up ~1.6m below the user's eyes
with a yaw offset equal to however they were turned.)

If the stage's active RenderSettings prim (via `renderSettingsPrimPath`
metadata, not "the first one found") targets a camera: camera position → head
position, camera heading → head heading. Otherwise the stage origin sits
`Anchor Distance` ahead of the head and `Anchor Height` above the floor, along
the head's heading. **Heading only, never pitch or roll** — aligning to those
would tilt the stage so its floor no longer matches the real one. Same
convention as the headset's own "reset view".

The anchor is **latched once per session** (or on `Resync Camera`), not
tracked continuously: following an animated camera every frame would drag the
user's reference frame around and defeat free look-around. The latch happens
inside the render callback because that's where the head pose is; eye 0's pose
is used (~3cm off centre, negligible).

The same transform folds in the stage's `metersPerUnit` and `upAxis`
(`StageToRoomUnits`), since XR poses are always metres and Y-up. Without that
a centimetre or Z-up stage renders 100× too large or on its side.

**The user's own adjustments — locomotion, snap turn and grip orbit — live in
one accumulated room-space transform, `userXform`**, each new adjustment appended
last: `worldFromStage = base * userXform * orbitLive`. They must share one
transform. They were briefly separate (a locomotion translation vector, then
an orbit composed after it), which breaks as soon as they interleave: after a
90° orbit the thumbstick moved you sideways, since its direction is computed
in room space but was applied in pre-orbit space. Moving the user forward is
shifting the stage backward, so locomotion appends `Translation(-step)`.
Direction is the *live* head's full orientation from the previous frame's
callback, pitch included — free flight, deliberately unlike the anchor's
heading-only rule: the right stick dollies along the view vector and strafes
along the head's right axis, and A/B move along the head's up axis.
Snap turn appends `T(-pivot) * R_y(±angle) * T(pivot)` with the pivot on the
head centre's vertical axis, so the view turns in place; turning the user
right is *+yaw* on the stage (+Y rotation carries what's ahead to the left). A
flick fires past 0.7 deflection and re-arms below 0.3. Snap is ignored while
the grip is held — the orbit pivot is latched in room space, and turning the
stage under it would move it off its surface point.
Resync clears `userXform` (Resync means "back to the camera").

### Reticle and grip orbit

**The reticle is one 3D point projected into each eye**, never each image's
centre. Quest's per-eye FOVs are asymmetric, so the two image centres point in
different directions and a centred reticle would split in two. Projecting the
*hit point* also gives correct stereo depth — it sits on the surface instead
of floating in front of it (a depth conflict that's uncomfortable to look at).
It's projected through the eye's **held** view so it lines up with the
geometry in that image; the compositor reprojects both together. It's drawn by
`DrawReticle` (`Reticle.cpp`) with scissored `glClear`s after the blit — no
shaders, buffers or VAOs, so nothing can leak into Hydra's next pass — and it
saves/restores scissor box and clear colour. It's a free function shared by
`XrPresenterGL` and the offscreen `--reticle` path, so a test image exercises
the headset's own drawing code rather than a stand-in. It's recomputed on a *copy* of the
eye image every frame, since a frozen, converged eye reuses its image while the
gaze still moves.

The gaze ray starts at the **head centre** (midpoint of the two eyes, eye 0's
orientation) — aiming from one eye puts near hits a couple of degrees off.

**Picking** (`HydraRenderer::Pick`, the non-deprecated
`TestIntersection(PickParams, …)`) is a render pass, so it's throttled to
~15Hz for the reticle, plus a fresh pick on grip press. **It never runs on a
progressive delegate's display engine** — a pick has its own camera, which
would reset Karma's accumulation on every reticle update and break frozen-pose.
With Storm displaying it uses engine 0 (Storm re-renders fully anyway);
otherwise a dedicated Storm engine sharing the same `Hgi`, built on first use
and dropped in `_CreateEngines` (the `_isPopulated` trap applies to it too).
Hits come back in the space the view matrix factors out of — **stage** space,
since we hand Hydra `worldFromStage * eyeView`. Use
`HdxPickTokens->resolveNearestToCenter`, **not** `resolveNearestToCamera`: the
latter returns the closest point anywhere in the pick cone, which on a surface
angled toward the viewer lands at the cone's edge. Measured with `--pick` on
the smoke-test sphere: NearestToCamera was off-axis by exactly the cone's
half-width (0.023m at 5.2m); NearestToCenter lands on the ray.

**Grip orbit** (right squeeze + right aim pose): on press, pick for the pivot
(falling back to the reticle's miss distance) and record the controller's
orientation. While held, `orbitLive = T(-pivot) * (start⁻¹ * now) * T(pivot)`
— row-vector, the world-frame rotation from start to current, applied about
the pivot, so the scene turns *with* the hand (grab-and-turn; flip the delta
to reverse). On release it's committed into `userXform`, and the final
`worldFromStage` is computed **after** that commit — otherwise the release
frame renders the pre-orbit placement for one frame, a visible flicker. Grip
also counts as effective-interactive (the interactive renderer while held):
orbiting changes the view every frame, which would restart a progressive
delegate continuously.

The rotation isn't the hand's raw delta: `OrbitRotation` (XrMath.h) splits
the world-frame delta `now ⊗ start⁻¹` (swing-twist) into yaw about world +Y
and the remaining swing, multiplies the yaw by `Orbit Twist Gain` (default
3), and with `Y-Up During Orbit` (default on) reduces the swing to its pitch
about the user's horizontal right *latched at grip start* — dropping roll so
the horizon never tilts. At gain 1 without Y-up it equals the old matrix
formulation to ~1e-15 (checked in hython against `start⁻¹ * now` built with
Gf); the quaternions are OpenXR's Hamilton convention, which
`SetRotate(GfQuatd(w, x, y, z))` maps correctly, as `XrPoseToMatrix` relies
on. The controller pose comes from an OpenXR
action space located after `xrWaitFrame` (it needs the predicted display
time), so it can't live in `_SyncInput`. While the orbit is active the
reticle is drawn at the pivot — fixed in room space by construction, since
`orbitLive` rotates about it — and gaze picking is suspended; following the
gaze mid-orbit made the reticle wander off the point being orbited. Release
forces an immediate re-pick.

### Camera placement crosses the thread boundary the other way

The render thread computes the head's stage-space pose (`liveHeadToWorld *
worldFromStage⁻¹`, full orientation this time — pitch included, it's a camera)
and hands it to a callback. `hxr_core` stays HDK-free: the *plugin* installs
that callback, which posts to Houdini's main loop via
`UT_HoudiniExecutionContext::instance()->post()` (the C++ counterpart of
`hou.ui.postEventCallback`), looking the node up by `getUniqueId()` at dispatch
time rather than capturing a pointer an event could outlive.

On the main thread `applyPlacement` sets keyframes (`setFloat(...,
PRM_AK_FORCE_KEY)` at `CHgetEvalTime()`) on the `pt`/`pr` parms and
`forceRecook()`s. `cookMyLop` then authors the camera from those parms under
`HUSD_AutoWriteLock` + `HUSD_AutoLayerLock` (the `LOP_Sphere` pattern), via
`MakeMatrixXform()` with a time sample at the current frame, converting the
stage-space parm pose to the camera's parent-local space.

Placements live in **keyframes, not hidden node state**, because
`cookModifyInput` rebuilds the layer every cook — and so they also persist in
the .hip, show in the channel editor, and are editable there. The rotate parms
are XYZ Euler in Gf row-vector composition (`Rx*Ry*Rz`); `EulerFromMatrix` /
`RotationFromEuler` are the single source of truth for that, and
`applyPlacement` round-trips and warns if they ever disagree. The write lock
must be released before the live block's read lock. Since the input's
`modVersion` can't see the node's own edits, a placement sets `myOutputDirty`
to force one reflatten, and Resync-while-applied does too (that's when the
snapshot's camera is actually read).

### Playbar scrub also crosses back to the main thread

Left grip + twist of the left controller about its aim axis (`-Z`) scrubs
the playbar. `TwistAboutLocalZ` (XrMath.h) takes the twist component of the
rotation since the grip press (swing-twist decomposition), so pointing the
controller elsewhere doesn't count; *positive* twist about the controller's
`+Z` maps forward — clockwise as the user sees it, confirmed in the headset
(the opposite sign, derived on paper, ran backwards). The target frame is `round(start + quarterTurns *
Scrub Rate)`, and the start frame *tracks `_timeCode` until the first whole
frame of movement* — otherwise a grip squeezed during playback would yank the
playbar back to the press frame.

The runtime reports each new target frame through a callback. The plugin
coalesces: the latest frame goes into shared state and at most one
`post()`ed event is queued (`queued` is cleared *before* the frame is read),
because frames arrive at headset rate and a heavy scene cooks far slower —
without this, events back up and the playbar keeps moving long after the
wrist stops. The event calls `hou.playbar.stop()` (if playing) then
`hou.setFrame()` through HOM from C++. The runtime never sets `_timeCode`
itself: the new frame comes back through the normal cook → `SetTimeCode`
path, so there's only one source of truth for the time.

The left grip counts as interactive placement, like the right: each frame
change would restart a progressive delegate.

### The desktop panel is captured at the OS level

The left menu button shows Houdini's interface on a panel in the headset. The
HDK has no way to render Houdini's UI into a texture, so `DesktopCapture`
mirrors **the whole monitor** Houdini's main window is on (the process's
largest visible top-level window; the primary monitor for the standalone
exe). Not the window: Houdini's menus, the Tab menu included, are separate
popup windows that a window capture misses.

Capture is DXGI Desktop Duplication, on a D3D11 device created on the
adapter that owns the monitor (a requirement of duplication). The image is
`CopyResource`d into a texture of ours that's registered with GL through
`WGL_NV_DX_interop2` — no CPU copy — and locked only around the GL blit.
Everything runs on the render thread with the GL context current: the D3D11
immediate context is single-threaded, and so is this. `AcquireNextFrame(0)`
never blocks; pointer-only updates arrive with `LastPresentTime == 0` and no
new image. Losing the duplication (UAC, mode change) keeps the last image and
retries every 0.5s, recreating the shared texture if the mode changed.

Orientation and colour: the capture is top-row-first and GL is
bottom-row-first, so `BlitTo` flips in the blit itself (destination y
reversed). The bytes are sRGB-encoded already, so they're copied untouched
into the sRGB swapchain with `GL_FRAMEBUFFER_SRGB` off — the opposite of the
eye path, where Hydra's linear AOV is encoded on write. The pointer isn't in
the captured image (duplication reports it separately, as the shape's
top-left corner plus a hotspot from the shape info); it's marked with
`DrawReticle`.

The panel is an `XrCompositionLayerQuad` after the projection layer, with its
own swapchain: the compositor samples it once (sharper than drawing it into
the eye buffers), and it's only re-drawn when the capture reports a change —
the compositor keeps showing the last released image otherwise. The swapchain
is the capture halved exactly until ≤2560 wide (a clean 2×2 box filter in the
blit), with a full mip chain regenerated after each draw; at ~20 pixels per
degree the headset can't show more, and without mips text shimmers. It opens
1m ahead of the head's heading and 15cm below eye level, then stays fixed in
the room.

`WGL_NV_DX_interop2` needs the monitor and the GL context on the same GPU;
a laptop's integrated-GPU display would fail at `wglDXOpenDeviceNV`.

### Interactive Placement is resolved on the render thread

Effective state = the node toggle **OR** either grip held past half travel
(orbit, scrub). The node only ever sends *configured* values; `HxrRuntime`
substitutes the Interactive Placement Renderer (Storm by default) / 0s /
not-frozen while effective-interactive is true. A grip routed back through a
Houdini cook would lag, which is why this isn't node-level.

**Headset buttons act on the node's own parms**, not runtime-local
overrides: X flips Interactive Placement, Y flips Stereo, the left trigger
plays/stops the playbar. The runtime reports a `Command` through one
callback; the plugin posts to the main thread and `setInt`s the parm (or
calls `hou.playbar.play()/stop()` through HOM), and the resulting cook hands
new values back through the normal setters — so the node's UI and the
headset always agree. A toggle tolerates the cook's latency; a hold
wouldn't. With no callback (the standalone exe) the runtime flips its own
`_interactive` / `_stereo`; playback needs a host with a playbar. The
triggers used to force interactive while held; with the toggle defaulting on
that did nothing visible. The left trigger is now play/stop — an edge
derived from its analogue value with hysteresis in `_SyncInput` (Touch
triggers have no click); the right trigger is free (kept for clicking on the
desktop panel). On the node the toggle defaults **on** and Renderer
defaults to Karma XPU: place first, then render. The parms the toggle
overrides (Renderer, Convergence Time, Freeze Pose, Refreeze Pose) are
deliberately **not** greyed out, so the final render can be set up while
still placing — greying them meant turning placement off, which starts
Karma, just to change them.

The freeze capture happens on the *effective* frozen false→true transition, so
pressing X or releasing a grip with Freeze Pose set freezes right there. The Apprentice
cap is passed per renderer (`SetRendererMaxSize` /
`SetInteractiveRendererMaxSize`) because it binds to a delegate: each applies
only while its own renderer is the one rendering.

`Max Render Resolution` (0×0 = uncapped) shrinks the per-eye render buffer,
preserving aspect, and the presenter's blit upscales to the swapchain. It is
both the licence-limit control and the performance knob that makes a
progressive delegate usable. Under Apprentice with a Karma delegate it is
additionally clamped to 1280×720 automatically — detected by process executable
name (`happrentice.exe`, the C++ equivalent of `hou.applicationName() ==
"happrentice"`), since no HDK API exposes the product licence tier or its
render limit.

---

## Environment facts

All verified against the actual headers/installation in this environment.
Re-verify rather than trusting these if the Houdini version changes.

| Fact | Value |
|---|---|
| Houdini | 22.0.436 (was 22.0.432; the upgrade changed nothing below) |
| USD | 26.05 (`PXR_VERSION` 2605), SideFX's own fork |
| Namespace | standard `pxr` (no custom SideFX namespace) |
| Toolchain | MSVC 2022, C++20, `/MD /bigobj /EHsc /permissive-` |
| USD libs | `$HFS/custom/houdini/dsolib/libpxr_*.lib` |
| OpenXR | SDK `release-1.1.63`, loader linked statically via FetchContent |
| Plugin deploys to | `$HOUDINI_USER_PREF_DIR/dso` — here `C:/Users/jdive/OneDrive/Documents/houdini22.0/dso`; configure prints it |

Non-obvious constraints, each of which cost real debugging time:

- **Git Bash's `$HOME` redirects Houdini's user pref dir.** Houdini puts
  `HOUDINI_USER_PREF_DIR` under `$HOME` when it's set and under Documents
  otherwise, and `houdini_configure_target` finds it by running `hconfig`.
  Git Bash sets `HOME` for its own shells, so builds run from it deployed the
  plugin to `C:/Users/jdive/houdini22.0/dso`, which a normally launched
  Houdini never searches — every in-headset test silently ran an old DLL.
  `CMakeLists.txt` now asks `hconfig` with `HOME` unset whenever `MSYSTEM` is
  defined. The same applies to running `hython`/`hconfig` from Git Bash:
  they see a different pref dir from the user's Houdini.

- **Hgi is OpenGL-only here.** Houdini ships `hgiGL` and `hgiInterop` but *no*
  `hgiVulkan`, so Storm produces GL textures and the OpenXR runtime must expose
  `XR_KHR_opengl_enable`. The Oculus PC runtime **does** support it (verified
  with `--probe`) alongside D3D11/D3D12/Vulkan — no SteamVR layer and no
  `WGL_NV_DX_interop2` bridge are needed for the scene. Re-probe before
  believing otherwise. (The desktop panel does use that bridge, but only to
  get its D3D11 capture into GL.)
- **Storm needs a GL compatibility profile.** Under a 4.5 *core* context its
  state holder and indirect-draw path throw `invalid enum` / `invalid
  operation` and the frame is garbage.
- **Python is a hard link dependency** even for C++-only code:
  `PXR_PYTHON_SUPPORT_ENABLED` is baked into Houdini's USD build, so
  `libpxr_python` + `python313.lib` are required or you get unresolved
  `Py_NoneStruct` / boost-python converter symbols.
- **The colour AOV is `HdFormatFloat16Vec4`** (linear half-float), not 8-bit.
  Readback must convert and sRGB-encode.
- **`HdRenderBuffer::Map()` returns rows bottom-up** (GL order).
  `HydraRenderer::ReadColor` flips them so callers get an image, top row
  first. Before that fix every offscreen test image was upside down, and it
  went unnoticed all through development because every test render was a
  headlit sphere, which is radially symmetric. The headset path never read
  back, so it was never affected. **Use an asymmetric scene to check
  orientation** — a symmetric one proves nothing. Verified by rendering one
  both through `ReadColor` and through blit + `glReadPixels` (`--reticle`):
  pixel-identical away from the reticle, 0 of 25,300 sampled pixels differing.
- **`SetEnablePresentation(false)`** — we own presentation (offscreen readback
  or XR swapchain blit), so letting the engine composite only drags in
  `hgiInterop` for nothing.
- **`WIN32_LEAN_AND_MEAN` strips `IUnknown`**, which `openxr_platform.h` needs
  unconditionally in its Win32 block (not just for D3D). `XrPlatform.h`
  includes `<unknwn.h>` to fix this; keep that include order.
- **`glBlitFramebuffer` honours the scissor test.** Hydra's render pass sets a
  scissor rect matching its render size and can leave it enabled. At full size
  that's invisible; when rendering below swapchain resolution it clipped the
  upscaled blit back down to a small rectangle in the corner. The presenter
  disables scissor around the blit, and uses the AOV texture's actual
  dimensions (`HgiTexture::GetDescriptor().dimensions`) as the blit source
  rather than the size that was requested.
- **Windows 11's System32 `onnxruntime.dll` shadows Houdini's.** System32 is
  searched before `PATH`, so any Houdini-native delegate that pulls in ONNX
  (Karma does) loads the wrong one and aborts. `hxr.exe` calls
  `SetDllDirectory($HFS/bin)` at startup to slot Houdini's copy ahead of
  System32; `houdini.exe` never needs this since `$HFS/bin` is its own
  directory. `run.ps1` also imports Houdini's environment from `hconfig`.
- **Karma cannot run in the standalone exe.** It resolves `opdef:` shader paths
  through Houdini's operator framework (OP director + HDA library), which only
  exists in a real Houdini process. Without it Karma still initialises and
  renders, but produces an all-zero (black) image, then crashes on the way
  out. This is not an env-var problem. Karma is plugin-only; the standalone
  tool validates the generic delegate mechanism with Storm.

---

## Invariants — breaking these causes crashes or silent staleness

**1. Never retain a `UsdStageRefPtr` obtained from `HUSD_AutoReadLock` past the
cook that produced it.**

`HUSD_LockedStage`'s own doc comment states a LOP's stage is guaranteed
unchanged across recooks *only* via that locked-stage mechanism. HUSD reuses
the same stage object across recooks and mutates it **in place**. Handing that
pointer to the render thread and reading it later raced Houdini's cook engine
and segfaulted the whole application when the playbar moved.

The fix: `rawStage->Flatten(false)` + `UsdStage::Open()` produces a stage
sharing nothing with HUSD's objects. It **must** happen synchronously inside
`cookMyLop` while the read lock is held — deferring it to the render thread
only shrinks the race window, it does not close it.

**2. Never use `UsdStageRefPtr` pointer identity as a change signal.**

Follows directly from the above: the pointer compares equal while content
changes underneath, so newly added prims (a camera, say) silently never reach
the headset. Use Houdini's own counter instead —
`getInput(0)->dataMicroNode().modVersion()`, documented as "bumped every time
the node gets dirty" — compared against the previous cook's value.

**3. Anything the render thread reads must be independent of Houdini.**
The flattened stage qualifies. Raw HUSD data does not.

**4. The OpenXR loader allows exactly one `XrInstance` per process.**
`XrViewportSession` tears down on both destruction and a failed `Init()`
(headset not ready, runtime mid-restart), because a leaked instance makes every
later attempt fail with `XR_ERROR_LIMIT_REACHED` until Houdini restarts. The
corollary: only one `XR Output` node can be Live at a time.

**5. A new stage or delegate needs a new `UsdImagingGLEngine`, not a new
pointer handed to the old one.** The engine populates Hydra exactly once per
instance (its private `_isPopulated`), so `Render()` with a root from a
different stage is silently ignored — upstream edits never reach the delegate.
`HydraRenderer::SetStage`/`SetRendererPlugin` rebuild the engine for this
reason. Pointer identity *is* sound at that level, unlike for raw HUSD stages:
every stage reaching `HydraRenderer` is a fresh immutable flattened copy.

**6. Never submit a pose that was never captured.** `xrEndFrame` rejects a zero
quaternion with `XR_ERROR_POSE_INVALID` and the loop dies. `RenderFrame` may
skip the render callback entirely on a given frame (runtime says don't render,
or views aren't valid yet — both routine at session start), so "held" is
tracked per eye and set only inside the callback, and an eye that has never
captured is forced to capture regardless of the frame-level decision.

---

## Houdini plugin workflow

1. Close Houdini. Once it has loaded `LOP_HoudiniXR.dll`, Windows locks the
   file and the build will fail at link.
2. `./scripts/build.cmd` — this deploys straight into Houdini's `dso` folder.
3. Start Houdini. Native DSOs do not hot-reload; a new *or changed* node type
   needs a restart.
4. Wire `XR Output` (`hxr_output`) downstream of some LOP content, ensure it's
   on the cook path, toggle **Live**.

Node parameters: `Live`, `Interactive Placement`,
`Interactive Placement Renderer`, `Renderer`, `Stereo`,
`Max Render Resolution`, `Convergence Time`, `Freeze Pose`, `Refreeze Pose`,
`Anchor Distance`, `Anchor Height`, `Resync Camera`, `Move Speed`,
`Snap Turn Angle`, `Orbit Twist Gain`, `Y-Up During Orbit`, `Scrub Rate`, `Desktop Panel Width`, `Show Reticle`, `Apply Camera Placement`, `Placement Translate`,
`Placement Rotate`.

**Controller input** (`XrViewportSession`, one action set): trigger (float,
both hands; the left read alone as a hysteresis edge), grip (float, both
hands as subaction paths,
read per hand), right thumbstick (Vector2f), left thumbstick (Vector2f),
right thumbstick click (boolean, edge via `changedSinceLastSync`), left menu
button, X and Y (boolean, edge), A and B (boolean, level), aim pose
(both hands as subaction paths, one action space per hand). Actions sync at
the top of every `RenderFrame`; the aim poses are located after
`xrWaitFrame`. All read as zero / invalid when the session isn't focused. In
the headset: X = flip Interactive Placement, Y = flip Stereo, left trigger
= play/stop the playbar, right stick = dolly/strafe
along/across the view, B/A = up/down, left stick flick = snap turn, right
stick click = place the camera at the head, right grip held + turn = orbit
about the reticle, left grip held + twist = scrub the playbar, left menu
button = show/hide the desktop panel.

---

## Verification

There is no test suite. These stand in for one:

- **Offscreen** (`./scripts/run.cmd`) is the fast regression check — no headset,
  no Houdini, ~2s. Run it after any change to `hxr_core`. A healthy run prints
  the engine built, `Converged: yes after 1 pass`, and the written file.
- **`--probe`** answers "does the active OpenXR runtime support what we need"
  and "which delegates are registered", with no headset and no session.
- **`--reticle`** (offscreen) draws the reticle into the written image, via
  the headset's own sequence: pick → project → blit → `DrawReticle` → read
  back. The pick runs *before* the render, as in the headset, since a pick is
  itself a render.
- **`--pick`** (offscreen) runs the reticle's narrow-frustum pick down the view
  centre. On the built-in sphere a correct result is ≈`(0, 0.240, 0.960)` —
  on the gaze ray, radius ~0.99 because Storm tessellates the sphere into
  facets that sit just inside the true surface. A point near `z = -5` would
  mean hits came back in camera space rather than stage space.
- **`--desktop`** runs the desktop panel's capture path — duplication,
  interop, the flipping/halving blit, the pointer mark — into an sRGB texture
  like the panel swapchain, and writes it out. A correct image is upright,
  with correct colours, the size `PanelSize` gives (a 3840×2400 monitor comes
  out 1920×1200), and a cross on the mouse pointer.
- **`--xr`** needs the Quest connected via Link/Air Link. A successful start
  prints reference space, swapchain size (2080x2096 per eye on Quest 2),
  `Session running.`, and the frame count on exit.
- The plugin path, and anything involving controller input or Karma, can only
  be verified inside Houdini with hardware attached — i.e. by the user.

Oculus runtime IPC teardown spam in the console on exit is normal, not an error.

---

## Status

Confirmed working on hardware: standalone offscreen and stereo XR rendering;
the in-process LOP bridge; playbar time sync; RenderSettings-camera anchoring
and Resync; selectable delegate with engine rebuild on upstream change; Karma
in the plugin.

Built but **not yet confirmed in-headset**: head-anchored placement (replacing
the origin-anchored version), thumbstick locomotion, thumbstick-click camera
placement, X-button interactive placement, the reticle, grip orbit (and its
yaw gain / Y-up), snap turn, left-grip playbar scrub, the desktop panel's quad layer and toggle (its
capture path is verified offscreen with `--desktop`). (The
pick underneath the reticle and orbit *is* verified, offscreen, via `--pick`.)

Open questions, deliberately instrumented rather than assumed:

- **What does `Flatten()` actually cost?** The reflatten path prints its
  duration in ms. If it's single-digit, the cost concern was unfounded. If it's
  large, `HUSD_LockedStageRegistry` is the sanctioned alternative — but note the
  API to extract a `UsdStageRefPtr` from a `HUSD_LockedStagePtr` was not found
  in the public headers and may involve `gusd`'s stage cache.
- **Does `modVersion()` gating hold up in practice?** The console prints which
  path each cook takes (`input changed ... reflattened` vs `input unchanged ...
  reused snapshot`). Mostly-reused while scrubbing baked animation means it's
  working.

---

## Working practices

**Verify HDK and USD APIs against the real headers before using them.** This
codebase was built that way throughout, and it mattered: `cookMyLop` (not
`cookMyStage`), `GetCameraRel()` living on `UsdRenderSettingsBase` rather than
`UsdRenderSettings`, `ComputeLocalToWorldTransform` on `UsdGeomImageable`,
`UsdStage::Flatten()` returning an `SdfLayerRefPtr` rather than a stage — all
of these differ from what a plausible guess would produce. `toolkit/samples/`
(especially `LOP/LOP_Sphere`) and `toolkit/include/` are authoritative.

Matrix convention: `GfMatrix4d` is **row-vector** (`p' = p * M`), so "apply A
then B" composes as `A * B`. Build projections through `GfFrustum` rather than
by hand so the convention stays consistent.

When a fix rests on an assumption that can't be settled from the headers, add
a print rather than asserting it — the `Flatten()` timing and the
reflatten/reuse lines both exist for that reason, and both earned their keep.
