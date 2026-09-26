#pragma once

#include "XrPlatform.h"
#include "XrPresenterGL.h"

#include <pxr/base/gf/matrix4d.h>

#include <cstdint>
#include <functional>
#include <vector>

class GLContext;

PXR_NAMESPACE_USING_DIRECTIVE

class XrViewportSession
{
public:
    struct EyeImage
    {
        uint32_t texture = 0;
        int      width   = 0;   // may be smaller than the swapchain; upscaled on present
        int      height  = 0;
        XrPosef  pose{};        // the pose this image was actually rendered from
        XrFovf   fov{};

        // Reticle position in this eye's NDC (-1..1, +y up). Computed by
        // projecting one 3D point into each eye rather than taken as each
        // image's centre: Quest's per-eye FOVs are asymmetric, so the image
        // centres point in different directions and a centred reticle would
        // split in two.
        bool  reticleVisible = false;
        float reticleNdcX    = 0.0f;
        float reticleNdcY    = 0.0f;
    };

    // Renders one eye given its current located view. The returned pose/fov
    // are what get submitted with the image -- they need not match `current`.
    // A frame held from an earlier pose is reprojected by the compositor to
    // wherever the head is now, which is what keeps a converging (or frozen)
    // image spatially stable while the user looks around.
    using RenderEyeFn = std::function<EyeImage(uint32_t view, XrView const& current)>;

    // The OpenXR loader permits exactly one XrInstance per process, so a
    // leaked instance from a failed Init() -- headset not ready, runtime
    // mid-restart -- would make every later attempt fail with
    // XR_ERROR_LIMIT_REACHED until the host process restarts. Both a failed
    // Init() and destruction tear down whatever was created.
    ~XrViewportSession() { Shutdown(); }

    bool Init(GLContext const& gl);
    void Shutdown();

    // False once the runtime has asked us to quit.
    bool PollEvents();

    // True between xrBeginSession and xrEndSession; frames only run while set.
    bool IsRunning() const { return _running; }

    bool RenderFrame(RenderEyeFn const& renderEye);

    uint32_t EyeWidth()  const { return _eyeWidth; }
    uint32_t EyeHeight() const { return _eyeHeight; }
    uint32_t ViewCount() const { return uint32_t(_viewConfigs.size()); }

    // Either hand's trigger, 0..1, as of the most recent RenderFrame. One
    // action bound to both hands: OpenXR resolves that to whichever is pulled
    // further, so no per-hand bookkeeping is needed. Reads 0 whenever input
    // isn't active (session not focused, controllers asleep).
    float TriggerValue() const { return _triggerValue; }

    // Right-hand thumbstick, each axis -1..1, +y pushed away from the user.
    // Zero whenever input isn't active.
    XrVector2f RightThumbstick() const { return _rightThumbstick; }

    // True only on the frame the right thumbstick is clicked down.
    bool RightThumbstickPressed() const { return _rightThumbstickPressed; }

    // Right grip (squeeze), 0..1.
    float GripValue() const { return _gripValue; }

    // Right controller's aim pose in the reference space, located at the
    // previous frame's predicted display time. False if not tracked.
    bool RightAimPose(XrPosef* pose) const
    {
        *pose = _rightAimPose;
        return _rightAimValid;
    }

private:
    bool _InitImpl(GLContext const& gl);
    bool _InitInput();
    void _SyncInput();
    void _LocateControllers(XrTime time);

    XrInstance _instance = XR_NULL_HANDLE;
    XrSystemId _systemId = XR_NULL_SYSTEM_ID;
    XrSession  _session  = XR_NULL_HANDLE;
    XrSpace    _space    = XR_NULL_HANDLE;

    XrPresenterGL _presenter;

    std::vector<XrViewConfigurationView> _viewConfigs;
    std::vector<XrView> _views;

    uint32_t _eyeWidth  = 0;
    uint32_t _eyeHeight = 0;

    XrActionSet _actionSet        = XR_NULL_HANDLE;
    XrAction    _triggerAction    = XR_NULL_HANDLE;
    XrAction    _thumbstickAction = XR_NULL_HANDLE;
    XrAction    _thumbClickAction = XR_NULL_HANDLE;
    XrAction    _gripAction       = XR_NULL_HANDLE;
    XrAction    _aimPoseAction    = XR_NULL_HANDLE;
    XrSpace     _rightAimSpace    = XR_NULL_HANDLE;
    float       _triggerValue     = 0.0f;
    float       _gripValue        = 0.0f;
    XrVector2f  _rightThumbstick{0.0f, 0.0f};
    bool        _rightThumbstickPressed = false;
    XrPosef     _rightAimPose{};
    bool        _rightAimValid = false;
    bool     _running   = false;
    bool     _quit      = false;
};
