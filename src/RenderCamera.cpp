#include "RenderCamera.h"

#include <pxr/base/arch/pragmas.h>
ARCH_PRAGMA_PUSH
ARCH_PRAGMA_MACRO_TOO_FEW_ARGUMENTS
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>
#include <pxr/usd/usdLux/lightAPI.h>
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
    return FindRenderCameraPath(stage, &cameraPath) &&
           FindPrimTransform(stage, cameraPath, time, outStageFromCamera);
}

bool FindPrimTransform(UsdStageRefPtr const& stage, SdfPath const& path, UsdTimeCode time,
                       GfMatrix4d* outStageFromPrim)
{
    if (!stage) {
        return false;
    }
    UsdGeomImageable imageable(stage->GetPrimAtPath(path));
    if (!imageable) {
        return false;
    }
    *outStageFromPrim = imageable.ComputeLocalToWorldTransform(time);
    return true;
}

std::vector<StageView> FindStageViews(UsdStageRefPtr const& stage)
{
    std::vector<StageView> cameras;
    std::vector<StageView> lights;
    if (!stage) {
        return cameras;
    }
    for (UsdPrim const& prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            cameras.push_back({prim.GetPath(), false});
        } else if (prim.HasAPI<UsdLuxLightAPI>() && !prim.IsA<UsdLuxDomeLight>() &&
                   !prim.IsA<UsdLuxDomeLight_1>()) {
            lights.push_back({prim.GetPath(), true});
        }
    }
    cameras.insert(cameras.end(), lights.begin(), lights.end());
    return cameras;
}
