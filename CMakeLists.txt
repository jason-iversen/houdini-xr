cmake_minimum_required(VERSION 3.24)

# Runtime library is selected via CMAKE_MSVC_RUNTIME_LIBRARY rather than raw flags.
cmake_policy(SET CMP0091 NEW)

project(houdini_xr LANGUAGES CXX)

# These must match SideFX's own USD build settings or the ABI will not line up:
# Houdini 22.0 builds with /std:c++20 /MD /bigobj /EHsc /permissive-
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Default to the newest install found rather than a pinned version: Houdini
# point releases remove the old directory, which otherwise breaks the build
# with a stale cached path. Override with -DHFS=... to pin a specific one.
if(NOT DEFINED HFS)
  file(GLOB _houdini_installs "C:/Program Files/Side Effects Software/Houdini *")
  list(FILTER _houdini_installs INCLUDE REGEX "Houdini [0-9]+\\.[0-9]+\\.[0-9]+$")
  if(_houdini_installs)
    list(SORT _houdini_installs COMPARE NATURAL ORDER DESCENDING)
    list(GET _houdini_installs 0 _houdini_newest)
  endif()
endif()

set(HFS "${_houdini_newest}" CACHE PATH "Houdini install root (HFS)")

if(NOT HFS OR NOT EXISTS "${HFS}")
  message(FATAL_ERROR
    "No Houdini install found. Pass -DHFS=\"<houdini install dir>\" to cmake.")
endif()

set(_pxr_inc "${HFS}/toolkit/include")
set(_pxr_lib "${HFS}/custom/houdini/dsolib")

if(NOT EXISTS "${_pxr_inc}/pxr/pxr.h")
  message(FATAL_ERROR
    "pxr headers not found under ${_pxr_inc}\n"
    "Pass -DHFS=\"<houdini install dir>\" to cmake.")
endif()

# pxr.h defines PXR_PYTHON_SUPPORT_ENABLED, so parts of tf/ reach for Python.h.
file(GLOB _py_inc "${_pxr_inc}/python3*")

set(_pxr_modules
  arch tf gf js trace work plug vt ar sdf pcp
  usd usdGeom usdShade usdLux usdImaging usdImagingGL usdRender
  hd hdSt hdx hf hio garch glf hgi hgiGL hgiInterop
  cameraUtil pxOsd boost python)

set(_pxr_link)
foreach(m IN LISTS _pxr_modules)
  set(_l "${_pxr_lib}/libpxr_${m}.lib")
  if(NOT EXISTS "${_l}")
    message(FATAL_ERROR "Expected USD library not found: ${_l}")
  endif()
  list(APPEND _pxr_link "${_l}")
endforeach()

# TBB library naming has varied across Houdini releases.
file(GLOB _tbb "${_pxr_lib}/libtbb*.lib" "${_pxr_lib}/tbb*.lib")

# PXR_PYTHON_SUPPORT_ENABLED is baked into Houdini's USD build, so the
# pxr_boost python converter registry drags in libpython even for a C++-only host.
file(GLOB _py_lib "${HFS}/python3??/libs/python3??.lib")
if(NOT _py_lib)
  message(FATAL_ERROR "No python3XX.lib found under ${HFS}/python3XX/libs")
endif()

include(FetchContent)

# Only the loader is needed, and statically linked so the exe stays self-contained.
set(DYNAMIC_LOADER   OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(openxr
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG        release-1.1.63
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(openxr)

set(_hxr_compile_options
  # 4305 fires throughout Houdini's own gf/ half-float headers, not our code.
  /bigobj /EHsc /permissive- /Zc:__cplusplus /W3 /wd4996 /wd4244 /wd4267 /wd4305)
set(_hxr_defines NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)

# Shared XR/render core: everything from the GL context through the OpenXR
# session and Storm render calls, used unmodified by both the standalone exe
# and the Houdini plugin below. Deliberately knows nothing about Houdini's own
# HDK (LOP_Node, HUSD, OP_*) -- its only job is "render a UsdStageRefPtr to a
# headset", however that stage arrives.
add_library(hxr_core OBJECT
  src/GLContext.cpp
  src/HydraRenderer.cpp
  src/XrPresenterGL.cpp
  src/XrSession.cpp
  src/HxrRuntime.cpp
  src/RenderCamera.cpp
  src/Reticle.cpp)
target_include_directories(hxr_core PUBLIC "${_pxr_inc}" "${_py_inc}" src)
target_compile_options(hxr_core PRIVATE ${_hxr_compile_options})
target_compile_definitions(hxr_core PRIVATE ${_hxr_defines})
target_link_libraries(hxr_core PUBLIC openxr_loader)

# Standalone desktop test harness -- no Houdini process required. This is
# what Milestone 1 was validated against, and stays useful for iterating on
# hxr_core itself without a Houdini restart per change.
add_executable(hxr src/main.cpp)
target_include_directories(hxr PRIVATE "${_pxr_inc}" "${_py_inc}" src)
target_link_libraries(hxr PRIVATE
  hxr_core ${_pxr_link} ${_tbb} ${_py_lib} opengl32 user32 gdi32)
target_compile_options(hxr PRIVATE ${_hxr_compile_options})
target_compile_definitions(hxr PRIVATE ${_hxr_defines})

# Houdini LOP plugin: loaded in-process by Houdini itself. Bridges a live LOP
# stage into hxr_core via HxrRuntime, started/stopped from cookMyLop.
list(APPEND CMAKE_PREFIX_PATH "${HFS}/toolkit/cmake")
find_package(Houdini REQUIRED)

add_library(LOP_HoudiniXR SHARED
  plugin/LOP_XrOutput.C
  plugin/LOP_XrOutput.h)
target_include_directories(LOP_HoudiniXR PRIVATE "${_pxr_inc}" "${_py_inc}" src)
target_link_libraries(LOP_HoudiniXR PRIVATE
  Houdini HoudiniThirdParty hxr_core ${_pxr_link} ${_tbb} ${_py_lib}
  opengl32 user32 gdi32)

# houdini_configure_target() with no INSTDIR deploys straight into
# $HOUDINI_USER_PREF_DIR/dso, so a build alone makes the node available on
# Houdini's next restart -- no manual copy or HOUDINI_DSO_PATH needed.
houdini_configure_target(LOP_HoudiniXR)

message(STATUS "HFS          : ${HFS}")
message(STATUS "pxr includes : ${_pxr_inc}")
message(STATUS "pxr libdir   : ${_pxr_lib}")
message(STATUS "python inc   : ${_py_inc}")
message(STATUS "tbb          : ${_tbb}")
