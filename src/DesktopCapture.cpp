#include "DesktopCapture.h"

#include "Reticle.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// WGL_NV_DX_interop2. Declared here rather than taken from wglext.h, which
// neither Windows nor Houdini's GL loader provides.
constexpr GLenum kWglAccessReadOnlyNv = 0x0000;

using PfnDxOpenDevice       = HANDLE(WINAPI*)(void* dxDevice);
using PfnDxCloseDevice      = BOOL(WINAPI*)(HANDLE device);
using PfnDxRegisterObject   = HANDLE(WINAPI*)(HANDLE device, void* dxObject, GLuint name,
                                              GLenum type, GLenum access);
using PfnDxUnregisterObject = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using PfnDxLockObjects      = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
using PfnGetExtensionsArb   = const char*(WINAPI*)(HDC dc);

struct DxInterop
{
    PfnDxOpenDevice       openDevice       = nullptr;
    PfnDxCloseDevice      closeDevice      = nullptr;
    PfnDxRegisterObject   registerObject   = nullptr;
    PfnDxUnregisterObject unregisterObject = nullptr;
    PfnDxLockObjects      lockObjects      = nullptr;
    PfnDxLockObjects      unlockObjects    = nullptr;   // same signature

    // Needs a current GL context: WGL entry points are per-driver.
    bool Load()
    {
        auto getExtensions = reinterpret_cast<PfnGetExtensionsArb>(
            wglGetProcAddress("wglGetExtensionsStringARB"));
        const char* extensions = getExtensions ? getExtensions(wglGetCurrentDC()) : nullptr;
        if (!extensions || !std::strstr(extensions, "WGL_NV_DX_interop")) {
            std::fprintf(stderr, "DesktopCapture: the GL driver doesn't expose "
                                 "WGL_NV_DX_interop2\n");
            return false;
        }
        openDevice       = reinterpret_cast<PfnDxOpenDevice>(wglGetProcAddress("wglDXOpenDeviceNV"));
        closeDevice      = reinterpret_cast<PfnDxCloseDevice>(wglGetProcAddress("wglDXCloseDeviceNV"));
        registerObject   = reinterpret_cast<PfnDxRegisterObject>(wglGetProcAddress("wglDXRegisterObjectNV"));
        unregisterObject = reinterpret_cast<PfnDxUnregisterObject>(wglGetProcAddress("wglDXUnregisterObjectNV"));
        lockObjects      = reinterpret_cast<PfnDxLockObjects>(wglGetProcAddress("wglDXLockObjectsNV"));
        unlockObjects    = reinterpret_cast<PfnDxLockObjects>(wglGetProcAddress("wglDXUnlockObjectsNV"));
        return openDevice && closeDevice && registerObject && unregisterObject &&
               lockObjects && unlockObjects;
    }
};

DxInterop g_interop;

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The monitor showing this process's largest visible top-level window. In
// Houdini that's the main window; the GL context's own window is never shown,
// so it doesn't compete.
HMONITOR ChooseMonitor()
{
    struct Largest
    {
        HWND hwnd = nullptr;
        LONG area = 0;
    } best;

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            RECT r{};
            if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
                !GetWindowRect(hwnd, &r)) {
                return TRUE;
            }
            auto*      largest = reinterpret_cast<Largest*>(param);
            const LONG area    = (r.right - r.left) * (r.bottom - r.top);
            if (area > largest->area) {
                largest->hwnd = hwnd;
                largest->area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));

    return best.hwnd ? MonitorFromWindow(best.hwnd, MONITOR_DEFAULTTOPRIMARY)
                     : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

} // namespace

