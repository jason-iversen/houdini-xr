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

    // A flat panel composited over the scene as its own quad layer -- the
    // desktop mirror. World-locked in the reference space. Set before
    // RenderFrame; `draw` is called during it, only when `changed` (or the
    // panel's swapchain is new), to fill the bound draw framebuffer. An
    // unchanged panel costs nothing: the compositor keeps showing the last
    // image released into its swapchain.
    struct Panel
    {
        bool     visible = false;
        uint32_t width   = 0;       // image size in pixels, for the swapchain
        uint32_t height  = 0;
        XrPosef  pose{};            // centre; the image faces +Z
        float    widthMetres = 1.0f;
        bool     changed = false;
        std::function<void(int width, int height)> draw;
    };
    // Single-eye rendering's picture, shown as a flat billboard fixed in the
    // room rather than fed to both eyes as a projection layer -- which puts
    // it at infinity with nothing moving as the head does, and is
    // uncomfortable. While visible, the projection layer isn't submitted:
    // the render callbacks still run (the caller does its per-frame
    // bookkeeping there), but their images aren't presented. Redrawn every
    // frame, since the image converges and the reticle moves.
    struct Billboard
    {
        bool        visible = false;
        EyeImage    image;          // texture, size and reticle; pose/fov unused
        XrPosef     pose{};         // centre; the image faces +Z
        XrExtent2Df size{};         // metres
    };
    void SetBillboard(Billboard const& billboard) { _billboard = billboard; }

    void SetPanel(Panel panel)
    {
        // Latched until actually drawn: a change on a frame the runtime says
        // not to render must not be lost.
        _panelDirty = _panelDirty || panel.changed;
        _panel      = std::move(panel);
    }

    uint32_t EyeWidth()  const { return _eyeWidth; }
    uint32_t EyeHeight() const { return _eyeHeight; }
    uint32_t ViewCount() const { return uint32_t(_viewConfigs.size()); }

    // The views as most recently located -- during a RenderFrame callback,
    // this frame's, for every eye (all are located before the first
    // callback). Zeroed until the first frame locates them.
    XrView const& LocatedView(uint32_t view) const { return _views[view]; }

    // Either hand's trigger, 0..1, as of the most recent RenderFrame. One
    // action bound to both hands: OpenXR resolves that to whichever is pulled
    // further. Reads 0 whenever input isn't active (session not focused,
    // controllers asleep).
    float TriggerValue() const { return _triggerValue; }

    // True only on the frame the left trigger is pulled -- an edge taken
    // from its analogue value with hysteresis (fires past 3/4 travel,
    // re-arms below 1/4), since Touch triggers have no click.
    bool LeftTriggerPressed() const { return _leftTriggerPressed; }

    // Right-hand thumbstick, each axis -1..1, +y pushed away from the user.
    // Zero whenever input isn't active.
    XrVector2f RightThumbstick() const { return _rightThumbstick; }

    // Left-hand thumbstick, same convention. Only its x axis is used (snap
    // turn); the right stick already has move and strafe.
    XrVector2f LeftThumbstick() const { return _leftThumbstick; }

    // Right controller's A and B buttons, held (level, not edge): vertical
    // movement.
    bool ButtonA() const { return _buttonA; }
    bool ButtonB() const { return _buttonB; }

    // True only on the frame the left controller's X / Y button is pressed.
    bool ButtonXPressed() const { return _buttonXPressed; }
    bool ButtonYPressed() const { return _buttonYPressed; }

    // True only on the frame the left menu button is pressed.
    bool LeftMenuPressed() const { return _leftMenuPressed; }

    // True only on the frame the right thumbstick is clicked down.
    bool RightThumbstickPressed() const { return _rightThumbstickPressed; }

    // Grip (squeeze), 0..1: the right grip orbits, the left scrubs the
    // playbar. One action with both hands as subaction paths.
    float GripValue() const { return _gripValue; }
    float LeftGripValue() const { return _leftGripValue; }

    // Right controller's aim pose in the reference space, located at the
    // previous frame's predicted display time. False if not tracked.
    bool RightAimPose(XrPosef* pose) const
    {
        *pose = _rightAimPose;
        return _rightAimValid;
    }

    // Same, for the left controller.
    bool LeftAimPose(XrPosef* pose) const
    {
        *pose = _leftAimPose;
        return _leftAimValid;
    }

private:
    bool _InitImpl(GLContext const& gl);
    bool _InitInput();
    void _SyncInput();
    void _LocateControllers(XrTime time);
    bool _LocateAim(XrSpace space, XrTime time, XrPosef* pose) const;

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
    XrAction    _turnAction       = XR_NULL_HANDLE;
    XrAction    _thumbClickAction = XR_NULL_HANDLE;
    XrAction    _menuAction       = XR_NULL_HANDLE;
    XrAction    _buttonXAction    = XR_NULL_HANDLE;
    XrAction    _buttonYAction    = XR_NULL_HANDLE;
    XrAction    _buttonAAction    = XR_NULL_HANDLE;
    XrAction    _buttonBAction    = XR_NULL_HANDLE;
    XrAction    _gripAction       = XR_NULL_HANDLE;
    XrAction    _aimPoseAction    = XR_NULL_HANDLE;
    XrSpace     _rightAimSpace    = XR_NULL_HANDLE;
    XrSpace     _leftAimSpace     = XR_NULL_HANDLE;
    XrPath      _leftHand         = XR_NULL_PATH;
    XrPath      _rightHand        = XR_NULL_PATH;
    float       _triggerValue     = 0.0f;
    float       _leftGripValue    = 0.0f;
    float       _gripValue        = 0.0f;
    XrVector2f  _rightThumbstick{0.0f, 0.0f};
    XrVector2f  _leftThumbstick{0.0f, 0.0f};
    bool        _rightThumbstickPressed = false;
    bool        _leftMenuPressed = false;
    bool        _buttonXPressed  = false;
    bool        _buttonYPressed  = false;
    bool        _leftTriggerPressed = false;
    bool        _leftTriggerDown    = false;   // hysteresis state; persists across syncs
    bool        _buttonA = false;
    bool        _buttonB = false;
    Panel       _panel;
    Billboard   _billboard;
    bool        _panelDirty = false;
    XrPosef     _rightAimPose{};
    bool        _rightAimValid = false;
    XrPosef     _leftAimPose{};
    bool        _leftAimValid = false;
    bool     _running   = false;
    bool     _quit      = false;
};
