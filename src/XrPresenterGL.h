#pragma once

#include "XrPlatform.h"

#include <cstdint>
#include <vector>

// Owns the per-eye swapchains and moves Storm's rendered texture into them.
//
// This is the GL path, which exists because Houdini's USD build ships Hgi with
// an OpenGL backend only, so the runtime must expose XR_KHR_opengl_enable. Both
// the Oculus PC runtime and SteamVR do -- run hxr --probe to check any other.
class XrPresenterGL
{
public:
    bool CreateSwapchains(XrSession session, uint32_t width, uint32_t height, uint32_t viewCount);
    void Destroy();

    XrSwapchain Swapchain(uint32_t view) const { return _views[view].swapchain; }

    // Acquire, blit the source GL texture in, release.
    bool PresentEye(uint32_t view, uint32_t srcTexture);

private:
    struct ViewSwapchain
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLKHR> images;
    };

    std::vector<ViewSwapchain> _views;
    uint32_t _width   = 0;
    uint32_t _height  = 0;
    uint32_t _readFbo = 0;
    uint32_t _drawFbo = 0;
    bool     _srgb    = false;
};
