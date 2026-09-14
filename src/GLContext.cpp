#include "GLContext.h"

#include <cstdio>

namespace {

constexpr int kContextMajorVersionArb = 0x2091;
constexpr int kContextMinorVersionArb = 0x2092;
constexpr int kContextProfileMaskArb  = 0x9126;

// Storm's HgiGL_ScopedStateHolder saves/restores legacy GL state and its indirect
// draw path relies on a usable default VAO, both of which fault in a core profile.
constexpr int kContextCompatibilityProfileBit = 0x0002;

using PfnWglCreateContextAttribsArb = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

HWND MakeHiddenWindow()
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);

    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = inst;
        wc.lpszClassName = L"hxrGLWindow";
        wc.style         = CS_OWNDC;
        if (!RegisterClassW(&wc)) {
            return nullptr;
        }
        registered = true;
    }

    return CreateWindowExW(0, L"hxrGLWindow", L"hxr", WS_OVERLAPPEDWINDOW,
                           0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
}

bool ApplyPixelFormat(HDC dc)
{
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;

    const int fmt = ChoosePixelFormat(dc, &pfd);
    return fmt != 0 && SetPixelFormat(dc, fmt, &pfd);
}

} // namespace

GLContext::~GLContext()
{
    if (_rc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(_rc);
    }
    if (_hdc && _hwnd) {
        ReleaseDC(_hwnd, _hdc);
    }
    if (_hwnd) {
        DestroyWindow(_hwnd);
    }
}

bool GLContext::Create()
{
    _hwnd = MakeHiddenWindow();
    if (!_hwnd) {
        std::fprintf(stderr, "GLContext: CreateWindowExW failed (%lu)\n", GetLastError());
        return false;
    }

    _hdc = GetDC(_hwnd);
    if (!_hdc || !ApplyPixelFormat(_hdc)) {
        std::fprintf(stderr, "GLContext: could not set a pixel format\n");
        return false;
    }

    // wglCreateContextAttribsARB can only be resolved once some context is current.
    HGLRC bootstrap = wglCreateContext(_hdc);
    if (!bootstrap) {
        std::fprintf(stderr, "GLContext: wglCreateContext failed (%lu)\n", GetLastError());
        return false;
    }
    wglMakeCurrent(_hdc, bootstrap);

    auto createContextAttribs = reinterpret_cast<PfnWglCreateContextAttribsArb>(
        wglGetProcAddress("wglCreateContextAttribsARB"));

    if (createContextAttribs) {
        const int attribs[] = {
            kContextMajorVersionArb, 4,
            kContextMinorVersionArb, 5,
            kContextProfileMaskArb,  kContextCompatibilityProfileBit,
            0
        };
        _rc = createContextAttribs(_hdc, nullptr, attribs);
    }

    if (_rc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(bootstrap);
        wglMakeCurrent(_hdc, _rc);
    } else {
        std::fprintf(stderr, "GLContext: no 4.5 context, falling back to bootstrap context\n");
        _rc = bootstrap;
    }

    return true;
}

void GLContext::MakeCurrent() const
{
    wglMakeCurrent(_hdc, _rc);
}
