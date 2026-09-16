#include "HxrRuntime.h"

#include "GLContext.h"
#include "HydraRenderer.h"
#include "RenderCamera.h"
#include "XrMath.h"
#include "XrSession.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

HxrRuntime::~HxrRuntime()
{
    Stop();
}

void HxrRuntime::Start()
{
    if (_thread.joinable()) {
        return;   // already running, or mid-shutdown from a prior Stop()
    }
    _stopRequested   = false;
    _failed          = false;
    _framesPresented = 0;

    // Stop() destroys the render thread's HydraRenderer along with the stage
    // and delegate choice it held, but the pending values outlive it. Re-arm
    // them so a restarted thread re-applies what it already has: the caller
    // may legitimately not hand us anything new (nothing changed while we
    // were stopped), and without this the fresh thread would come up with no
    // stage at all and render black -- which is exactly what toggling Live
    // off and on used to do.
    {
        std::lock_guard<std::mutex> lock(_stageMutex);
        if (_pendingStage) {
            _stageDirty = true;
        }
        if (!_pendingRenderer.empty()) {
            _rendererDirty = true;
        }
    }

    _thread = std::thread(&HxrRuntime::ThreadMain, this);
}

void HxrRuntime::Stop()
{
    _stopRequested = true;
    if (_thread.joinable()) {
        _thread.join();
    }
    _running = false;
}

void HxrRuntime::SetStage(UsdStageRefPtr const& stage)
{
    std::lock_guard<std::mutex> lock(_stageMutex);
    _pendingStage = stage;
    _stageDirty   = true;
}

void HxrRuntime::SetAnchor(float distMeters, float heightMeters)
{
    _anchorDist   = distMeters;
    _anchorHeight = heightMeters;
}

void HxrRuntime::SetRendererPlugin(std::string const& pluginId)
{
    std::lock_guard<std::mutex> lock(_stageMutex);
    if (pluginId != _pendingRenderer) {
        _pendingRenderer = pluginId;
        _rendererDirty   = true;
    }
}

void HxrRuntime::SetMaxRenderSize(int maxWidth, int maxHeight)
{
    _maxRenderWidth  = maxWidth;
    _maxRenderHeight = maxHeight;
}

void HxrRuntime::SetFrozen(bool frozen)
{
    // Turning freezing on captures the pose at that moment, rather than
    // keeping whatever was last held from the timed mode.
    if (_frozen.exchange(frozen) != frozen && frozen) {
        _refreezeRequested = true;
    }
}

