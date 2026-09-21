#include "RenderCamera.h"

#include <pxr/base/arch/pragmas.h>
ARCH_PRAGMA_PUSH
ARCH_PRAGMA_MACRO_TOO_FEW_ARGUMENTS
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdRender/settings.h>
ARCH_PRAGMA_POP

bool FindRenderCameraPath(UsdStageRefPtr const& stage, SdfPath* outCameraPath)
{
    if (!stage) {
        return false;
    }

    UsdRenderSettings settings =
        UsdRenderSettings::GetStageRenderSettings(UsdStageWeakPtr(stage));
    if (!settings) {
        return false;
    }

    SdfPathVector targets;
    if (!settings.GetCameraRel().GetTargets(&targets) || targets.empty()) {
        return false;
    }

    if (!stage->GetPrimAtPath(targets.front())) {
        return false;
    }

    *outCameraPath = targets.front();
    return true;
}

bool FindRenderCameraTransform(UsdStageRefPtr const& stage, UsdTimeCode time,
                               GfMatrix4d* outStageFromCamera)
{
    SdfPath cameraPath;
    if (!FindRenderCameraPath(stage, &cameraPath)) {
        return false;
    }

    UsdGeomImageable camera(stage->GetPrimAtPath(cameraPath));
    if (!camera) {
        return false;
    }

    *outStageFromCamera = camera.ComputeLocalToWorldTransform(time);
    return true;
}
