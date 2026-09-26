#pragma once

#include "XrPlatform.h"

#include <cstdint>
#include <functional>
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

    // Acquire, blit the source GL texture in, overlay the reticle, release.
    // The source may be smaller than the swapchain image; the blit scales it
    // up to fill. The reticle position is in this eye's NDC.
    bool PresentEye(uint32_t view, uint32_t srcTexture, int srcWidth, int srcHeight,
                    bool reticleVisible, float reticleNdcX, float reticleNdcY);

    // The desktop panel's swapchain: separate from the eyes', sized to the
    // panel image, and created on first use. `created` reports a new
    // swapchain, whose images hold nothing until drawn into.
    bool EnsurePanelSwapchain(uint32_t width, uint32_t height, bool* created);
    XrSwapchain PanelSwapchain() const { return _panel.swapchain; }
    uint32_t PanelWidth() const { return _panelWidth; }
    uint32_t PanelHeight() const { return _panelHeight; }

    // Acquire a panel image, bind it as the draw framebuffer, let `draw` fill
    // it (given its size), rebuild the mip chain, release. Mips matter here:
    // the compositor shrinks the panel well below its pixel size, and
    // without them text aliases and shimmers as the head moves.
    bool PresentPanel(std::function<void(int width, int height)> const& draw);

private:
    struct ViewSwapchain
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLKHR> images;
    };

    std::vector<ViewSwapchain> _views;
    ViewSwapchain _panel;
    uint32_t _panelWidth  = 0;
    uint32_t _panelHeight = 0;
    uint32_t _panelMips   = 1;
    XrSession _session    = XR_NULL_HANDLE;
    int64_t   _format     = 0;
    uint32_t _width   = 0;
    uint32_t _height  = 0;
    uint32_t _readFbo = 0;
    uint32_t _drawFbo = 0;
    bool     _srgb    = false;
};
