#include "HydraRenderer.h"

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hf/pluginDesc.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/imaging/hgiGL/texture.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

const TfToken kStormPlugin("HdStormRendererPlugin");

bool IsRegistered(TfToken const& id)
{
    HfPluginDesc desc;
    return HdRendererPluginRegistry::GetInstance().GetPluginDesc(id, &desc);
}

float HalfToFloat(uint16_t h)
{
    const int sign = (h >> 15) & 1;
    const int exp  = (h >> 10) & 0x1F;
    const int mant = h & 0x3FF;

    float value;
    if (exp == 0) {
        value = std::ldexp(float(mant), -24);
    } else if (exp == 31) {
        value = mant ? std::numeric_limits<float>::quiet_NaN()
                     : std::numeric_limits<float>::infinity();
    } else {
        value = std::ldexp(float(mant + 1024), exp - 25);
    }
    return sign ? -value : value;
}

uint8_t EncodeSrgb(float linear)
{
    const float c = std::fmin(std::fmax(linear, 0.0f), 1.0f);
    const float s = (c <= 0.0031308f) ? c * 12.92f
                                      : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

uint8_t EncodeLinear(float v)
{
    const float c = std::fmin(std::fmax(v, 0.0f), 1.0f);
    return uint8_t(c * 255.0f + 0.5f);
}

} // namespace

HydraRenderer::~HydraRenderer()
{
    // Engines first, while the GL context that created their Hgi resources
    // is still current, and before the Hgi they share goes away.
    _engines.clear();
    for (Upload& upload : _uploads) {
        if (upload.texture) {
            glDeleteTextures(1, &upload.texture);
        }
    }
    _hgi.reset();
}

TfToken const& HydraRenderer::DefaultRendererId()
{
    return kStormPlugin;
}

std::vector<HydraRenderer::RendererInfo> HydraRenderer::AvailableRenderers()
{
    // The registry's own list, not UsdImagingGLEngine::GetRendererPlugins():
    // that one filters through each plugin's IsSupported(), which for Storm
    // probes the current GL context -- and this gets called from contexts
    // with no GL current, such as a Houdini parameter menu being opened.
    HfPluginDescVector descs;
    HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);

    std::vector<RendererInfo> out;
    out.reserve(descs.size());
    for (HfPluginDesc const& desc : descs) {
        out.push_back({desc.id, desc.displayName});
    }
    return out;
}

void HydraRenderer::Init(GfVec2i const& bufferSize, int viewCount)
{
    _size      = bufferSize;
    _viewCount = viewCount > 0 ? viewCount : 1;
    _uploads.assign(size_t(_viewCount), Upload{});
    if (_pluginId.IsEmpty()) {
        _pluginId = kStormPlugin;
    }

    // One Hgi for every engine this object ever creates; the engines only
    // borrow it.
    _hgi    = Hgi::CreatePlatformDefaultHgi();
    _driver = HdDriver{HgiTokens->renderDriver, VtValue(_hgi.get())};
}

void HydraRenderer::SetRenderSize(GfVec2i const& size)
{
    if (size == _size) {
        return;
    }
    _size = size;
    for (auto const& engine : _engines) {
        engine->SetRenderBufferSize(_size);
        engine->SetFraming(CameraUtilFraming(
            GfRect2i(GfVec2i(0, 0), _size[0], _size[1])));
    }
}

bool HydraRenderer::SetStage(UsdStageRefPtr const& stage)
{
    // Pointer identity is sound here, unlike for raw HUSD stages: every stage
    // reaching this class is a fresh, immutable flattened copy, so the same
    // object really does mean the same content.
    if (stage == _stage) {
        return true;
    }
    _stage = stage;
    return _stage ? _CreateEngines(1) : true;
}

bool HydraRenderer::SetRendererPlugin(TfToken const& id)
{
    TfToken wanted = id.IsEmpty() ? kStormPlugin : id;

    if (!IsRegistered(wanted)) {
        std::fprintf(stderr, "HydraRenderer: no render delegate '%s' is registered; "
                             "falling back to %s\n",
                     wanted.GetText(), kStormPlugin.GetText());
        wanted = kStormPlugin;
    }

    if (wanted == _pluginId) {
        return true;
    }
    _pluginId = wanted;
    return _stage ? _CreateEngines(1) : true;
}

