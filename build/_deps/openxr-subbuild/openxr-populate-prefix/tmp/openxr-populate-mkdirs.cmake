# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src")
  file(MAKE_DIRECTORY "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-src")
endif()
file(MAKE_DIRECTORY
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-build"
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix"
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/tmp"
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/src/openxr-populate-stamp"
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/src"
  "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/src/openxr-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/src/openxr-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "G:/My Drive/Projects/code/3dgs/houdini-xr/build/_deps/openxr-subbuild/openxr-populate-prefix/src/openxr-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
