#pragma once

#include <pxr/usd/usd/stage.h>

#include <pxr/base/gf/matrix4d.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

PXR_NAMESPACE_USING_DIRECTIVE

// Owns one OpenXR session + Storm renderer running continuously on its own
// thread, at headset refresh rate, independent of whatever cadence supplies
// new stage content.
//
// Start()/Stop()/SetStage()/SetAnchor() only ever touch a mutex or atomics
// and return immediately -- they never open a session, create a GL context,
// or block on the render loop. That makes them safe to call from a Houdini
// LOP node's cookMyLop(), which runs on Houdini's main/cook thread and must
// not stall waiting on headset IO.
class HxrRuntime
{
public:
    ~HxrRuntime();

    void Start();
    void Stop();

    // Safe from any thread. The render thread picks up the latest values;
    // it does not need a new stage or a call to Start() to see a new anchor.
    void SetStage(UsdStageRefPtr const& stage);
    void SetAnchor(float distMeters, float heightMeters);

    // Hydra render delegate plugin id (e.g. HdStormRendererPlugin,
    // BRAY_HdKarma). Empty means the default. Takes effect on the next frame,
    // rebuilding the engine if a stage is already loaded.
    void SetRendererPlugin(std::string const& pluginId);

    // Caps the per-eye render size, preserving aspect; the result is upscaled
    // to the swapchain on present. 0 in either dimension means uncapped. This
    // is how a progressive delegate is made responsive enough to be usable.
    void SetMaxRenderSize(int maxWidth, int maxHeight);

    // A further cap that applies only while the *configured* delegate is the
    // one rendering -- a licence limit on that delegate (Apprentice caps Karma
    // at 1280x720). Interactive placement runs Storm, which it doesn't bind.
    void SetRendererMaxSize(int maxWidth, int maxHeight);

    // Interactive Placement: render a live Storm view regardless of what's
    // configured, so the viewpoint can be placed at full speed. The effective
    // state is this toggle OR a held controller trigger, resolved on the
    // render thread each frame -- a trigger routed back through a Houdini
    // cook would lag. Turning it off hands the exact same pose to the
    // configured delegate: if that's frozen, the freeze captures right there.
    void SetInteractive(bool on) { _interactive = on; }

    // Right-thumbstick locomotion speed at full deflection, metres per
    // second. Movement is relative to the head's horizontal heading.
    void SetMoveSpeed(float metresPerSecond) { _moveSpeed = metresPerSecond; }

    // The gaze reticle. It sits on the surface under the line of sight, and
    // marks the pivot a right-grip orbit will turn about.
    void SetShowReticle(bool show) { _showReticle = show; }

    // Invoked, ON THE RENDER THREAD, when the right thumbstick is clicked:
    // the head's full pose (position and orientation, pitch included) in
    // *stage* coordinates -- i.e. the transform a camera prim would need to
    // see exactly what the user sees. The callee owns marshalling this to
    // wherever it can act on it; this class knows nothing about Houdini.
    struct CameraPlacement
    {
        GfMatrix4d cameraToStage;
        double     timeCode = 0.0;
    };
    using PlacementCallback = std::function<void(CameraPlacement const&)>;
    void SetPlacementCallback(PlacementCallback callback);

    // How the head pose used for rendering relates to the live one.
    //
    // A progressive delegate restarts accumulation on every camera change, so
    // rendering at the live pose each frame means it never gets past its
    // first, noisiest sample. Instead the pose is *held*: rendering continues
    // at the held pose while the compositor reprojects the improving image to
    // wherever the head actually is. The held frame stays spatially stable --
    // it looks like a picture fixed in space, not stuck to the face.
    //
    //   converge seconds > 0 : hold until converged or the time is up, then
    //                          re-capture the live pose and go again.
    //   frozen               : hold indefinitely -- render to convergence and
    //                          keep showing it. Refreeze re-captures.
    //
    // Storm converges in one pass, so with either setting it re-captures every
    // frame and behaves exactly as a live viewport.
    void SetConvergeSeconds(float seconds) { _convergeSeconds = seconds; }
    void SetFrozen(bool frozen) { _frozen = frozen; }
    void RequestRefreeze() { _refreezeRequested = true; }

    // USD time code to render at -- by convention in Houdini/HUSD-authored
    // stages, 1 time-code unit == 1 Houdini frame (OP_Context::getFloatFrame()).
    void SetTimeCode(double timeCode) { _timeCode = timeCode; }

    // Re-attempts the RenderSettings-camera anchor lookup on the next frame,
    // using whatever stage/time are current at that point. Without this, the
    // one-time latch at session start is the only way the anchor gets set --
    // this lets the user re-snap to the camera later without toggling Live
    // off and back on (which would also tear down and restart the session).
    void RequestResyncCamera() { _resyncRequested = true; }

    bool IsRunning() const { return _running.load(); }
    bool HasFailed() const { return _failed.load(); }
    int  FramesPresented() const { return _framesPresented.load(); }

private:
    void ThreadMain();

    std::thread       _thread;
    std::atomic<bool> _stopRequested{false};
    std::atomic<bool> _running{false};
    std::atomic<bool> _failed{false};
    std::atomic<int>  _framesPresented{0};

    std::atomic<float>  _anchorDist{2.0f};
    std::atomic<float>  _anchorHeight{1.2f};
    std::atomic<double> _timeCode{0.0};
    std::atomic<bool>   _resyncRequested{false};
    std::atomic<int>    _maxRenderWidth{0};
    std::atomic<int>    _maxRenderHeight{0};
    std::atomic<int>    _rendererMaxWidth{0};
    std::atomic<int>    _rendererMaxHeight{0};
    std::atomic<float>  _convergeSeconds{1.0f};
    std::atomic<bool>   _frozen{false};
    std::atomic<bool>   _interactive{false};
    std::atomic<bool>   _refreezeRequested{false};
    std::atomic<float>  _moveSpeed{1.5f};
    std::atomic<bool>   _showReticle{true};

    std::mutex     _stageMutex;
    UsdStageRefPtr _pendingStage;
    bool           _stageDirty = false;
    std::string    _pendingRenderer;
    bool           _rendererDirty = false;

    std::mutex        _callbackMutex;
    PlacementCallback _placementCallback;
};
