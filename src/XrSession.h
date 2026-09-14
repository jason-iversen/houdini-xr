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
    // Renders one eye and returns the GL texture holding the result.
    using RenderEyeFn = std::function<uint32_t(uint32_t view,
                                               GfMatrix4d const& viewMatrix,
                                               GfMatrix4d const& projMatrix)>;

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
