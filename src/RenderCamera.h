#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Looks up the stage's active RenderSettings prim -- via the standard
// renderSettingsPrimPath stage metadata, the same mechanism a USD render
// delegate would use, not "the first RenderSettings prim found" -- and
// returns the path of the camera it targets. False if there's no active
// RenderSettings prim, no camera relationship, or the target doesn't exist.
bool FindRenderCameraPath(UsdStageRefPtr const& stage, SdfPath* outCameraPath);

// That camera's local-to-world (stage-space) transform at `time`.
bool FindRenderCameraTransform(UsdStageRefPtr const& stage, UsdTimeCode time,
                               GfMatrix4d* outStageFromCamera);
