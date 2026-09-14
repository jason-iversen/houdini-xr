#pragma once

// Include order matters. garch/glApi.h defines the GL types at global scope (and
// claims the __gl_h_ guard so the Windows GL header stays out), which is what
// openxr_platform.h needs in scope before it declares the OpenGL binding structs.
#include <windows.h>

// openxr_platform.h's XR_USE_PLATFORM_WIN32 block declares the MSFT perception
// anchor interop entry points in terms of IUnknown, which WIN32_LEAN_AND_MEAN
// keeps out of windows.h.
#include <unknwn.h>

#include <pxr/imaging/garch/glApi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
