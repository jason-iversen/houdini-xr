# houdini-xr

An XR viewport for SideFX Houdini, targeting Meta Quest 2 and later over PCVR
(Link / Air Link). Houdini's own Scene Viewer is not involved: this renders the
LOP stage with **Hydra/Storm** and presents it to an **OpenXR** compositor,
either from a standalone test harness or from inside Houdini as an HDK LOP
plugin.

Quest hardware is ARM/Android and Houdini is x86 desktop-only, so this is
inherently PCVR — Houdini runs on the PC, frames are streamed to the headset.

---

## Quick start

```bash
./scripts/build.cmd          # configure + build all targets
./scripts/run.cmd --probe    # list the active OpenXR runtime's extensions
./scripts/run.cmd            # offscreen: render one frame to hxr_frame.bmp
./scripts/run.cmd --xr       # stereo session to the headset (Ctrl+C to stop)
```

Useful flags: `--stage file.usd`, `--frames N`, `--size WxH`, `--dist M`,
`--height M`, `--out image.bmp`.

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
  StormRenderer.* UsdImagingGLEngine wrapper; per-eye render, AOV readback
  XrPlatform.h    Correct include order for OpenXR's GL headers
  XrSession.*     OpenXR instance/system/session lifecycle + frame loop
  XrPresenterGL.* Per-eye swapchains; blits Storm's AOV texture in
  XrMath.h        XrPosef/XrFovf -> GfMatrix4d, via GfFrustum
  RenderCamera.*  Resolves the stage's RenderSettings -> camera transform
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
render path unchanged.

---

## Architecture

**Cook cadence is decoupled from present cadence.** This is the central design
point and everything else follows from it.

- `LOP_XrOutput::cookMyLop` runs on Houdini's main/cook thread, occasionally
  (on parameter or upstream changes, and every frame while Live is on).
- `HxrRuntime` owns a separate thread running the OpenXR session and Storm at
  headset refresh (72–120Hz), continuously, regardless of cook activity.
- The two communicate only through mutex/atomic-guarded setters
  (`SetStage`, `SetTimeCode`, `SetAnchor`, `RequestResyncCamera`). Every one of
  them returns immediately — none opens a session, creates a GL context, or
  blocks on headset I/O, because stalling Houdini's cook thread is not
  acceptable.

The render thread creates and owns its **own** GL context. It never shares or
touches Houdini's viewport contexts.

The node is a passthrough: `cookModifyInput` then no edits, so it can sit
anywhere in a LOP chain. It must be on the active cook path (display flag, or
something downstream depending on it) or it will not cook and nothing updates.

---

## Environment facts

All verified against the actual headers/installation in this environment.
Re-verify rather than trusting these if the Houdini version changes.

| Fact | Value |
|---|---|
| Houdini | 22.0.432, `C:\Program Files\Side Effects Software\Houdini 22.0.432` |
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
- **`SetEnablePresentation(false)`** — we own presentation (offscreen readback
  or XR swapchain blit), so letting the engine composite only drags in
  `hgiInterop` for nothing.
- **`WIN32_LEAN_AND_MEAN` strips `IUnknown`**, which `openxr_platform.h` needs
  unconditionally in its Win32 block (not just for D3D). `XrPlatform.h`
  includes `<unknwn.h>` to fix this; keep that include order.

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

---

## Houdini plugin workflow

1. Close Houdini. Once it has loaded `LOP_HoudiniXR.dll`, Windows locks the
   file and the build will fail.
2. `./scripts/build.cmd` — this deploys straight into Houdini's `dso` folder.
3. Start Houdini. Native DSOs do not hot-reload; a new *or changed* node type
   needs a restart.
4. Wire `XR Output` (`hxr_output`) downstream of some LOP content, ensure it's
   on the cook path, toggle **Live**.

Node parameters: `Live`, `Anchor Distance`, `Anchor Height`, `Resync Camera`.

**Anchor behaviour:** if the stage's active RenderSettings prim (via the
`renderSettingsPrimPath` metadata, not "the first one found") targets a camera,
that camera's inverted stage transform becomes the initial placement — standing
at the room's calibrated origin facing its native forward shows what the camera
saw. Otherwise it falls back to `Anchor Distance`/`Anchor Height` in front of
the user. The camera anchor is **latched once per session**, not tracked
continuously: following an animated camera every frame would drag the user's
reference frame around and defeat free look-around. `Resync Camera` (or Live
off/on) re-latches.

**Time sync:** `flags().setTimeDep(live)` forces a recook every playbar frame,
and `context.getFloatFrame()` feeds `UsdImagingGLRenderParams::frame`. HUSD
authors USD time samples using the Houdini frame number, not seconds.

---

## Testing

- **Offscreen** (`./scripts/run.cmd`) is the fast regression check — no headset,
  no Houdini. Run it after any change to `hxr_core`.
- **`--probe`** answers "does the active OpenXR runtime support what we need"
  with no headset and no session.
- **`--xr`** needs the Quest connected via Link/Air Link. A successful start
  prints reference space, swapchain size (2080x2096 per eye on Quest 2),
  `Session running.`, and the frame count on exit.
- The plugin path can only be verified inside Houdini with hardware attached.

Oculus runtime IPC teardown spam in the console on exit is normal, not an error.

---

## Status

Working: standalone offscreen and stereo XR rendering; the in-process LOP
bridge; playbar time sync; RenderSettings-camera anchoring; Resync button.

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
