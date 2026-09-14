#pragma once

#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// Hydra/Storm behind a per-eye render call. Milestone 2 repoints SetStage() at a
// stage handed across from a Houdini LOP cook; nothing else here has to change.
class StormRenderer
{
public:
    bool Init(GfVec2i const& bufferSize);

    void SetStage(UsdStageRefPtr const& stage) { _stage = stage; }
    UsdStageRefPtr const& Stage() const { return _stage; }

    void RenderEye(GfMatrix4d const& view, GfMatrix4d const& proj, double frame = 0.0);

    // Offscreen validation path.
    bool ReadColor(std::vector<uint8_t>& rgba, int& width, int& height) const;

    // XR path: the GL texture backing the colour AOV, for blitting straight into
    // an OpenXR swapchain image with no CPU round trip.
    uint32_t ColorTextureId() const;

    std::string RendererName() const;

private:
    std::unique_ptr<UsdImagingGLEngine> _engine;
    UsdStageRefPtr _stage;
    GfVec2i _size{0, 0};
};