bool DesktopCapture::Init()
{
    if (IsValid()) {
        return true;
    }
    if (!g_interop.Load()) {
        return false;
    }

    // Duplication must run on the adapter that drives the monitor, so find
    // the output first and create the device on its adapter.
    const HMONITOR monitor = ChooseMonitor();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::fprintf(stderr, "DesktopCapture: CreateDXGIFactory1 failed\n");
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput>   output;
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> candidate;
        for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor) {
                output = candidate;
                break;
            }
        }
        if (!output) {
            adapter.Reset();
        }
    }
    if (!output || FAILED(output->QueryInterface(IID_PPV_ARGS(&_output)))) {
        std::fprintf(stderr, "DesktopCapture: no DXGI output for the chosen monitor\n");
        Shutdown();
        return false;
    }

    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &_device, nullptr, &_context))) {
        std::fprintf(stderr, "DesktopCapture: D3D11CreateDevice failed\n");
        Shutdown();
        return false;
    }

    _interopDevice = g_interop.openDevice(_device);
    if (!_interopDevice) {
        // Most likely the monitor hangs off a different GPU from the one
        // running the GL context (a laptop's integrated GPU, say).
        std::fprintf(stderr, "DesktopCapture: wglDXOpenDeviceNV failed (error %lu) -- is "
                             "the monitor on a different GPU from Houdini?\n", GetLastError());
        Shutdown();
        return false;
    }

    glGenFramebuffers(1, &_readFbo);

    if (!_CreateDuplication()) {
        Shutdown();
        return false;
    }

    // The first acquire after DuplicateOutput returns the whole desktop, so
    // this normally fills the image straight away rather than showing an
    // empty panel until something on screen changes.
    for (int attempt = 0; attempt < 20 && !_haveImage; ++attempt) {
        Update();
        if (!_haveImage) {
            Sleep(10);
        }
    }

    std::printf("DesktopCapture: %dx%d monitor%s\n", _width, _height,
                _haveImage ? "" : " (no image yet)");
    return true;
}

bool DesktopCapture::_CreateDuplication()
{
    const HRESULT hr = _output->DuplicateOutput(_device, &_duplication);
    if (FAILED(hr)) {
        // E_ACCESSDENIED while a UAC prompt or the lock screen is up;
        // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE if too many apps are duplicating.
        std::fprintf(stderr, "DesktopCapture: DuplicateOutput failed (0x%08lx)\n",
                     static_cast<unsigned long>(hr));
        _duplication = nullptr;
        return false;
    }

    DXGI_OUTDUPL_DESC desc{};
    _duplication->GetDesc(&desc);
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        std::fprintf(stderr, "DesktopCapture: monitor is rotated; the panel will be too\n");
    }

    // A mode change (resolution, HDR toggle) arrives as a lost duplication;
    // the new one may be a different size or format.
    const int width  = int(desc.ModeDesc.Width);
    const int height = int(desc.ModeDesc.Height);
    if (!_shared || width != _width || height != _height || desc.ModeDesc.Format != _format) {
        _ReleaseSharedTexture();
        if (!_CreateSharedTexture(width, height, desc.ModeDesc.Format)) {
            SafeRelease(_duplication);
            return false;
        }
    }
    return true;
}

bool DesktopCapture::_CreateSharedTexture(int width, int height, unsigned format)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = UINT(width);
    desc.Height           = UINT(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT(format);
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(_device->CreateTexture2D(&desc, nullptr, &_shared))) {
        std::fprintf(stderr, "DesktopCapture: CreateTexture2D failed\n");
        return false;
    }

    glGenTextures(1, &_glTexture);
    _interopObject = g_interop.registerObject(_interopDevice, _shared, _glTexture,
                                              GL_TEXTURE_2D, kWglAccessReadOnlyNv);
    if (!_interopObject) {
        std::fprintf(stderr, "DesktopCapture: wglDXRegisterObjectNV failed (error %lu, "
                             "DXGI format %u)\n", GetLastError(), format);
        _ReleaseSharedTexture();
        return false;
    }

    _width     = width;
    _height    = height;
    _format    = format;
    _haveImage = false;
    return true;
}

void DesktopCapture::_ReleaseSharedTexture()
{
    if (_interopObject) {
        g_interop.unregisterObject(_interopDevice, _interopObject);
        _interopObject = nullptr;
    }
    if (_glTexture) {
        glDeleteTextures(1, &_glTexture);
        _glTexture = 0;
    }
    SafeRelease(_shared);
    _haveImage = false;
}

