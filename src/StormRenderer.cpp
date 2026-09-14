#include "StormRenderer.h"

#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/simpleMaterial.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hgiGL/texture.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

const TfToken kStormPlugin("HdStormRendererPlugin");

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

bool StormRenderer::Init(GfVec2i const& bufferSize)
{
    _size = bufferSize;

    UsdImagingGLEngine::Parameters params;
    params.rendererPluginId = kStormPlugin;
    params.gpuEnabled = true;

    _engine = std::make_unique<UsdImagingGLEngine>(params);

    if (!_engine->SetRendererAov(HdAovTokens->color)) {
        std::fprintf(stderr, "StormRenderer: SetRendererAov(color) failed\n");
        return false;
    }

    // We own presentation: offscreen readback here, and an OpenXR swapchain blit
    // later. Letting the engine composite would drag in hgiInterop for nothing.
    _engine->SetEnablePresentation(false);

    _engine->SetRenderBufferSize(_size);
    _engine->SetFraming(CameraUtilFraming(
        GfRect2i(GfVec2i(0, 0), _size[0], _size[1])));

    return true;
}

void StormRenderer::RenderEye(GfMatrix4d const& view, GfMatrix4d const& proj, double frame)
{
    if (!_engine || !_stage) {
        return;
    }

    _engine->SetCameraState(view, proj);

    // Headlight at the eye, so a stage carrying no lights of its own still
    // resolves to something other than a black frame.
    const GfVec3d eyePos = view.GetInverse().ExtractTranslation();

    GlfSimpleLight light;
    light.SetPosition(GfVec4f(float(eyePos[0]), float(eyePos[1]), float(eyePos[2]), 1.0f));

    GlfSimpleMaterial material;
    material.SetAmbient(GfVec4f(0.2f, 0.2f, 0.2f, 1.0f));
    material.SetDiffuse(GfVec4f(1.0f, 1.0f, 1.0f, 1.0f));

    _engine->SetLightingState({light}, material, GfVec4f(0.1f, 0.1f, 0.1f, 1.0f));

    UsdImagingGLRenderParams params;
    params.frame          = frame;
    params.complexity     = 1.0f;
    params.drawMode       = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    params.enableLighting = true;
    params.clearColor     = GfVec4f(0.05f, 0.05f, 0.08f, 1.0f);

    _engine->Render(_stage->GetPseudoRoot(), params);
}

bool StormRenderer::ReadColor(std::vector<uint8_t>& rgba, int& width, int& height) const
{
    if (!_engine) {
        return false;
    }

    HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer) {
        std::fprintf(stderr, "StormRenderer: no colour AOV render buffer\n");
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
        // Storm's colour AOV is linear half-float; encode to sRGB so the
        // validation image matches what a viewport would show.
        const uint16_t* in = static_cast<const uint16_t*>(src);
        for (size_t i = 0; i < pixels; ++i) {
            const size_t o = i * 4;
            rgba[o + 0] = EncodeSrgb(HalfToFloat(in[o + 0]));
            rgba[o + 1] = EncodeSrgb(HalfToFloat(in[o + 1]));
            rgba[o + 2] = EncodeSrgb(HalfToFloat(in[o + 2]));
            rgba[o + 3] = EncodeLinear(HalfToFloat(in[o + 3]));
        }
    } else {
        std::fprintf(stderr, "StormRenderer: unhandled colour AOV format %d\n", int(format));
        ok = false;
    }

    buffer->Unmap();
    return ok;
}

uint32_t StormRenderer::ColorTextureId() const
{
    if (!_engine) {
        return 0;
    }

    HgiTextureHandle handle = _engine->GetAovTexture(HdAovTokens->color);
    auto* texture = dynamic_cast<HgiGLTexture*>(handle.Get());

    return texture ? texture->GetTextureId() : 0;
}

std::string StormRenderer::RendererName() const
{
    if (!_engine) {
        return "<uninitialised>";
    }

    const TfToken id = _engine->GetCurrentRendererId();
    return _engine->GetRendererDisplayName(id) + " [" + id.GetString() + "]";
}
