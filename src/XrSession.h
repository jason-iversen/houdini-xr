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

private:
    bool _InitImpl(GLContext const& gl);

    XrInstance _instance = XR_NULL_HANDLE;
    XrSystemId _systemId = XR_NULL_SYSTEM_ID;
    XrSession  _session  = XR_NULL_HANDLE;
    XrSpace    _space    = XR_NULL_HANDLE;

    XrPresenterGL _presenter;

    std::vector<XrViewConfigurationView> _viewConfigs;
    std::vector<XrView> _views;

    uint32_t _eyeWidth  = 0;
    uint32_t _eyeHeight = 0;
    bool     _running   = false;
    bool     _quit      = false;
};
