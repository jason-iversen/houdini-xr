#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Looks up the stage's active RenderSettings prim -- via the standard
// renderSettingsPrimPath stage metadata, the same mechanism a USD render
// delegate would use, not "the first RenderSettings prim found" -- and, if it
// targets a camera, returns that camera's local-to-world (stage-space)
// transform at `time`. Returns false if there's no active RenderSettings
// prim, no camera relationship, or the target camera prim doesn't exist.
bool FindRenderCameraTransform(UsdStageRefPtr const& stage, UsdTimeCode time,
                               GfMatrix4d* outStageFromCamera);
