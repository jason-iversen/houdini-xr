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
`--converge S`, `--frozen`, `--pick`, `--reticle`, `--frames N`, `--size WxH`,
`--dist M`, `--height M`, `--out image.bmp`.

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
  HxrRuntime.*    Owns the render thread; the seam both consumers share
  main.cpp        Standalone harness (offscreen / --xr / --probe)
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

Anything that changes what a held pose would render — new stage, new delegate,
time code, anchor, render size — un-converges the held frames, so a frozen view
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

**The user's own adjustments — locomotion and grip orbit — live in one
accumulated room-space transform, `userXform`**, each new adjustment appended
last: `worldFromStage = base * userXform * orbitLive`. They must share one
transform. They were briefly separate (a locomotion translation vector, then
an orbit composed after it), which breaks as soon as they interleave: after a
90° orbit the thumbstick moved you sideways, since its direction is computed
in room space but was applied in pre-orbit space. Moving the user forward is
shifting the stage backward, so locomotion appends `Translation(-step)`.
Direction follows the *live* head heading from the previous frame's callback.
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
also counts as effective-interactive (Storm while held), for the same reason
as the trigger: orbiting changes the view every frame, which would restart a
progressive delegate continuously. The controller pose comes from an OpenXR
action space located after `xrWaitFrame` (it needs the predicted display
time), so it can't live in `_SyncInput`.

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

### Interactive Placement is resolved on the render thread

Effective state = the node toggle **OR** either controller trigger held past
half travel (`XrViewportSession::TriggerValue()`). The node only ever sends
*configured* values; `HxrRuntime` substitutes Storm / 0s / not-frozen while
effective-interactive is true. A trigger routed back through a Houdini cook
would lag, which is why this isn't node-level. `updateParmsFlags` greys the
overridden parms for the toggle only.

The freeze capture happens on the *effective* frozen false→true transition, so
releasing the trigger with Freeze Pose set freezes right there. The Apprentice
cap is passed separately (`SetRendererMaxSize`) because it binds to the
configured delegate, not to the Storm substitute.

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
| Plugin deploys to | `$HOUDINI_USER_PREF_DIR/dso` (here: under OneDrive) |

Non-obvious constraints, each of which cost real debugging time:

- **Hgi is OpenGL-only here.** Houdini ships `hgiGL` and `hgiInterop` but *no*
  `hgiVulkan`, so Storm produces GL textures and the OpenXR runtime must expose
  `XR_KHR_opengl_enable`. The Oculus PC runtime **does** support it (verified
  with `--probe`) alongside D3D11/D3D12/Vulkan — no SteamVR layer and no
  `WGL_NV_DX_interop2` bridge are needed. Re-probe before believing otherwise.
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

Node parameters: `Live`, `Interactive Placement`, `Renderer`,
`Max Render Resolution`, `Convergence Time`, `Freeze Pose`, `Refreeze Pose`,
`Anchor Distance`, `Anchor Height`, `Resync Camera`, `Move Speed`,
`Show Reticle`, `Apply Camera Placement`, `Placement Translate`,
`Placement Rotate`.

**Controller input** (`XrViewportSession`, one action set): trigger (float,
both hands), right thumbstick (Vector2f), right thumbstick click (boolean,
edge via `changedSinceLastSync`), right squeeze (float), right aim pose (via
an action space). Actions sync at the top of every `RenderFrame`; the aim
pose is located after `xrWaitFrame`. All read as zero / invalid when the
session isn't focused. In the headset: trigger held = interactive placement,
thumbstick = move, thumbstick click = place the camera at the head, grip held
+ turn = orbit about the reticle.

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
placement, trigger-driven interactive placement, the reticle, grip orbit. (The
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
