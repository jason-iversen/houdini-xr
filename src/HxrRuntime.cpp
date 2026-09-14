#include "HxrRuntime.h"

#include "GLContext.h"
#include "RenderCamera.h"
#include "StormRenderer.h"
#include "XrSession.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/imaging/garch/glApi.h>

#include <chrono>
#include <cstdio>

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

    // Stop() destroys the render thread's StormRenderer along with the stage
    // it held, but _pendingStage outlives it. Re-arm it so a restarted thread
    // re-applies the stage it already has: the caller may legitimately not
    // hand us a new one (nothing changed while we were stopped), and without
    // this the fresh thread would come up with no stage at all and render
    // black -- which is exactly what toggling Live off and on used to do.
    {
        std::lock_guard<std::mutex> lock(_stageMutex);
        if (_pendingStage) {
            _stageDirty = true;
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

    StormRenderer renderer;
    if (!renderer.Init(GfVec2i(int(xr.EyeWidth()), int(xr.EyeHeight())))) {
        xr.Shutdown();
        _failed = true;
        return;
    }

    _running = true;

    // Latched once per session (per call to Start()), not re-evaluated every
    // frame: continuously following the render camera would drag the user's
    // whole reference frame around on every animated camera move, which
    // defeats free look-around. "Initial position" means exactly that --
    // toggling Live off and back on is how you reset to the camera again.
    bool anchorLatched    = false;
    bool anchorFromCamera = false;
    GfMatrix4d lockedWorldFromStage(1.0);

    while (!_stopRequested.load() && xr.PollEvents()) {
        {
            std::lock_guard<std::mutex> lock(_stageMutex);
            if (_stageDirty) {
                renderer.SetStage(_pendingStage);
                _stageDirty = false;
            }
        }

        if (_resyncRequested.exchange(false)) {
            anchorLatched    = false;
            anchorFromCamera = false;
        }

        if (!anchorLatched && renderer.Stage()) {
            GfMatrix4d stageFromCamera;
            if (FindRenderCameraTransform(renderer.Stage(), UsdTimeCode(_timeCode.load()),
                                          &stageFromCamera)) {
                // Placing the camera's own stage-space position/orientation at
                // the reference-space origin is the same inverse-pose math as
                // XrPoseToViewMatrix -- standing at the room's calibrated
                // origin facing its native -Z now shows what the camera saw.
                lockedWorldFromStage = stageFromCamera.GetInverse();
                anchorFromCamera = true;
                std::printf("HxrRuntime: initial anchor from RenderSettings camera\n");
            }
            anchorLatched = true;
        }

        if (!xr.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // STAGE reference space puts (0,0,0) at floor level at the room's
        // calibrated center; this fallback offset is what keeps content in
        // front of the user instead of centered underfoot when there's no
        // render camera to anchor to. Re-read every frame since SetAnchor()
        // can change it live without a new stage.
        GfMatrix4d worldFromStage = lockedWorldFromStage;
        if (!anchorFromCamera) {
            worldFromStage.SetTranslate(GfVec3d(
                0.0, double(_anchorHeight.load()), -double(_anchorDist.load())));
        }

        const double timeCode = _timeCode.load();

        auto renderEye = [&renderer, worldFromStage, timeCode](uint32_t, GfMatrix4d const& view,
                                                                GfMatrix4d const& proj) {
            renderer.RenderEye(worldFromStage * view, proj, timeCode);
            return renderer.ColorTextureId();
        };

        if (!xr.RenderFrame(renderEye)) {
            break;
        }
        ++_framesPresented;
    }

    xr.Shutdown();
    _running = false;
}
