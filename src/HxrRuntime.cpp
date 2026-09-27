#include "HxrRuntime.h"

#include "DesktopCapture.h"
#include "GLContext.h"
#include "HydraRenderer.h"
#include "RenderCamera.h"
#include "XrMath.h"
#include "XrSession.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
        if (!_pendingInteractiveRenderer.empty()) {
            _interactiveRendererDirty = true;
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

void HxrRuntime::SetInteractiveRenderer(std::string const& pluginId)
{
    std::lock_guard<std::mutex> lock(_stageMutex);
    if (pluginId != _pendingInteractiveRenderer) {
        _pendingInteractiveRenderer = pluginId;
        _interactiveRendererDirty   = true;
    }
}

void HxrRuntime::SetInteractiveRendererMaxSize(int maxWidth, int maxHeight)
{
    _interactiveMaxWidth  = maxWidth;
    _interactiveMaxHeight = maxHeight;
}

void HxrRuntime::SetMaxRenderSize(int maxWidth, int maxHeight)
{
    _maxRenderWidth  = maxWidth;
    _maxRenderHeight = maxHeight;
}

void HxrRuntime::SetRendererMaxSize(int maxWidth, int maxHeight)
{
    _rendererMaxWidth  = maxWidth;
    _rendererMaxHeight = maxHeight;
}

void HxrRuntime::SetPlacementCallback(PlacementCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _placementCallback = std::move(callback);
}

void HxrRuntime::SetCommandCallback(CommandCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _commandCallback = std::move(callback);
}

void HxrRuntime::SetScrubCallback(ScrubCallback callback)
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    _scrubCallback = std::move(callback);
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

// Single-eye rendering's one view, standing in for both eyes: from the
// midpoint between them, facing the way eye 0 does (a Quest's eyes are
// parallel), with a field of view covering both. The eyes' FOVs are
// asymmetric -- each reaches further to its own side -- so rendering one
// real eye and showing it to both would leave the other eye a black strip
// along its outer edge.
XrView CyclopeanView(XrViewportSession const& xr)
{
    XrView view = xr.LocatedView(0);
    if (xr.ViewCount() < 2) {
        return view;
    }
    XrView const& other = xr.LocatedView(1);
    view.pose.position = {0.5f * (view.pose.position.x + other.pose.position.x),
                          0.5f * (view.pose.position.y + other.pose.position.y),
                          0.5f * (view.pose.position.z + other.pose.position.z)};
    view.fov.angleLeft  = std::min(view.fov.angleLeft,  other.fov.angleLeft);
    view.fov.angleRight = std::max(view.fov.angleRight, other.fov.angleRight);
    view.fov.angleDown  = std::min(view.fov.angleDown,  other.fov.angleDown);
    view.fov.angleUp    = std::max(view.fov.angleUp,    other.fov.angleUp);
    return view;
}

// How much larger the cyclopean view's image plane is than one eye's, per
// axis -- the factor to render it at, to keep one eye's pixel density. False
// until the views have been located.
bool MonoScale(XrViewportSession const& xr, GfVec2d* scale)
{
    XrFovf const& eye  = xr.LocatedView(0).fov;
    const XrFovf  both = CyclopeanView(xr).fov;
    const double eyeW  = std::tan(eye.angleRight) - std::tan(eye.angleLeft);
    const double eyeH  = std::tan(eye.angleUp) - std::tan(eye.angleDown);
    if (eyeW <= 0.0 || eyeH <= 0.0) {
        return false;
    }
    *scale = GfVec2d((std::tan(both.angleRight) - std::tan(both.angleLeft)) / eyeW,
                     (std::tan(both.angleUp) - std::tan(both.angleDown)) / eyeH);
    return true;
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

    // What the node asked for vs what's actually running -- they differ while
    // interactive placement (toggle or grip) substitutes the
    // interactive renderer.
    std::string configuredRenderer;
    std::string interactiveRenderer;   // empty = Storm
    std::string activeRenderer;
    bool        lastFrozen = false;
    bool        lastMono   = false;
    bool        lastStereo = _stereo.load();
    // Where the single-eye billboard stands: at the depth of whatever was
    // under the reticle when it was captured, so the thing being looked at
    // stays at its true distance and only the rest is flattened onto it.
    constexpr double kBillboardFallback = 3.0;    // metres, nothing under the reticle
    constexpr double kBillboardMin      = 0.5;
    constexpr double kBillboardMax      = 100.0;
    double      billboardDistance = kBillboardFallback;
    // Latched once per session rather than re-derived every frame: if the
    // runtime's reported FOVs wobbled at all, a per-frame value could flip
    // the render size by a pixel and restart a progressive delegate forever.
    GfVec2d     monoScale(1.0, 1.0);
    bool        monoScaleLatched = false;

    // The user's own adjustments to where the stage sits -- thumbstick
    // locomotion, snap turn and grip orbit -- accumulated as one room-space transform,
    // each new adjustment appended last. Holding them separately (a
    // translation vector, then an orbit) breaks as soon as they interleave:
    // after a 90-degree orbit the thumbstick would move you sideways, since
    // its direction is computed in room space but would be applied in
    // pre-orbit space. Moving the user forward is shifting the stage back.
    // Resync clears it (Resync means "back to the camera").
    //
    // Head poses come from the previous frame's render callback -- one frame
    // of latency, not perceptible.
    GfMatrix4d userXform(1.0);
    HeadFrame  liveHead;
    GfMatrix4d liveHeadToWorld(1.0);   // eye 0's full pose, for camera placement
    bool       haveLiveHead = false;
    GfVec3d    eyePos[2] = {GfVec3d(0.0), GfVec3d(0.0)};
    bool       haveEye1 = false;
    auto       lastFrameTime = std::chrono::steady_clock::now();
    constexpr double kStickDeadzone = 0.15;
    constexpr double kMaxFrameDt    = 0.1;   // don't lurch after a stall

    // Snap turn: one step per flick of the left stick. Hysteresis, so a
    // stick hovering near the threshold can't fire repeatedly -- it has to
    // come back near centre before the next flick counts.
    constexpr double kSnapFire  = 0.7;
    constexpr double kSnapRearm = 0.3;
    bool             snapArmed  = true;

    // Grip orbit: while held, the stage rotates about the surface point under
    // the reticle by however the right controller has turned since the grip
    // began -- grab-and-turn, so the scene turns with the hand, with yaw
    // amplified (see OrbitRotation). Committed into userXform on release.
    constexpr float kGripHeldThreshold = 0.5f;
    bool          gripWasHeld = false;
    GfVec3d       orbitPivotRoom(0.0);
    XrQuaternionf orbitStartOrientation{0.0f, 0.0f, 0.0f, 1.0f};   // at grip start
    GfVec3d       orbitRight(1.0, 0.0, 0.0);   // user's horizontal right, at grip start
    GfMatrix4d    orbitLive(1.0);

    // Playbar scrub (left grip + twist). The start frame follows the
    // playbar until the twist first moves it a whole frame, so a grip
    // squeezed during playback doesn't yank the playbar back to where it was
    // at the press.
    bool          scrubHeld  = false;
    bool          scrubMoved = false;
    XrQuaternionf scrubStartOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    double        scrubStartFrame = 0.0;
    double        scrubLastFrame  = 0.0;

    // Desktop panel (left menu button): Houdini's interface on a flat panel,
    // composited over the scene. Placed where the user is looking when it
    // opens, level and just below eye height like a monitor, then fixed in
    // the room. Capture is set up on first open rather than at session start
    // -- most sessions never use it. Declared after the renderer so it's
    // destroyed first, while the GL context is still current.
    DesktopCapture desktop;
    bool           panelOpen  = false;
    bool           panelFresh = false;   // just opened: draw even if the desktop is unchanged
    XrPosef        panelPose{};
    constexpr double kPanelDistance = 1.0;    // metres
    constexpr double kPanelDrop     = 0.15;   // metres below eye level

    // Reticle: a ray from the head centre along the gaze, picked against the
    // scene. Picking is a render pass, so it's throttled; the stage-space hit
    // stays glued to its surface between picks, including through an orbit.
    constexpr double kPickInterval          = 1.0 / 15.0;
    constexpr double kReticleMissDistance   = 2.0;   // metres, when nothing is hit
    constexpr double kPickFovDegrees        = 0.5;   // narrow enough to act as a ray
    GfVec3d reticleStage(0.0);
    bool    reticleOnSurface = false;
    auto    lastPickTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);

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
    SdfPath    snapPath;                   // the camera/light last snapped to, if any
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
        std::string    newInteractiveRenderer;
        bool           applyStage    = false;
        bool           applyRenderer = false;
        bool           applyInteractiveRenderer = false;
        {
            std::lock_guard<std::mutex> lock(_stageMutex);
            if (_rendererDirty) {
                newRenderer    = _pendingRenderer;
                applyRenderer  = true;
                _rendererDirty = false;
            }
            if (_interactiveRendererDirty) {
                newInteractiveRenderer    = _pendingInteractiveRenderer;
                applyInteractiveRenderer  = true;
                _interactiveRendererDirty = false;
            }
            if (_stageDirty) {
                newStage    = _pendingStage;
                applyStage  = true;
                _stageDirty = false;
            }
        }

        if (applyRenderer) {
            configuredRenderer = newRenderer;
        }
        if (applyInteractiveRenderer) {
            interactiveRenderer = newInteractiveRenderer;
        }

        // Effective state: the toggle, or either grip held past half travel
        // (an orbit or a scrub). Resolved here rather than in the node so a
        // grip takes effect this frame, not after a cook. Both change the
        // view every frame -- the camera, or the time -- which would restart
        // a progressive delegate continuously, so they're shown live in the
        // interactive renderer, and releasing hands back to the configured
        // delegate (freezing there if Freeze Pose is on).
        const bool gripHeld     = xr.GripValue() > kGripHeldThreshold;
        const bool leftGripHeld = xr.LeftGripValue() > kGripHeldThreshold;
        const bool interactive  = _interactive.load() || gripHeld || leftGripHeld;
        const bool  frozen          = !interactive && _frozen.load();
        const float convergeSeconds = interactive ? 0.0f : _convergeSeconds.load();

        // Single-eye rendering is for the configured delegate only:
        // interactive placement is always stereo -- it's cheap there, and
        // depth matters most while placing.
        // Only once views have been located: the single view is captured
        // from them before RenderFrame (see the billboard below).
        const bool mono = !interactive && !_stereo.load() && viewCount > 1 && haveLiveHead;
        const bool monoSwitched = mono != lastMono;
        lastMono = mono;

        // Delegate first, so a stage arriving in the same frame builds its
        // engine once, with the right delegate, rather than twice.
        const std::string wantRenderer =
            !interactive                ? configuredRenderer
            : interactiveRenderer.empty() ? HydraRenderer::DefaultRendererId().GetString()
                                          : interactiveRenderer;
        const bool rendererSwitched = wantRenderer != activeRenderer;
        if (rendererSwitched) {
            renderer.SetRendererPlugin(TfToken(wantRenderer));
            activeRenderer = wantRenderer;
        }
        if (applyStage) {
            // This is what "syncs the delegate" on upstream change: SetStage
            // rebuilds the engine so Hydra repopulates from the new content.
            // The camera anchor is deliberately left alone here -- that only
            // re-latches via the Resync button (or a session restart).
            renderer.SetStage(newStage);
            roomFromStageUnits = StageToRoomUnits(newStage);
        }

        // Switching Stereo while Renderer is displaying restarts the render
        // from scratch -- fresh engines, so nothing accumulated or paused in
        // one mode carries into the other. Tied to the setting, not to the
        // effective mono state: that also flips whenever a grip is held, and
        // rebuilding there would re-sync the scene on every grip. A new
        // delegate or stage this frame has rebuilt the engines already.
        const bool stereoSetting = _stereo.load();
        const bool restartRender =
            stereoSetting != lastStereo && !interactive && !rendererSwitched && !applyStage;
        lastStereo = stereoSetting;
        if (restartRender) {
            renderer.Restart();
            std::printf("HxrRuntime: Stereo %s -- render restarted\n",
                        stereoSetting ? "on" : "off");
        }

        if (_resyncRequested.exchange(false)) {
            anchorLatched    = false;
            anchorFromCamera = false;
            snapPath         = SdfPath();   // the next left-stick click starts over
            // Resync means "back to the camera": drop movement and orbit too.
            userXform   = GfMatrix4d(1.0);
            orbitLive   = GfMatrix4d(1.0);
            gripWasHeld = false;
        }

        // --- Left stick click: snap to the next camera or light ---
        // Re-anchors exactly as the RenderSettings camera does -- the prim's
        // position at the head, its heading as the head's, floor kept level
        // -- and, like Resync, drops movement and orbit so the view is the
        // prim's. The list is rebuilt from the current snapshot on every
        // click, so upstream edits are picked up; the next entry is the one
        // after the last snapped-to path, or the first if that's gone.
        if (xr.LeftThumbstickPressed() && haveLiveHead && renderer.Stage()) {
            const std::vector<StageView> views = FindStageViews(renderer.Stage());
            size_t next = 0;
            for (size_t i = 0; i < views.size(); ++i) {
                if (views[i].path == snapPath) {
                    next = (i + 1) % views.size();
                    break;
                }
            }
            GfMatrix4d stageFromView;
            if (views.empty()) {
                std::printf("HxrRuntime: no cameras or lights on the stage to snap to\n");
            } else if (FindPrimTransform(renderer.Stage(), views[next].path,
                                         UsdTimeCode(_timeCode.load()), &stageFromView)) {
                snapPath         = views[next].path;
                stageFromCamera  = stageFromView;
                anchorHead       = liveHead;
                anchorFromCamera = true;
                anchorLatched    = true;
                userXform        = GfMatrix4d(1.0);
                orbitLive        = GfMatrix4d(1.0);
                gripWasHeld      = false;
                std::printf("HxrRuntime: snapped to %s %s (%zu of %zu)\n",
                            views[next].isLight ? "light" : "camera",
                            snapPath.GetText(), next + 1, views.size());
            }
        }

        if (!xr.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            lastFrameTime = std::chrono::steady_clock::now();
            continue;
        }

        {
            const auto   frameNow = std::chrono::steady_clock::now();
            const double dt = std::min(
                kMaxFrameDt, std::chrono::duration<double>(frameNow - lastFrameTime).count());
            lastFrameTime = frameNow;

            // The head's own axes, pitch and all: forward/back dollies along
            // the line of sight, left/right strafes across it, and A/B move
            // along the head's up. Unlike the anchor, which keeps the floor
            // level, this is free flight -- look down and push forward to
            // descend. Rigid pose, so the axes are unit length.
            const GfVec3d forward = liveHeadToWorld.TransformDir(GfVec3d(0.0, 0.0, -1.0));
            const GfVec3d right   = liveHeadToWorld.TransformDir(GfVec3d(1.0, 0.0, 0.0));
            const GfVec3d up      = liveHeadToWorld.TransformDir(GfVec3d(0.0, 1.0, 0.0));

            GfVec3d velocity(0.0);
            const XrVector2f stick = xr.RightThumbstick();
            GfVec2d deflection(stick.x, stick.y);
            const double magnitude = deflection.GetLength();
            if (magnitude > kStickDeadzone) {
                // Rescale so movement starts from zero at the deadzone edge
                // rather than jumping to 15% speed.
                deflection *= (magnitude - kStickDeadzone) / ((1.0 - kStickDeadzone) * magnitude);
                velocity += forward * deflection[1] + right * deflection[0];
            }
            velocity += up * (double(xr.ButtonB()) - double(xr.ButtonA()));

            if (haveLiveHead && velocity != GfVec3d(0.0)) {
                userXform = userXform * Translation(-velocity * double(_moveSpeed.load()) * dt);
            }
        }

        // The anchor itself is latched inside the render callback, where the
        // head pose is available. Anchor Distance/Height are read live in
        // here, so those sliders still take effect without a resync.
        const GfMatrix4d base = computeWorldFromStage();

        // Head centre: midpoint of the eyes, with eye 0's orientation (the
        // eyes are parallel). Aiming from one eye would put near hits a
        // couple of degrees off the true line of sight.
        GfMatrix4d headCentreToWorld = liveHeadToWorld;
        headCentreToWorld.SetTranslateOnly(haveEye1 ? (eyePos[0] + eyePos[1]) * 0.5 : eyePos[0]);
        const GfVec3d headCentre  = headCentreToWorld.ExtractTranslation();
        const GfVec3d headForward = headCentreToWorld.TransformDir(GfVec3d(0.0, 0.0, -1.0));

        // A pick camera at the head looking down the gaze; Hydra's view
        // matrix is stage -> eye, i.e. stage -> room -> head.
        auto pickAlongGaze = [&](GfMatrix4d const& worldFromStage_, GfVec3d* hitStage) {
            GfFrustum frustum;
            frustum.SetPerspective(kPickFovDegrees, 1.0, kNearPlane, kFarPlane);
            return renderer.Pick(worldFromStage_ * headCentreToWorld.GetInverse(),
                                 frustum.ComputeProjectionMatrix(), _timeCode.load(), hitStage);
        };

        // --- Snap turn ---
        // Appended to userXform like locomotion. Turning the user right is
        // turning the stage the other way about them, which is +yaw here
        // (+Y rotation carries what's ahead off to the left). Pivoting on the
        // head centre's vertical axis turns the view in place, with no
        // sideways lurch. Not while orbiting: the orbit's pivot was latched
        // in room space, and turning the stage underneath it would move its
        // surface point away -- the flick is consumed but ignored.
        {
            const double turnX = xr.LeftThumbstick().x;
            if (std::abs(turnX) < kSnapRearm) {
                snapArmed = true;
            } else if (snapArmed && std::abs(turnX) > kSnapFire) {
                snapArmed = false;
                const double degrees = double(_snapTurnDegrees.load());
                if (degrees > 0.0 && !gripHeld && haveLiveHead) {
                    GfMatrix4d turn;
                    turn.SetRotate(GfRotation(GfVec3d(0.0, 1.0, 0.0),
                                              turnX > 0.0 ? degrees : -degrees));
                    const GfVec3d pivot(headCentre[0], 0.0, headCentre[2]);
                    userXform = userXform * Translation(-pivot) * turn * Translation(pivot);
                }
            }
        }

        // --- Desktop panel ---
        if (xr.LeftMenuPressed() && haveLiveHead) {
            panelOpen = !panelOpen;
            if (panelOpen && !desktop.IsValid() && !desktop.Init()) {
                panelOpen = false;   // DesktopCapture has said why, on stderr
            }
            if (panelOpen) {
                const GfVec3d forward = Horizontal(headForward);
                const GfVec3d centre  = headCentre + forward * kPanelDistance -
                                        GfVec3d(0.0, kPanelDrop, 0.0);
                // Facing the user: the +Y rotation taking -Z to `forward`
                // (the quad's image faces its local +Z).
                const double yaw = std::atan2(-forward[0], -forward[2]);
                panelPose.position    = {float(centre[0]), float(centre[1]), float(centre[2])};
                panelPose.orientation = {0.0f, float(std::sin(0.5 * yaw)), 0.0f,
                                         float(std::cos(0.5 * yaw))};
                panelFresh = true;
            }
        }
        {
            XrViewportSession::Panel panel;
            if (panelOpen) {
                const bool changed = desktop.Update() || panelFresh;
                panelFresh = false;

                int width  = 0;
                int height = 0;
                desktop.PanelSize(&width, &height);
                panel.visible     = true;
                panel.width       = uint32_t(width);
                panel.height      = uint32_t(height);
                panel.pose        = panelPose;
                panel.widthMetres = _panelWidth.load();
                panel.changed     = changed;
                panel.draw        = [&desktop](int w, int h) { desktop.BlitTo(w, h); };
            }
            xr.SetPanel(std::move(panel));
        }

        // --- Toggles: X placement/Renderer, Y stereo, left trigger play ---
        // Toggles, not holds: a progressive render is something to leave
        // running and look around in. Flipped where each setting lives (the
        // node's parms, Houdini's playbar, via the callback) so the UI agrees
        // with the headset.
        auto issue = [&](Command command) {
            CommandCallback callback;
            {
                std::lock_guard<std::mutex> lock(_callbackMutex);
                callback = _commandCallback;
            }
            if (callback) {
                callback(command);
            } else if (command == Command::ToggleInteractive) {
                _interactive = !_interactive.load();
            } else if (command == Command::ToggleStereo) {
                _stereo = !_stereo.load();
            }
        };
        if (xr.ButtonXPressed()) {
            issue(Command::ToggleInteractive);
        }
        if (xr.ButtonYPressed()) {
            issue(Command::ToggleStereo);
        }
        if (xr.LeftTriggerPressed()) {
            issue(Command::TogglePlayback);
        }

        // --- Grip orbit ---
        XrPosef    aimPose{};
        const bool aimValid = xr.RightAimPose(&aimPose);

        if (gripHeld && !gripWasHeld && haveLiveHead && anchorLatched && aimValid) {
            // Grip start. A fresh pick rather than the reticle's, which can be
            // up to a pick interval stale. Nothing under the reticle means
            // orbiting about where the reticle is drawn anyway.
            const GfMatrix4d settled = base * userXform;
            GfVec3d hitStage;
            orbitPivotRoom = pickAlongGaze(settled, &hitStage)
                                 ? settled.Transform(hitStage)
                                 : headCentre + headForward * kReticleMissDistance;
            orbitStartOrientation = aimPose.orientation;
            const GfVec3d forward = Horizontal(headForward);
            orbitRight            = GfVec3d(-forward[2], 0.0, forward[0]);
            gripWasHeld           = true;
        }
        if (gripHeld && gripWasHeld && aimValid) {
            // The hand's rotation since the grip began, reshaped (yaw gain,
            // optionally no roll), applied to the scene about the pivot. The
            // pitch axis is the user's right *at grip start*, so turning the
            // head mid-orbit doesn't change what the hand does.
            const GfMatrix4d delta =
                OrbitRotation(orbitStartOrientation, aimPose.orientation,
                              double(_orbitYawGain.load()), _orbitYUp.load(), orbitRight);
            orbitLive = Translation(-orbitPivotRoom) * delta * Translation(orbitPivotRoom);
        }
        if (!gripHeld && gripWasHeld) {
            userXform   = userXform * orbitLive;   // commit
            orbitLive   = GfMatrix4d(1.0);
            gripWasHeld = false;
            // The reticle was pinned to the pivot; re-pick the gaze now
            // rather than show the pre-orbit hit for up to a pick interval.
            lastPickTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        }

        // After the commit, so the release frame doesn't flash the pre-orbit
        // placement.
        GfMatrix4d worldFromStage = base * userXform * orbitLive;

        // --- Reticle ---
        // Pinned to the pivot while orbiting, rather than following the gaze:
        // the pivot is the point everything turns about, and a reticle that
        // wandered off it as the head moved was misleading. The pivot is
        // fixed in room space by construction (orbitLive rotates about it),
        // so no pick is needed until release.
        const bool showReticle = _showReticle.load() && haveLiveHead && anchorLatched;
        if (showReticle && !gripWasHeld) {
            const auto pickNow = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(pickNow - lastPickTime).count() >= kPickInterval) {
                lastPickTime     = pickNow;
                reticleOnSurface = pickAlongGaze(worldFromStage, &reticleStage);
            }
        }
        const GfVec3d reticleRoom = gripWasHeld      ? orbitPivotRoom
                                    : reticleOnSurface ? worldFromStage.Transform(reticleStage)
                                                       : headCentre + headForward * kReticleMissDistance;

        // Thumbstick click: report where the head is, in stage space, to
        // whoever wants to place a camera there. Uses the previous frame's
        // pose, like locomotion does. Row-vector: head-local -> world ->
        // stage, so the result is what a camera's local-to-world would be.
        if (xr.RightThumbstickPressed() && haveLiveHead && anchorLatched) {
            PlacementCallback callback;
            {
                std::lock_guard<std::mutex> lock(_callbackMutex);
                callback = _placementCallback;
            }
            if (callback) {
                callback(CameraPlacement{liveHeadToWorld * worldFromStage.GetInverse(),
                                         _timeCode.load()});
            }
        }

        // --- Playbar scrub ---
        {
            XrPosef    leftAim{};
            const bool leftAimValid = xr.LeftAimPose(&leftAim);
            if (!leftGripHeld) {
                scrubHeld = false;
            } else if (!scrubHeld && leftAimValid) {
                scrubHeld             = true;
                scrubMoved            = false;
                scrubStartOrientation = leftAim.orientation;
            }

            const double rate = double(_scrubRate.load());
            if (scrubHeld && leftAimValid && rate > 0.0) {
                if (!scrubMoved) {
                    scrubStartFrame = _timeCode.load();
                }
                // Positive twist about the controller's +Z plays forward --
                // clockwise, as the user sees it (confirmed in the headset;
                // the opposite sign ran backwards).
                const double quarterTurns =
                    TwistAboutLocalZ(scrubStartOrientation, leftAim.orientation) / (0.5 * M_PI);
                const double frame = std::round(scrubStartFrame + quarterTurns * rate);
                if (scrubMoved ? frame != scrubLastFrame : frame != std::round(scrubStartFrame)) {
                    scrubMoved     = true;
                    scrubLastFrame = frame;
                    ScrubCallback callback;
                    {
                        std::lock_guard<std::mutex> lock(_callbackMutex);
                        callback = _scrubCallback;
                    }
                    if (callback) {
                        callback(frame);
                    }
                }
            }
        }

        const double timeCode = _timeCode.load();

        // Cheap to re-derive every frame (a few atomics), and SetRenderSize is
        // a no-op when unchanged -- so the caps can be adjusted live. Each
        // renderer-specific cap binds only while its delegate is rendering.
        GfVec2i renderBase = eyeSize;
        if (mono) {
            // From an earlier frame's views: this frame's aren't located
            // until RenderFrame, and the render size has to be set before it.
            if (!monoScaleLatched) {
                monoScaleLatched = MonoScale(xr, &monoScale);
            }
            renderBase = GfVec2i(int(eyeSize[0] * monoScale[0] + 0.5),
                                 int(eyeSize[1] * monoScale[1] + 0.5));
        }
        GfVec2i renderSize = FitWithin(renderBase, _maxRenderWidth.load(), _maxRenderHeight.load());
        renderSize = interactive
                         ? FitWithin(renderSize, _interactiveMaxWidth.load(),
                                     _interactiveMaxHeight.load())
                         : FitWithin(renderSize, _rendererMaxWidth.load(),
                                     _rendererMaxHeight.load());
        renderer.SetRenderSize(renderSize);

        if (monoSwitched && mono) {
            for (int i = 1; i < viewCount; ++i) {
                renderer.PauseView(i);   // resumed by its next RenderEye
            }
        }

        const bool contentChanged = applyStage || rendererSwitched || monoSwitched ||
                                    restartRender ||
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
        const auto now = std::chrono::steady_clock::now();

        // Freezing captures the pose at the moment it takes effect -- whether
        // from the toggle (or X), or from a grip being released with Freeze
        // Pose set -- rather than keeping whatever was last held.
        const bool froze = frozen && !lastFrozen;
        lastFrozen = frozen;

        // Only a decision here -- the capture itself happens in the callback,
        // and so does bookkeeping about it. RenderFrame may not call the
        // callback at all on a given frame (the runtime says not to render,
        // or the views aren't valid yet, both routine at session start), and
        // recording "held" before the pose was actually taken is how a zero
        // pose ends up in xrEndFrame.
        // Switching between stereo and single-eye re-captures too: the held
        // poses are per-eye in one and the cyclopean view in the other.
        bool recapture = _refreezeRequested.exchange(false) || froze || monoSwitched;
        if (!recapture && !frozen) {
            bool allConverged = true;
            for (size_t i = 0; i < (mono ? size_t(1) : held.size()); ++i) {
                allConverged = allConverged && held[i].converged;
            }
            const double heldFor =
                std::chrono::duration<double>(now - heldSince).count();
            recapture = allConverged || convergeSeconds <= 0.0f ||
                        heldFor >= double(convergeSeconds);
        }

        auto captureEye = [&](HeldEye& eye, XrView const& source) {
            eye.pose      = source.pose;
            eye.fov       = source.fov;
            eye.view      = XrPoseToViewMatrix(source.pose);
            eye.proj      = XrFovToProjectionMatrix(source.fov, kNearPlane, kFarPlane);
            eye.held      = true;
            eye.converged = false;
            heldSince     = now;
        };

        // A frozen, converged eye has nothing left to render: keep presenting
        // the frame it already has.
        auto renderHeldEye = [&](uint32_t index) {
            HeldEye& eye = held[index];
            if (frozen && eye.converged) {
                return;
            }
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
        };

        // Reticle on a copy, every frame: a frozen, converged eye reuses its
        // image but the gaze still moves. Projected through the eye's *held*
        // view, so it lines up with the geometry in that image; the
        // compositor then reprojects both together to the live head pose.
        auto withReticle = [&](HeldEye const& eye) {
            XrViewportSession::EyeImage image = eye.image;
            if (showReticle) {
                const GfVec4d clip =
                    GfVec4d(reticleRoom[0], reticleRoom[1], reticleRoom[2], 1.0) *
                    (eye.view * eye.proj);
                if (clip[3] > 1e-6) {   // in front of this eye
                    image.reticleVisible = true;
                    image.reticleNdcX    = float(clip[0] / clip[3]);
                    image.reticleNdcY    = float(clip[1] / clip[3]);
                }
            }
            return image;
        };

        // --- Single-eye: one render, on a billboard ---
        // Rendered here, before RenderFrame, from the views it located last
        // frame: the billboard is fixed in the room, so a capture pose one
        // frame old is invisible, and its placement is then known before the
        // frame is submitted. Shown on a flat quad standing where the
        // captured view looked, sized to exactly fill that view -- from the
        // capture point it lines up with the scene, and as the head moves it
        // behaves like a picture in the room, not one at infinity glued to
        // the eyes.
        XrViewportSession::Billboard billboard;
        if (mono) {
            HeldEye& eye = held[0];
            if (recapture || !eye.held) {
                captureEye(eye, CyclopeanView(xr));

                billboardDistance = kBillboardFallback;
                GfVec3d hitStage;
                if (pickAlongGaze(worldFromStage, &hitStage)) {
                    const double depth =
                        GfDot(worldFromStage.Transform(hitStage) - headCentre, headForward);
                    if (depth > 0.0) {
                        billboardDistance = std::clamp(depth, kBillboardMin, kBillboardMax);
                    }
                }
            }
            renderHeldEye(0);

            // The asymmetric frustum's window at the billboard's depth,
            // centred where it falls rather than on the view axis.
            const double d  = billboardDistance;
            const double l  = std::tan(double(eye.fov.angleLeft));
            const double r  = std::tan(double(eye.fov.angleRight));
            const double dn = std::tan(double(eye.fov.angleDown));
            const double up = std::tan(double(eye.fov.angleUp));
            const GfVec3d centre = XrPoseToMatrix(eye.pose).Transform(
                GfVec3d(0.5 * (l + r) * d, 0.5 * (dn + up) * d, -d));

            billboard.visible              = true;
            billboard.image                = withReticle(eye);
            billboard.pose.orientation     = eye.pose.orientation;
            billboard.pose.position        = {float(centre[0]), float(centre[1]), float(centre[2])};
            billboard.size                 = {float((r - l) * d), float((up - dn) * d)};
        }
        xr.SetBillboard(billboard);

        auto renderEye = [&](uint32_t index, XrView const& current) {
            if (index < 2) {
                eyePos[index] = XrPoseToMatrix(current.pose).ExtractTranslation();
                haveEye1      = haveEye1 || index == 1;
            }
            if (index == 0) {
                liveHeadToWorld = XrPoseToMatrix(current.pose);
                liveHead        = HeadFrameFromPose(current.pose);
                haveLiveHead    = true;
            }

            // Latch the anchor from the first eye's pose. Eye 0 is ~3cm off
            // the head centre, which is well below anything noticeable here.
            if (index == 0 && !anchorLatched && renderer.Stage()) {
                anchorHead       = liveHead;
                anchorFromCamera = FindRenderCameraTransform(
                    renderer.Stage(), UsdTimeCode(timeCode), &stageFromCamera);
                anchorLatched = true;
                worldFromStage = computeWorldFromStage() * userXform * orbitLive;
                std::printf("HxrRuntime: anchor latched %s, head at (%.2f, %.2f, %.2f)\n",
                            anchorFromCamera ? "to RenderSettings camera"
                                             : "to Anchor Distance/Height",
                            anchorHead.position[0], anchorHead.position[1],
                            anchorHead.position[2]);
            }

            // Single-eye already rendered above, onto the billboard; the
            // session doesn't present this (it isn't submitted as a
            // projection layer), so only the bookkeeping above matters.
            if (mono) {
                return withReticle(held[0]);
            }

            // An eye that has never captured a pose has nothing valid to
            // submit, whatever the frame-level decision was.
            HeldEye& eye = held[index];
            if (recapture || !eye.held) {
                captureEye(eye, current);
            }
            renderHeldEye(index);
            return withReticle(eye);
        };

        if (!xr.RenderFrame(renderEye)) {
            break;
        }
        ++_framesPresented;
    }

    xr.Shutdown();
    _running = false;
}
