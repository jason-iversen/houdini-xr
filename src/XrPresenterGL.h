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

    // Flat quad layers, each with a swapchain of its own: the desktop panel,
    // and the billboard single-eye rendering is shown on.
    enum class Quad
    {
        Panel,
        Billboard,
        Count
    };

    // Created on first use, recreated if the size changes. `created` reports
    // a new swapchain, whose images hold nothing until drawn into.
    bool EnsureQuadSwapchain(Quad quad, uint32_t width, uint32_t height, bool* created);
    XrSwapchain QuadSwapchain(Quad quad) const { return _quads[int(quad)].chain.swapchain; }
    uint32_t QuadWidth(Quad quad) const { return _quads[int(quad)].width; }
    uint32_t QuadHeight(Quad quad) const { return _quads[int(quad)].height; }

    // Acquire an image, bind it as the draw framebuffer, let `draw` fill it
    // (given its size), rebuild the mip chain, release. Mips matter: the
    // compositor often shrinks a quad well below its pixel size, and without
    // them detail aliases and shimmers as the head moves.
    bool PresentQuad(Quad quad, std::function<void(int width, int height)> const& draw);

    // Into the bound draw framebuffer: a linear-colour texture (a Hydra AOV),
    // stretched to fill, sRGB-encoded on write when the swapchains are sRGB,
    // then the reticle at an NDC position. PresentEye's own blit; also what
    // fills the billboard.
    void BlitLinear(uint32_t srcTexture, int srcWidth, int srcHeight, int dstWidth,
                    int dstHeight, bool reticleVisible, float reticleNdcX, float reticleNdcY);

private:
    struct ViewSwapchain
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLKHR> images;
    };
    struct QuadChain
    {
        ViewSwapchain chain;
        uint32_t      width  = 0;
        uint32_t      height = 0;
        uint32_t      mips   = 1;
    };

    std::vector<ViewSwapchain> _views;
    QuadChain _quads[int(Quad::Count)];
    XrSession _session    = XR_NULL_HANDLE;
    int64_t   _format     = 0;
    uint32_t _width   = 0;
    uint32_t _height  = 0;
    uint32_t _readFbo = 0;
    uint32_t _drawFbo = 0;
    bool     _srgb    = false;
};
