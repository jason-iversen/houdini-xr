#pragma once

#include <pxr/pxr.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// Hydra behind a per-eye render call, with the render delegate selectable at
// runtime. Storm is the default and the only delegate that converges in a
// single pass.
//
// Progressive delegates (Karma CPU/XPU) accumulate samples across calls, and
// any camera change restarts that. Two eyes alternating through one engine
// would therefore never converge, even with a perfectly still head -- so a
// progressive delegate gets one engine per view, all sharing one Hgi. That's
// detected rather than assumed: the first render on the shared engine reports
// IsConverged(); if it's false, per-view engines are built. Storm stays on a
// single engine and pays nothing for the capability.
class HydraRenderer
{
public:
    struct RendererInfo
    {
        TfToken     id;
        std::string displayName;
    };

    ~HydraRenderer();

    static TfToken const&            DefaultRendererId();
    static std::vector<RendererInfo> AvailableRenderers();

    void Init(GfVec2i const& bufferSize, int viewCount);

    // Both of these rebuild the UsdImagingGLEngine(s). An engine populates
    // Hydra exactly once per instance (its private _isPopulated flag), so a
    // new stage handed to an existing engine is silently ignored -- upstream
    // edits would never reach the delegate. A new stage or delegate needs
    // new engines, not new pointers given to the old ones.
    bool SetStage(UsdStageRefPtr const& stage);
    bool SetRendererPlugin(TfToken const& id);

    // Live-resizes the render buffers without rebuilding. Rendering below the
    // swapchain's resolution and letting the presenter upscale is how a
    // progressive delegate becomes usable at all, and how a licence's
    // render-size cap is respected.
    void SetRenderSize(GfVec2i const& size);

    UsdStageRefPtr const& Stage() const { return _stage; }
    TfToken const&        RendererId() const { return _pluginId; }
    GfVec2i const&        RenderSize() const { return _size; }

    void RenderEye(int view, GfMatrix4d const& viewMatrix, GfMatrix4d const& projMatrix,
                   double frame = 0.0);

    // True once the delegate has nothing more to add for the current camera.
    // Immediately true for Storm; for a progressive delegate, only after it
    // has accumulated enough samples at an unchanged camera.
    bool IsConverged(int view) const;

    // Nearest surface under the centre of a (narrow) pick frustum, in stage
    // space. A pick is a render with its own camera, so it never runs on a
    // progressive delegate's display engine -- that would reset its
    // accumulation every time. With Storm displaying it uses that engine
    // (Storm re-renders fully each frame anyway); otherwise a dedicated Storm
    // engine, built on first use and dropped whenever the engines rebuild.
    bool Pick(GfMatrix4d const& viewMatrix, GfMatrix4d const& projMatrix, double frame,
              GfVec3d* outHitStage);

    // Offscreen validation path.
    bool ReadColor(int view, std::vector<uint8_t>& rgba, int& width, int& height) const;

    struct ColorTexture
    {
        uint32_t id     = 0;
        int      width  = 0;   // the texture's actual size, which is what a
        int      height = 0;   // blit needs -- not what was asked for
    };

    // GL texture holding the view's colour AOV. GPU delegates hand one back
    // directly. For delegates whose AOV lives in CPU memory it's uploaded into
    // a texture this object owns, so the presenter sees the same thing either
    // way.
    ColorTexture GetColorTexture(int view);

    std::string RendererName() const;

private:
    struct Upload
    {
        uint32_t texture = 0;
        int      width   = 0;
        int      height  = 0;
        int      format  = -1;
    };

    bool _CreateEngines(int count);
    bool _ConfigureEngine(UsdImagingGLEngine& engine) const;
    UsdImagingGLEngine* _EngineFor(int view) const;

    HgiUniquePtr _hgi;
    HdDriver     _driver;

    std::vector<std::unique_ptr<UsdImagingGLEngine>> _engines;
    std::unique_ptr<UsdImagingGLEngine>              _pickEngine;
    std::vector<Upload>                              _uploads;

    UsdStageRefPtr _stage;
    TfToken        _pluginId;
    GfVec2i        _size{0, 0};
    int            _viewCount = 1;

    // Whether the first render since the engines were (re)built has told us
    // yet if this delegate needs per-view engines.
    bool _probed = false;
};
