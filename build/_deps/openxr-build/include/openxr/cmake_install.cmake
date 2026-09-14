# Install script for directory: G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files/houdini_xr")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Headers" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/openxr" TYPE FILE FILES
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_platform_defines.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_loader_negotiation.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_platform.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_reflection.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_reflection_structs.h"
    "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src/include/openxr/openxr_reflection_parent_structs.h"
    )
endif()

