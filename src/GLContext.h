#pragma once

#include <windows.h>

// Storm requires a current GL context before UsdImagingGLEngine will initialise.
// The HDC/HGLRC pair is also precisely what XrGraphicsBindingOpenGLWin32KHR
// consumes, so the same context carries over to the XR path unchanged.
class GLContext
{
public:
    ~GLContext();

    bool Create();
    void MakeCurrent() const;

    HDC   Dc() const { return _hdc; }
    HGLRC Rc() const { return _rc; }

private:
    HWND  _hwnd = nullptr;
    HDC   _hdc  = nullptr;
    HGLRC _rc   = nullptr;
};
