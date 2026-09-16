#include "XrPresenterGL.h"

#include <algorithm>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

bool XrPresenterGL::CreateSwapchains(XrSession session, uint32_t width, uint32_t height,
                                     uint32_t viewCount)
{
    _width  = width;
    _height = height;

    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr))) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
        return false;
    }
    if (formats.empty()) {
        std::fprintf(stderr, "XrPresenterGL: runtime offered no swapchain formats\n");
        return false;
    }

    int64_t chosen = formats.front();
    for (const int64_t candidate : {int64_t(GL_SRGB8_ALPHA8), int64_t(GL_RGBA8)}) {
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            chosen = candidate;
            break;
        }
    }
    _srgb = (chosen == int64_t(GL_SRGB8_ALPHA8));

    _views.resize(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                           XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format      = chosen;
        info.sampleCount = 1;
        info.width       = width;
        info.height      = height;
        info.faceCount   = 1;
        info.arraySize   = 1;
        info.mipCount    = 1;

        if (XR_FAILED(xrCreateSwapchain(session, &info, &_views[i].swapchain))) {
            std::fprintf(stderr, "XrPresenterGL: xrCreateSwapchain failed for view %u\n", i);
            return false;
        }

        uint32_t imageCount = 0;
        if (XR_FAILED(xrEnumerateSwapchainImages(_views[i].swapchain, 0, &imageCount, nullptr))) {
            return false;
        }
        _views[i].images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
        if (XR_FAILED(xrEnumerateSwapchainImages(
                _views[i].swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(_views[i].images.data())))) {
            return false;
        }
    }

    glGenFramebuffers(1, &_readFbo);
    glGenFramebuffers(1, &_drawFbo);

    std::printf("XrPresenterGL: %u swapchains, %ux%u, format 0x%llx%s\n",
                viewCount, width, height, static_cast<unsigned long long>(chosen),
                _srgb ? " (sRGB)" : "");

    return true;
}

bool XrPresenterGL::PresentEye(uint32_t view, uint32_t srcTexture, int srcWidth, int srcHeight)
{
    ViewSwapchain& target = _views[view];

    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(target.swapchain, &acquireInfo, &index))) {
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(target.swapchain, &waitInfo))) {
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, _readFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, srcTexture, 0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _drawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, target.images[index].image, 0);

    // glBlitFramebuffer ignores the viewport but honours the scissor test.
    // Hydra's render pass sets a scissor rect matching its render size and
    // can leave it enabled; at full size that's invisible, but when rendering
    // below swapchain resolution it clips the upscaled blit back down to a
    // small rectangle in the corner.
    const bool scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);

    // The AOV is linear; the encode happens on write into an sRGB target.
    if (_srgb) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    glBlitFramebuffer(0, 0, srcWidth, srcHeight,
                      0, 0, int(_width), int(_height),
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (_srgb) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }

    if (scissorWasEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(target.swapchain, &releaseInfo));
}

void XrPresenterGL::Destroy()
{
    for (ViewSwapchain& target : _views) {
        if (target.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(target.swapchain);
        }
    }
    _views.clear();

    if (_readFbo) {
        glDeleteFramebuffers(1, &_readFbo);
        _readFbo = 0;
    }
    if (_drawFbo) {
        glDeleteFramebuffers(1, &_drawFbo);
        _drawFbo = 0;
    }
}