bool HydraRenderer::_ConfigureEngine(UsdImagingGLEngine& engine) const
{
    if (!engine.SetRendererAov(HdAovTokens->color)) {
        std::fprintf(stderr, "HydraRenderer: SetRendererAov(color) failed for %s\n",
                     _pluginId.GetText());
        return false;
    }

    // We own presentation: offscreen readback, or an OpenXR swapchain blit.
    // Letting the engine composite would drag in hgiInterop for nothing.
    engine.SetEnablePresentation(false);

    engine.SetRenderBufferSize(_size);
    engine.SetFraming(CameraUtilFraming(
        GfRect2i(GfVec2i(0, 0), _size[0], _size[1])));
    return true;
}

bool HydraRenderer::_CreateEngines(int count)
{
    // Release the old ones before building new: each live engine is a full
    // set of Hgi resources and Hydra scene state.
    _engines.clear();
    _probed = false;

    for (int i = 0; i < count; ++i) {
        UsdImagingGLEngine::Parameters params;
        params.driver           = _driver;
        params.rendererPluginId = _pluginId;
        params.gpuEnabled       = true;

        auto engine = std::make_unique<UsdImagingGLEngine>(params);

        const TfToken actual = engine->GetCurrentRendererId();
        if (actual != _pluginId) {
            std::fprintf(stderr, "HydraRenderer: engine could not use '%s', running '%s' instead\n",
                         _pluginId.GetText(), actual.GetText());
            _pluginId = actual;
        }

        if (!_ConfigureEngine(*engine)) {
            _engines.clear();
            return false;
        }
        _engines.push_back(std::move(engine));
    }

    std::printf("HydraRenderer: %d engine%s built -- %s\n", count, count == 1 ? "" : "s",
                RendererName().c_str());
    return true;
}

UsdImagingGLEngine* HydraRenderer::_EngineFor(int view) const
{
    if (_engines.empty()) {
        return nullptr;
    }
    const size_t index = size_t(view) < _engines.size() ? size_t(view) : 0;
    return _engines[index].get();
}

void HydraRenderer::RenderEye(int view, GfMatrix4d const& viewMatrix,
                              GfMatrix4d const& projMatrix, double frame)
{
    UsdImagingGLEngine* engine = _EngineFor(view);
    if (!engine || !_stage) {
        return;
    }

    engine->SetCameraState(viewMatrix, projMatrix);

    // Headlight at the eye, so a stage carrying no lights of its own still
    // resolves to something other than a black frame.
    const GfVec3d eyePos = viewMatrix.GetInverse().ExtractTranslation();

    GlfSimpleLight light;
    light.SetPosition(GfVec4f(float(eyePos[0]), float(eyePos[1]), float(eyePos[2]), 1.0f));

    GlfSimpleMaterial material;
    material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.0f));
    material.SetDiffuse(GfVec4f(1.0f, 1.0f, 1.0f, 1.0f));

    engine->SetLightingState({light}, material, GfVec4f(0.1f, 0.1f, 0.1f, 1.0f));

    UsdImagingGLRenderParams params;
    params.frame          = frame;
    params.complexity     = 1.0f;
    params.drawMode       = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    params.enableLighting = true;
    params.clearColor     = GfVec4f(0.05f, 0.05f, 0.08f, 1.0f);

    engine->Render(_stage->GetPseudoRoot(), params);

    // First render since the engines were built: if the delegate has more to
    // do after one pass, it's progressive, and the other views need engines of
    // their own so that alternating cameras don't keep resetting it.
    if (!_probed && view == 0) {
        _probed = true;
        if (_viewCount > 1 && _engines.size() == 1 && !engine->IsConverged()) {
            std::printf("HydraRenderer: %s is progressive -- one engine per view\n",
                        _pluginId.GetText());
            for (int i = 1; i < _viewCount; ++i) {
                UsdImagingGLEngine::Parameters params2;
                params2.driver           = _driver;
                params2.rendererPluginId = _pluginId;
                params2.gpuEnabled       = true;
                auto extra = std::make_unique<UsdImagingGLEngine>(params2);
                if (_ConfigureEngine(*extra)) {
                    _engines.push_back(std::move(extra));
                }
            }
        }
    }
}

bool HydraRenderer::IsConverged(int view) const
{
    UsdImagingGLEngine* engine = _EngineFor(view);
    return engine ? engine->IsConverged() : true;
}

