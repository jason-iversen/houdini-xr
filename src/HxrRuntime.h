#pragma once

#include <pxr/usd/usd/stage.h>

#include <atomic>
#include <mutex>
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

    std::mutex     _stageMutex;
    UsdStageRefPtr _pendingStage;
    bool           _stageDirty = false;
};
