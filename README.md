# houdini-xr

View a Houdini Solaris stage in a Meta Quest headset, in stereo, with head
tracking — live, while you keep working in Houdini.

Houdini has no native VR viewport, and the HDK exposes no OpenXR hooks into its
Scene Viewer. This takes the other route: it renders the LOP stage with
**Hydra/Storm** (the renderer Houdini already ships) and presents the result
directly to an **OpenXR** compositor. It runs in-process as a Houdini plugin,
so there is no export step — the headset shows the stage your LOP network is
producing right now.

Because Quest hardware is ARM/Android and Houdini is x86 desktop-only, this is
PCVR: Houdini runs on the PC and frames are streamed to the headset over Link
or Air Link.

## Status

Working and validated on hardware:

- Standalone offscreen rendering (no headset, no Houdini)
- Stereo OpenXR session on Quest 2 over Oculus Link
- In-process Houdini LOP plugin bridging a live stage to the headset
- Playbar time sync, so scrubbing and playback are reflected in the headset
- Initial view positioned from a RenderSettings camera when one is authored

This is a working prototype, not a finished tool. There is no controller input,
no in-headset navigation, and no UI beyond the node's parameters.

## Requirements

- Windows (tested on Windows 11)
- Houdini 22.0 (tested against 22.0.432)
- Visual Studio 2022 Build Tools, CMake 3.24+, Git
- Meta Quest 2 or later, with Oculus Link / Air Link
- An OpenXR runtime exposing `XR_KHR_opengl_enable` — the Oculus PC runtime
  does, and is the default here

The OpenXR loader is fetched and built automatically by CMake; nothing else
needs installing.

## Building

```bash
./scripts/build.cmd
```

This builds three targets and deploys the plugin straight into Houdini's `dso`
folder, so no manual copying is needed:

| Target | What it is |
|---|---|
| `hxr_core` | Shared render core — USD/Hydra/OpenXR, no Houdini dependency |
| `hxr.exe` | Standalone test harness |
| `LOP_HoudiniXR.dll` | The Houdini plugin |

Close Houdini before rebuilding — once it has loaded the plugin, Windows locks
the DLL and the build will fail.

> Use the `.cmd` wrappers rather than the `.ps1` files directly. This project
> lives on a Google Drive mount, which PowerShell's execution policy refuses to
> run scripts from; the wrappers sidestep that without changing any security
> setting.

## Using the standalone tool

Useful for checking your setup, and for iterating on the renderer without
restarting Houdini each time.

```bash
./scripts/run.cmd --probe              # what does the active OpenXR runtime support?
./scripts/run.cmd                      # render one frame to hxr_frame.bmp
./scripts/run.cmd --stage cc.usda      # ...from a USD file
./scripts/run.cmd --xr                 # stereo session to the headset
./scripts/run.cmd --xr --frames 600    # ...for a fixed number of frames
```

Options: `--size WxH`, `--out image.bmp`, `--dist M`, `--height M`.

With no `--stage`, it renders a built-in sphere — enough to confirm the whole
path works end to end.

## Using the Houdini node

1. Start Houdini (a newly built plugin needs a restart to be picked up).
2. In a LOP network, add an **XR Output** node downstream of your content.
3. Make sure it's on the active cook path — set its display flag, or have
   something downstream depend on it. It won't update otherwise.
4. Connect the headset, then toggle **Live**.

| Parameter | Effect |
|---|---|
| **Live** | Starts/stops the XR session |
| **Anchor Distance** | How far in front of you the stage sits (metres) |
| **Anchor Height** | How high the stage sits (metres) |
| **Resync Camera** | Re-snap the view to the RenderSettings camera |

The node is a passthrough — it doesn't modify the stage, so it can sit anywhere
in the chain.

**Where you start from.** If the stage's RenderSettings prim targets a camera,
your initial position and orientation in the headset match that camera — stand
at your play area's origin facing forward and you see what it saw. Otherwise
the stage is placed in front of you using Anchor Distance/Height. Either way
the placement is set once when the session starts, so you're free to walk
around afterwards rather than being dragged along by an animated camera. Press
**Resync Camera** to re-snap.

`cc.usda` and `crag.hipnc` in this directory are small test scenes. Note that
`cc.usda` has no RenderSettings prim, so it exercises the Anchor
Distance/Height fallback rather than camera anchoring.

## How it works

The critical design point is that **cooking and rendering run at different
rates**. Houdini cooks occasionally, on edits and frame changes. The headset
needs frames at 72–120Hz, continuously. So:

- The LOP node's `cookMyLop` runs on Houdini's main thread. It takes a
  snapshot of the stage and hands it over — then returns immediately. It never
  blocks on the headset.
- A separate thread owns the OpenXR session and Storm, rendering continuously
  with its own GL context, independent of whether Houdini is cooking.

The two communicate only through a small set of guarded setters. Head tracking
updates every frame regardless of cook activity, so looking around stays smooth
even when Houdini is busy.

The snapshot is taken by flattening the stage, which detaches it from Houdini's
internal data. This matters: Houdini reuses and mutates its stage objects
between cooks, so holding a direct reference and reading it from another thread
crashes the application. See [CLAUDE.md](CLAUDE.md) for the details.

## Troubleshooting

**Black or empty view in the headset.** Check the node is on the active cook
path (display flag set). The console prints a line on every cook indicating
whether it rebuilt the stage snapshot or reused the existing one.

**Nothing happens when toggling Live.** The node has to cook for the toggle to
take effect. Same fix as above.

**`XR_ERROR_FORM_FACTOR_UNAVAILABLE`.** No headset is connected or active —
start Link/Air Link first. `--probe` works without a headset and will confirm
the runtime is otherwise healthy.

**Build fails after running Houdini.** Houdini still has the plugin DLL open.
Close it and rebuild.

**A wall of Oculus IPC log spam on exit.** Normal runtime teardown, not an
error.

## Further reading

[CLAUDE.md](CLAUDE.md) documents the architecture in depth, the verified
environment constraints, and the invariants that must not be broken when
modifying this code.