bool HydraRenderer::ReadColor(int view, std::vector<uint8_t>& rgba, int& width, int& height) const
{
    UsdImagingGLEngine* engine = _EngineFor(view);
    if (!engine) {
        return false;
    }

    HdRenderBuffer* buffer = engine->GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer) {
        std::fprintf(stderr, "HydraRenderer: no colour AOV render buffer\n");
        return false;
    }

    buffer->Resolve();
    width  = int(buffer->GetWidth());
    height = int(buffer->GetHeight());

    const HdFormat format = buffer->GetFormat();

    void* src = buffer->Map();
    if (!src) {
        return false;
    }

    const size_t pixels = size_t(width) * size_t(height);
    rgba.resize(pixels * 4);

    bool ok = true;
    if (format == HdFormatUNorm8Vec4) {
        std::memcpy(rgba.data(), src, rgba.size());
    } else if (format == HdFormatFloat16Vec4) {
        // Linear half-float; encode to sRGB so the validation image matches
        // what a viewport would show.
        const uint16_t* in = static_cast<const uint16_t*>(src);
        for (size_t i = 0; i < pixels; ++i) {
            const size_t o = i * 4;
            rgba[o + 0] = EncodeSrgb(HalfToFloat(in[o + 0]));
            rgba[o + 1] = EncodeSrgb(HalfToFloat(in[o + 1]));
            rgba[o + 2] = EncodeSrgb(HalfToFloat(in[o + 2]));
            rgba[o + 3] = EncodeLinear(HalfToFloat(in[o + 3]));
        }
    } else if (format == HdFormatFloat32Vec4) {
        const float* in = static_cast<const float*>(src);
        for (size_t i = 0; i < pixels; ++i) {
            const size_t o = i * 4;
            rgba[o + 0] = EncodeSrgb(in[o + 0]);
            rgba[o + 1] = EncodeSrgb(in[o + 1]);
            rgba[o + 2] = EncodeSrgb(in[o + 2]);
            rgba[o + 3] = EncodeLinear(in[o + 3]);
        }
    } else {
        std::fprintf(stderr, "HydraRenderer: unhandled colour AOV format %d\n", int(format));
        ok = false;
    }

    buffer->Unmap();
    return ok;
}

HydraRenderer::ColorTexture HydraRenderer::GetColorTexture(int view)
{
    UsdImagingGLEngine* engine = _EngineFor(view);
    if (!engine) {
        return {};
    }

    HgiTextureHandle handle = engine->GetAovTexture(HdAovTokens->color);
    if (auto* texture = dynamic_cast<HgiGLTexture*>(handle.Get())) {
        const GfVec3i dims = texture->GetDescriptor().dimensions;
        return {texture->GetTextureId(), dims[0], dims[1]};
    }

    // No GL-backed texture: the delegate's AOV lives in CPU memory. Upload it
    // in its native format -- the swapchain blit does the linear->sRGB encode,
    // exactly as it does for Storm's half-float output, so nothing about the
    // colour pipeline differs between the two paths.
    HdRenderBuffer* buffer = engine->GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer) {
        return {};
    }

    buffer->Resolve();
    const int      width  = int(buffer->GetWidth());
    const int      height = int(buffer->GetHeight());
    const HdFormat format = buffer->GetFormat();

    GLenum internalFormat = 0;
    GLenum type           = 0;
    switch (format) {
        case HdFormatUNorm8Vec4:  internalFormat = GL_RGBA8;   type = GL_UNSIGNED_BYTE; break;
        case HdFormatFloat16Vec4: internalFormat = GL_RGBA16F; type = GL_HALF_FLOAT;    break;
        case HdFormatFloat32Vec4: internalFormat = GL_RGBA32F; type = GL_FLOAT;         break;
        default:
            std::fprintf(stderr, "HydraRenderer: cannot upload colour AOV format %d\n",
                         int(format));
            return {};
    }

    void* src = buffer->Map();
    if (!src) {
        return {};
    }

    Upload& upload = _uploads[size_t(view) < _uploads.size() ? size_t(view) : 0];

    if (!upload.texture) {
        glGenTextures(1, &upload.texture);
        glBindTexture(GL_TEXTURE_2D, upload.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_2D, upload.texture);
    }

    const bool reallocate = (width != upload.width) || (height != upload.height) ||
                            (int(format) != upload.format);
    if (reallocate) {
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(internalFormat), width, height, 0,
                     GL_RGBA, type, src);
        upload.width  = width;
        upload.height = height;
        upload.format = int(format);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, type, src);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    buffer->Unmap();
    return {upload.texture, width, height};
}

std::string HydraRenderer::RendererName() const
{
    return UsdImagingGLEngine::GetRendererDisplayName(_pluginId) +
           " [" + _pluginId.GetString() + "]";
}
