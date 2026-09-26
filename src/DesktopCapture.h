#pragma once

#include "XrPlatform.h"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutput1;
struct IDXGIOutputDuplication;

// Mirrors one monitor into a GL texture -- how Houdini's interface gets into
// the headset. The HDK has no way to render Houdini's UI to a texture, so
// this works at the OS level instead.
//
// Captures with DXGI Desktop Duplication, of a whole monitor rather than a
// window: Houdini's menus (the Tab menu included) are separate popup
// windows, which a window capture would miss. The D3D11 image is shared into
// GL with WGL_NV_DX_interop2 -- no CPU copy -- which NVIDIA, AMD and Intel
// drivers all expose, provided the monitor and the GL context are on the
// same GPU.
//
// Everything here runs on the thread that owns the GL context.
class DesktopCapture
{
public:
    ~DesktopCapture() { Shutdown(); }

    // Captures the monitor showing this process's largest visible window --
    // Houdini's main window, in the plugin -- or the primary monitor if the
    // process has none (the standalone tool). False, with the reason on
    // stderr, if capture or interop isn't available.
    bool Init();
    void Shutdown();
    bool IsValid() const { return _interopDevice != nullptr; }

    // Pulls the newest desktop image into the shared texture. Non-blocking:
    // returns immediately when nothing has changed. True if the image or the
    // pointer moved since the last call. Losing the duplication -- a UAC
    // prompt, a display mode change -- keeps the last image and retries.
    bool Update();

    int Width() const { return _width; }
    int Height() const { return _height; }

    // The size to show the capture at: halved -- exactly, so BlitTo's
    // bilinear filter is a clean 2x2 box -- until no wider than kPanelMaxWidth.
    // At ~20 pixels per degree on a Quest, a panel of any comfortable size
    // can't show more than that; a 4K monitor at full size is wasted memory.
    static constexpr int kPanelMaxWidth = 2560;
    void PanelSize(int* width, int* height) const;

    // Blits the latest image into the currently bound draw framebuffer,
    // stretched to fill dstWidth x dstHeight, with the mouse pointer marked.
    // Flipped on the way: the capture is top-row-first (D3D), while GL and
    // OpenXR's GL swapchains are bottom-row-first. The bytes are sRGB-encoded
    // and copied untouched, so the target should be an sRGB format with
    // GL_FRAMEBUFFER_SRGB off -- the compositor then decodes them once.
    void BlitTo(int dstWidth, int dstHeight);

private:
    bool _CreateDuplication();
    bool _CreateSharedTexture(int width, int height, unsigned format);
    void _ReleaseSharedTexture();

    ID3D11Device*           _device      = nullptr;
    ID3D11DeviceContext*    _context     = nullptr;
    IDXGIOutput1*           _output      = nullptr;
    IDXGIOutputDuplication* _duplication = nullptr;
    ID3D11Texture2D*        _shared      = nullptr;   // our copy, registered with GL

    HANDLE   _interopDevice = nullptr;
    HANDLE   _interopObject = nullptr;
    uint32_t _glTexture     = 0;
    uint32_t _readFbo       = 0;

    int      _width  = 0;
    int      _height = 0;
    unsigned _format = 0;
    bool     _haveImage = false;

    // Pointer hotspot in image pixels, top-left origin. Duplication reports
    // the shape's top-left corner; the hotspot offset comes with the shape.
    bool _pointerVisible = false;
    int  _pointerX = 0;
    int  _pointerY = 0;
    int  _hotspotX = 0;
    int  _hotspotY = 0;

    ULONGLONG _lastRetryMs = 0;
};