bool DesktopCapture::Update()
{
    if (!IsValid()) {
        return false;
    }
    if (!_duplication) {
        // Throttled: while the secure desktop is up, every attempt fails.
        const ULONGLONG now = GetTickCount64();
        if (now - _lastRetryMs < 500) {
            return false;
        }
        _lastRetryMs = now;
        if (!_CreateDuplication()) {
            return false;
        }
    }

    DXGI_OUTDUPL_FRAME_INFO  info{};
    ComPtr<IDXGIResource>    resource;
    const HRESULT hr = _duplication->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;   // nothing new
    }
    if (FAILED(hr)) {
        // DXGI_ERROR_ACCESS_LOST: mode change, UAC, a fullscreen-exclusive
        // app. Keep showing the last image and start over.
        SafeRelease(_duplication);
        return false;
    }

    bool changed = false;

    // Pointer-only updates arrive with no new image (LastPresentTime == 0).
    if (info.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(resource.As(&frame))) {
            _context->CopyResource(_shared, frame.Get());
            _haveImage = true;
            changed    = true;
        }
    }

    // Shape before position: they can arrive together, and the hotspot
    // offset belongs to the shape.
    if (info.PointerShapeBufferSize > 0) {
        std::vector<BYTE> shape(info.PointerShapeBufferSize);
        UINT required = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
        if (SUCCEEDED(_duplication->GetFramePointerShape(UINT(shape.size()), shape.data(),
                                                         &required, &shapeInfo))) {
            _hotspotX = int(shapeInfo.HotSpot.x);
            _hotspotY = int(shapeInfo.HotSpot.y);
        }
    }
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool visible = info.PointerPosition.Visible != FALSE;
        const int  x       = int(info.PointerPosition.Position.x) + _hotspotX;
        const int  y       = int(info.PointerPosition.Position.y) + _hotspotY;
        changed = changed || visible != _pointerVisible || x != _pointerX || y != _pointerY;
        _pointerVisible = visible;
        _pointerX       = x;
        _pointerY       = y;
    }

    _duplication->ReleaseFrame();
    return changed;
}

void DesktopCapture::PanelSize(int* width, int* height) const
{
    *width  = _width;
    *height = _height;
    while (*width > kPanelMaxWidth) {
        *width /= 2;
        *height /= 2;
    }
}

void DesktopCapture::BlitTo(int dstWidth, int dstHeight)
{
    const bool scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool srgbWasEnabled    = glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE;
    glDisable(GL_SCISSOR_TEST);      // blits honour it; see XrPresenterGL
    glDisable(GL_FRAMEBUFFER_SRGB);  // already sRGB-encoded: copy, don't re-encode

    HANDLE object = _interopObject;
    if (_glTexture && _haveImage && g_interop.lockObjects(_interopDevice, 1, &object)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, _readFbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               _glTexture, 0);
        // Destination y reversed: top-row-first in, bottom-row-first out.
        glBlitFramebuffer(0, 0, _width, _height, 0, dstHeight, dstWidth, 0,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        g_interop.unlockObjects(_interopDevice, 1, &object);

        if (_pointerVisible && _width > 0 && _height > 0) {
            const float ndcX = (float(_pointerX) + 0.5f) / float(_width) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (float(_pointerY) + 0.5f) / float(_height) * 2.0f;
            DrawReticle(dstWidth, dstHeight, ndcX, ndcY);
        }
    } else {
        // Nothing captured yet: a neutral grey rather than whatever the
        // target happened to contain.
        GLfloat clear[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
    }

    if (scissorWasEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (srgbWasEnabled) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
}

void DesktopCapture::Shutdown()
{
    _ReleaseSharedTexture();
    if (_interopDevice) {
        g_interop.closeDevice(_interopDevice);
        _interopDevice = nullptr;
    }
    if (_readFbo) {
        glDeleteFramebuffers(1, &_readFbo);
        _readFbo = 0;
    }
    SafeRelease(_duplication);
    SafeRelease(_output);
    SafeRelease(_context);
    SafeRelease(_device);
    _width  = 0;
    _height = 0;
    _format = 0;
    _pointerVisible = false;
}