namespace {

// A room-level frame taken from a head pose: where the head is, and which way
// it faces *horizontally*. Pitch and roll are deliberately dropped -- aligning
// the stage to them would tilt it so its floor no longer matches the real one.
struct HeadFrame
{
    GfVec3d position{0.0, 0.0, 0.0};
    GfVec3d forward{0.0, 0.0, -1.0};   // horizontal, unit length
};

GfVec3d Horizontal(GfVec3d direction)
{
    direction[1] = 0.0;
    // Looking straight up or down leaves no heading; keep the room's forward.
    return direction.GetLength() < 1e-6 ? GfVec3d(0.0, 0.0, -1.0) : direction.GetNormalized();
}

HeadFrame HeadFrameFromPose(XrPosef const& pose)
{
    const GfMatrix4d headToWorld = XrPoseToMatrix(pose);
    return {headToWorld.ExtractTranslation(),
            Horizontal(headToWorld.TransformDir(GfVec3d(0.0, 0.0, -1.0)))};
}

// Rotation about +Y taking horizontal direction `from` to `to`.
GfMatrix4d YawBetween(GfVec3d const& from, GfVec3d const& to)
{
    const double radians = std::atan2(GfCross(from, to)[1], GfDot(from, to));
    GfMatrix4d m;
    m.SetRotate(GfRotation(GfVec3d(0.0, 1.0, 0.0), radians * 180.0 / M_PI));
    return m;
}

// Stage units -> metres and stage up -> +Y, so that everything downstream can
// treat stage coordinates as room coordinates. XR poses are always metres
// and Y-up; a centimetre or Z-up stage would otherwise come out 100x too
// large or lying on its side.
GfMatrix4d StageToRoomUnits(UsdStageRefPtr const& stage)
{
    GfMatrix4d m(1.0);
    if (!stage) {
        return m;
    }
    m.SetScale(UsdGeomGetStageMetersPerUnit(UsdStageWeakPtr(stage)));
    if (UsdGeomGetStageUpAxis(UsdStageWeakPtr(stage)) == UsdGeomTokens->z) {
        GfMatrix4d zUpToYUp;
        zUpToYUp.SetRotate(GfRotation(GfVec3d(1.0, 0.0, 0.0), -90.0));
        m = m * zUpToYUp;
    }
    return m;
}

GfMatrix4d Translation(GfVec3d const& t)
{
    GfMatrix4d m;
    m.SetTranslate(t);
    return m;
}

// Largest size that fits within the cap while keeping the eye's aspect ratio;
// scaling the two axes independently would stretch the image.
GfVec2i FitWithin(GfVec2i const& eye, int maxWidth, int maxHeight)
{
    double scale = 1.0;
    if (maxWidth > 0) {
        scale = std::min(scale, double(maxWidth) / double(eye[0]));
    }
    if (maxHeight > 0) {
        scale = std::min(scale, double(maxHeight) / double(eye[1]));
    }
    return GfVec2i(std::max(1, int(eye[0] * scale)), std::max(1, int(eye[1] * scale)));
}

} // namespace

void HxrRuntime::ThreadMain()
{
    GLContext gl;
    if (!gl.Create()) {
        _failed = true;
        return;
    }
    gl.MakeCurrent();

    if (!GarchGLApiLoad()) {
        std::fprintf(stderr, "HxrRuntime: GarchGLApiLoad failed -- no usable GL context\n");
        _failed = true;
        return;
    }

    XrViewportSession xr;
    if (!xr.Init(gl)) {
        _failed = true;
        return;
    }

    const GfVec2i eyeSize(int(xr.EyeWidth()), int(xr.EyeHeight()));
    const int     viewCount = int(xr.ViewCount());

    HydraRenderer renderer;
    renderer.Init(FitWithin(eyeSize, _maxRenderWidth.load(), _maxRenderHeight.load()),
                  viewCount);

    _running = true;

    // The pose each eye is currently rendering from (see SetConvergeSeconds
    // in the header for why this isn't simply the live pose every frame), and
    // the last image produced from it -- reused as-is once a frozen view has
    // converged, so a finished frame costs nothing to keep showing.
    struct HeldEye
    {
        XrPosef    pose{};
        XrFovf     fov{};
        GfMatrix4d view;
        GfMatrix4d proj;
        XrViewportSession::EyeImage image;
        bool       held      = false;   // set only when a pose is actually captured
        bool       converged = false;
    };
    std::vector<HeldEye> held(static_cast<size_t>(viewCount));
    std::chrono::steady_clock::time_point heldSince = std::chrono::steady_clock::now();

    // Anything that changes what a held pose would render must un-converge
    // the held frames, or a frozen view would keep showing a stale image --
    // or, after an engine rebuild, a texture that no longer exists.
    double     lastTimeCode = std::numeric_limits<double>::quiet_NaN();
    GfMatrix4d lastWorldFromStage(0.0);
    GfVec2i    lastRenderSize(-1, -1);
    GfVec2i    lastTextureSize(-1, -1);

    constexpr double kNearPlane = 0.05;
    constexpr double kFarPlane  = 5000.0;

    // The anchor: where the stage sits in the room. Latched once per session
    // (or on Resync), not re-evaluated every frame -- continuously following
    // the render camera would drag the user's whole reference frame around on
    // every animated camera move, which defeats free look-around.
    //
    // It is anchored to the *head at latch time*, not to the reference-space
    // origin. In STAGE space that origin is on the floor at the play-area
    // centre, facing the room's calibrated forward -- so anchoring there put
    // the render camera ~1.6m below the user's eyes with a yaw offset equal
    // to however they happened to be turned. With a render camera: camera
    // position -> head position, camera heading -> head heading. Without one:
    // the stage origin sits Anchor Distance ahead of the head, Anchor Height
    // above the floor. Heading only, never pitch (see HeadFrame).
    bool       anchorLatched    = false;
    bool       anchorFromCamera = false;
    HeadFrame  anchorHead;
    GfMatrix4d stageFromCamera(1.0);       // raw, in stage units
    GfMatrix4d roomFromStageUnits(1.0);    // metres + Y-up; refreshed with the stage

    // Row-vector composition: each factor applies after the one before it.
    auto computeWorldFromStage = [&]() -> GfMatrix4d {
        if (!anchorLatched) {
            return GfMatrix4d(1.0);
        }
        if (anchorFromCamera) {
            const GfVec3d cameraPos =
                roomFromStageUnits.Transform(stageFromCamera.ExtractTranslation());
            const GfVec3d cameraFwd = Horizontal(roomFromStageUnits.TransformDir(
                stageFromCamera.TransformDir(GfVec3d(0.0, 0.0, -1.0))));   // USD cameras look down -Z
            return roomFromStageUnits *
                   Translation(-cameraPos) *
                   YawBetween(cameraFwd, anchorHead.forward) *
                   Translation(anchorHead.position);
        }
        const GfVec3d ahead(0.0, double(_anchorHeight.load()), -double(_anchorDist.load()));
        return roomFromStageUnits *
               Translation(ahead) *
               YawBetween(GfVec3d(0.0, 0.0, -1.0), anchorHead.forward) *
               Translation(GfVec3d(anchorHead.position[0], 0.0, anchorHead.position[2]));
    };

    while (!_stopRequested.load() && xr.PollEvents()) {
        // Copy the pending values out under the lock, then apply them after
        // releasing it: applying can rebuild the whole Hydra engine, and the
        // cook thread's SetStage()/SetRendererPlugin() must never block
        // behind that.
        UsdStageRefPtr newStage;
        std::string    newRenderer;
        bool           applyStage    = false;
        bool           applyRenderer = false;
        {
            std::lock_guard<std::mutex> lock(_stageMutex);
            if (_rendererDirty) {
                newRenderer    = _pendingRenderer;
                applyRenderer  = true;
                _rendererDirty = false;
            }
            if (_stageDirty) {
                newStage    = _pendingStage;
                applyStage  = true;
                _stageDirty = false;
            }
        }

        // Delegate first, so a stage arriving in the same frame builds its
        // engine once, with the right delegate, rather than twice.
        if (applyRenderer) {
            renderer.SetRendererPlugin(TfToken(newRenderer));
        }
        if (applyStage) {
            // This is what "syncs the delegate" on upstream change: SetStage
            // rebuilds the engine so Hydra repopulates from the new content.
            // The camera anchor is deliberately left alone here -- that only
            // re-latches via the Resync button (or a session restart).
            renderer.SetStage(newStage);
            roomFromStageUnits = StageToRoomUnits(newStage);
        }

        if (_resyncRequested.exchange(false)) {
            anchorLatched    = false;
            anchorFromCamera = false;
        }

        if (!xr.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // The anchor itself is latched inside the render callback, where the
        // head pose is available. Anchor Distance/Height are read live in
        // here, so those sliders still take effect without a resync.
        GfMatrix4d worldFromStage = computeWorldFromStage();

        const double timeCode = _timeCode.load();

        // Cheap to re-derive every frame (two atomics), and SetRenderSize is a
        // no-op when unchanged -- so the cap can be adjusted live.
        const GfVec2i renderSize =
            FitWithin(eyeSize, _maxRenderWidth.load(), _maxRenderHeight.load());
        renderer.SetRenderSize(renderSize);

        const bool contentChanged = applyStage || applyRenderer ||
                                    timeCode != lastTimeCode ||
                                    worldFromStage != lastWorldFromStage ||
                                    renderSize != lastRenderSize;
        if (contentChanged) {
            for (HeldEye& eye : held) {
                eye.converged = false;
            }
            lastTimeCode       = timeCode;
            lastWorldFromStage = worldFromStage;
            lastRenderSize     = renderSize;
        }

        // Decide once per frame whether every eye re-captures the live pose.
        // Both eyes must move together or they'd disagree about where the
        // head is.
        const bool  frozen          = _frozen.load();
        const float convergeSeconds = _convergeSeconds.load();
        const auto  now             = std::chrono::steady_clock::now();

        // Only a decision here -- the capture itself happens in the callback,
        // and so does bookkeeping about it. RenderFrame may not call the
        // callback at all on a given frame (the runtime says not to render,
        // or the views aren't valid yet, both routine at session start), and
        // recording "held" before the pose was actually taken is how a zero
        // pose ends up in xrEndFrame.
        bool recapture = _refreezeRequested.exchange(false);
        if (!recapture && !frozen) {
            bool allConverged = true;
            for (HeldEye const& eye : held) {
                allConverged = allConverged && eye.converged;
            }
            const double heldFor =
                std::chrono::duration<double>(now - heldSince).count();
            recapture = allConverged || convergeSeconds <= 0.0f ||
                        heldFor >= double(convergeSeconds);
        }

        auto renderEye = [&](uint32_t index, XrView const& current) {
            HeldEye& eye = held[index];

            // Latch the anchor from the first eye's pose. Eye 0 is ~3cm off
            // the head centre, which is well below anything noticeable here.
            if (index == 0 && !anchorLatched && renderer.Stage()) {
                anchorHead       = HeadFrameFromPose(current.pose);
                anchorFromCamera = FindRenderCameraTransform(
                    renderer.Stage(), UsdTimeCode(timeCode), &stageFromCamera);
                anchorLatched = true;
                worldFromStage = computeWorldFromStage();
                std::printf("HxrRuntime: anchor latched %s, head at (%.2f, %.2f, %.2f)\n",
                            anchorFromCamera ? "to RenderSettings camera"
                                             : "to Anchor Distance/Height",
                            anchorHead.position[0], anchorHead.position[1],
                            anchorHead.position[2]);
            }

            // An eye that has never captured a pose has nothing valid to
            // submit, whatever the frame-level decision was.
            if (recapture || !eye.held) {
                eye.pose      = current.pose;
                eye.fov       = current.fov;
                eye.view      = XrPoseToViewMatrix(current.pose);
                eye.proj      = XrFovToProjectionMatrix(current.fov, kNearPlane, kFarPlane);
                eye.held      = true;
                eye.converged = false;
                heldSince     = now;
            }

            // A frozen, converged eye has nothing left to render: keep
            // presenting the frame it already has.
            if (!(frozen && eye.converged)) {
                renderer.RenderEye(int(index), worldFromStage * eye.view, eye.proj, timeCode);
                eye.converged = renderer.IsConverged(int(index));

                const HydraRenderer::ColorTexture colour = renderer.GetColorTexture(int(index));
                if (index == 0 && (colour.width != lastTextureSize[0] ||
                                   colour.height != lastTextureSize[1])) {
                    lastTextureSize = GfVec2i(colour.width, colour.height);
                    std::printf("HxrRuntime: colour AOV texture %dx%d (render size %dx%d, "
                                "swapchain %dx%d)\n",
                                colour.width, colour.height,
                                renderer.RenderSize()[0], renderer.RenderSize()[1],
                                eyeSize[0], eyeSize[1]);
                }
                eye.image = XrViewportSession::EyeImage{
                    colour.id, colour.width, colour.height, eye.pose, eye.fov};
            }
            return eye.image;
        };

        if (!xr.RenderFrame(renderEye)) {
            break;
        }
        ++_framesPresented;
    }

    xr.Shutdown();
    _running = false;
}
