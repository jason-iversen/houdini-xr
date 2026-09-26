// Two modes:
//   (default) offscreen -- render one frame to a BMP, no headset needed.
//   --xr                -- drive an OpenXR stereo session and present to the HMD.

#include "GLContext.h"
#include "HxrRuntime.h"
#include "HydraRenderer.h"
#include "Reticle.h"
#include "XrPlatform.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/sphere.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct Options
{
    std::string stagePath;
    std::string outPath = "hxr_frame.bmp";
    std::string renderer;   // Hydra plugin id; empty = Storm
    int   maxWidth  = 0;    // per-eye render cap for --xr; 0 = uncapped
    int   maxHeight = 0;
    float converge  = 1.0f; // seconds to hold a pose for a progressive delegate
    bool  frozen    = false;
    bool  pick      = false; // offscreen: also pick along the view centre
    bool  reticle   = false; // offscreen: draw the reticle into the image
    int  width  = 1280;
    int  height = 720;
    bool  xr     = false;
    bool  probe  = false;
    int   frames = 0;   // 0 = run until the runtime asks us to stop

    // Where the stage's own origin sits relative to the XR reference space.
    // STAGE reference space puts (0,0,0) at floor level at the room's
    // calibrated center, so with no offset, content authored at the stage
    // origin is centered on the floor under the user rather than floating
    // in front of them. These defaults put it at arm's reach and roughly
    // chest height instead.
    float anchorDist   = 2.0f;
    float anchorHeight = 1.2f;
};

// Reports what the currently selected OpenXR runtime can do. Needs no headset
// and no session, so it answers "does this runtime expose the GL binding?"
// without anything being plugged in.
int RunProbe()
{
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        std::fprintf(stderr,
                     "xrEnumerateInstanceExtensionProperties failed (XrResult %d)\n"
                     "No usable OpenXR runtime -- is one installed and running?\n",
                     int(result));
        return 1;
    }

    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "xrEnumerateInstanceExtensionProperties failed (XrResult %d)\n",
                     int(result));
        return 1;
    }

    bool hasOpenGl = false;
    std::printf("Runtime advertises %u extensions:\n", count);
    for (XrExtensionProperties const& extension : extensions) {
        std::printf("  %s\n", extension.extensionName);
        if (std::strcmp(extension.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) {
            hasOpenGl = true;
        }
    }

    std::printf("\n%s: %s\n", XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                hasOpenGl ? "SUPPORTED" : "NOT SUPPORTED");

    // Registry listing only -- no GL context needed, and no attempt to load
    // the plugins, so this reports what's discoverable rather than proven.
    std::printf("\nHydra render delegates registered (pass one as --renderer):\n");
    for (HydraRenderer::RendererInfo const& info : HydraRenderer::AvailableRenderers()) {
        std::printf("  %-28s %s%s\n", info.id.GetText(), info.displayName.c_str(),
                    info.id == HydraRenderer::DefaultRendererId() ? "  (default)" : "");
    }

    return hasOpenGl ? 0 : 1;
}

Options ParseArgs(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = (i + 1) < argc;

        if (arg == "--stage" && hasNext) {
            opts.stagePath = argv[++i];
        } else if (arg == "--out" && hasNext) {
            opts.outPath = argv[++i];
        } else if (arg == "--size" && hasNext) {
            std::sscanf(argv[++i], "%dx%d", &opts.width, &opts.height);
        } else if (arg == "--frames" && hasNext) {
            opts.frames = std::atoi(argv[++i]);
        } else if (arg == "--xr") {
            opts.xr = true;
        } else if (arg == "--probe") {
            opts.probe = true;
        } else if (arg == "--dist" && hasNext) {
            opts.anchorDist = std::stof(argv[++i]);
        } else if (arg == "--height" && hasNext) {
            opts.anchorHeight = std::stof(argv[++i]);
        } else if (arg == "--renderer" && hasNext) {
            opts.renderer = argv[++i];
        } else if (arg == "--max-res" && hasNext) {
            std::sscanf(argv[++i], "%dx%d", &opts.maxWidth, &opts.maxHeight);
        } else if (arg == "--converge" && hasNext) {
            opts.converge = std::stof(argv[++i]);
        } else if (arg == "--frozen") {
            opts.frozen = true;
        } else if (arg == "--pick") {
            opts.pick = true;
        } else if (arg == "--reticle") {
            opts.reticle = true;
        } else {
            std::printf("usage: hxr [--stage file.usd] [--xr] [--probe] [--frames N]"
                        " [--renderer PluginId] [--max-res WxH] [--converge S] [--frozen]"
                        " [--pick] [--reticle]"
                        " [--dist M] [--height M] [--out image.bmp] [--size WxH]\n");
        }
    }
    return opts;
}

UsdStageRefPtr MakeSmokeTestStage()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSphere sphere = UsdGeomSphere::Define(stage, SdfPath("/smokeTestSphere"));
    sphere.CreateRadiusAttr().Set(1.0);
    return stage;
}

bool WriteBmp(std::string const& path, std::vector<uint8_t> const& rgba, int w, int h)
{
    const int rowBytes   = w * 3;
    const int padding    = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * h;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    uint8_t header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    auto put16 = [&](int at, uint16_t v) { std::memcpy(header + at, &v, 2); };
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2,  uint32_t(54 + imageBytes));
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(w));
    put32(22, uint32_t(h));
    put16(26, 1);
    put16(28, 24);
    put32(34, uint32_t(imageBytes));
    out.write(reinterpret_cast<char*>(header), sizeof(header));

    std::vector<uint8_t> row(size_t(rowBytes + padding), 0);
    for (int y = h - 1; y >= 0; --y) {   // BMP scanlines run bottom-up
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            row[size_t(x) * 3 + 0] = px[2];
            row[size_t(x) * 3 + 1] = px[1];
            row[size_t(x) * 3 + 2] = px[0];
        }
        out.write(reinterpret_cast<char*>(row.data()), std::streamsize(row.size()));
    }

    return out.good();
}

uint8_t EncodeSrgb8(float linear)
{
    const float c = std::fmin(std::fmax(linear, 0.0f), 1.0f);
    const float s = (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

uint8_t EncodeLinear8(float v)
{
    return uint8_t(std::fmin(std::fmax(v, 0.0f), 1.0f) * 255.0f + 0.5f);
}

// The offscreen twin of XrPresenterGL::PresentEye: blit the colour AOV into a
// target of our own, draw the reticle with the same DrawReticle the headset
// uses, read it back. Rows come back top-down and sRGB-encoded, for WriteBmp.
bool ComposeWithReticle(HydraRenderer& renderer, float ndcX, float ndcY,
                        std::vector<uint8_t>& rgba, int& w, int& h)
{
    const HydraRenderer::ColorTexture colour = renderer.GetColorTexture(0);
    if (!colour.id) {
        return false;
    }
    w = colour.width;
    h = colour.height;

    // Float target: the blit copies Hydra's linear values untouched, leaving
    // one sRGB encode below -- the same single encode the headset's sRGB
    // swapchain performs.
    GLuint target = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint readFbo = 0;
    GLuint drawFbo = 0;
    glGenFramebuffers(1, &readFbo);
    glGenFramebuffers(1, &drawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour.id, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);

    glDisable(GL_SCISSOR_TEST);   // blits honour it; see XrPresenterGL
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    DrawReticle(w, h, ndcX, ndcY);

    std::vector<float> texels(size_t(w) * size_t(h) * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, drawFbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, texels.data());

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &readFbo);
    glDeleteFramebuffers(1, &drawFbo);
    glDeleteTextures(1, &target);

    // glReadPixels returns the bottom row first; WriteBmp wants the top first.
    rgba.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        const float* src = &texels[size_t(h - 1 - y) * size_t(w) * 4];
        uint8_t*     dst = &rgba[size_t(y) * size_t(w) * 4];
        for (int i = 0; i < w * 4; ++i) {
            dst[i] = (i % 4 == 3) ? EncodeLinear8(src[i]) : EncodeSrgb8(src[i]);
        }
    }
    return true;
}

int RunOffscreen(Options const& opts, UsdStageRefPtr const& stage)
{
    HydraRenderer renderer;
    renderer.Init(GfVec2i(opts.width, opts.height), /*viewCount*/ 1);
    renderer.SetRendererPlugin(TfToken(opts.renderer));
    if (!renderer.SetStage(stage)) {
        return 1;
    }
    std::printf("Renderer: %s\n", renderer.RendererName().c_str());

    GfMatrix4d view;
    view.SetLookAt(GfVec3d(0.0, 1.5, 6.0), GfVec3d(0.0, 0.0, 0.0), GfVec3d(0.0, 1.0, 0.0));

    GfFrustum frustum;
    frustum.SetPerspective(60.0, double(opts.width) / double(opts.height), 0.1, 1000.0);

    // Reticle, as the headset places it: pick the gaze ray (here the view
    // centre), fall back to a fixed distance on a miss, project the result
    // back into the view. Picked before rendering, mirroring the headset's
    // pick -> render -> present order, since a pick is itself a render.
    float reticleNdcX = 0.0f;
    float reticleNdcY = 0.0f;
    if (opts.reticle) {
        GfFrustum pickFrustum;
        pickFrustum.SetPerspective(0.5, 1.0, 0.1, 1000.0);
        const GfMatrix4d camToWorld = view.GetInverse();

        GfVec3d    aim;
        const bool onSurface =
            renderer.Pick(view, pickFrustum.ComputeProjectionMatrix(), 0.0, &aim);
        if (!onSurface) {
            aim = camToWorld.ExtractTranslation() +
                  camToWorld.TransformDir(GfVec3d(0.0, 0.0, -1.0)) * 2.0;
        }

        const GfVec4d clip = GfVec4d(aim[0], aim[1], aim[2], 1.0) *
                             (view * frustum.ComputeProjectionMatrix());
        reticleNdcX = float(clip[0] / clip[3]);
        reticleNdcY = float(clip[1] / clip[3]);
        std::printf("Reticle %s at (%.3f, %.3f, %.3f), NDC (%.3f, %.3f)\n",
                    onSurface ? "on surface" : "at miss distance",
                    aim[0], aim[1], aim[2], reticleNdcX, reticleNdcY);
    }

    // Progressive delegates need repeated passes at a fixed camera; keep
    // going until converged or the convergence budget is spent.
    const auto start = std::chrono::steady_clock::now();
    int passes = 0;
    do {
        renderer.RenderEye(0, view, frustum.ComputeProjectionMatrix());
        ++passes;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= double(opts.converge) && !opts.frozen) {
            break;
        }
    } while (!renderer.IsConverged(0));
    std::printf("Converged: %s after %d pass%s\n", renderer.IsConverged(0) ? "yes" : "no",
                passes, passes == 1 ? "" : "es");

    std::vector<uint8_t> pixels;
    int w = 0;
    int h = 0;
    const bool read = opts.reticle
                          ? ComposeWithReticle(renderer, reticleNdcX, reticleNdcY, pixels, w, h)
                          : renderer.ReadColor(0, pixels, w, h);
    if (!read) {
        return 1;
    }
    if (!WriteBmp(opts.outPath, pixels, w, h)) {
        std::fprintf(stderr, "Could not write %s\n", opts.outPath.c_str());
        return 1;
    }

    std::printf("Wrote %dx%d -> %s\n", w, h, opts.outPath.c_str());
    const HydraRenderer::ColorTexture colour = renderer.GetColorTexture(0);
    std::printf("Colour AOV GL texture id: %u (%dx%d)\n", colour.id, colour.width, colour.height);

    // The same narrow-frustum pick the headset reticle uses, down the view
    // centre. Verifies without hardware that hits come back in stage space.
    // For the built-in sphere (r=1 at the origin, camera at (0,1.5,6)) the
    // expected front-surface hit is (0, 0.243, 0.970).
    if (opts.pick) {
        GfFrustum pickFrustum;
        pickFrustum.SetPerspective(0.5, 1.0, 0.1, 1000.0);
        GfVec3d hit;
        if (renderer.Pick(view, pickFrustum.ComputeProjectionMatrix(), 0.0, &hit)) {
            std::printf("Pick hit (stage space): (%.3f, %.3f, %.3f)\n", hit[0], hit[1], hit[2]);
        } else {
            std::printf("Pick: no hit\n");
        }
    }

    return 0;
}

// This function's whole job is the standalone-exe half of the runtime's
// contract: block until it's done, and surface failures on stdout with a
// process exit code. The Houdini plugin is the other caller of HxrRuntime --
// it does neither, since a LOP node's cookMyLop must return immediately and
// report problems through the node itself, not through a process exit code.
int RunXr(Options const& opts, UsdStageRefPtr const& stage)
{
    HxrRuntime runtime;
    runtime.SetRendererPlugin(opts.renderer);
    runtime.SetMaxRenderSize(opts.maxWidth, opts.maxHeight);
    runtime.SetConvergeSeconds(opts.converge);
    runtime.SetFrozen(opts.frozen);
    runtime.SetStage(stage);
    runtime.SetAnchor(opts.anchorDist, opts.anchorHeight);
    runtime.Start();

    std::printf("Anchor: %.1fm in front, %.1fm up (--dist / --height to adjust)\n",
                opts.anchorDist, opts.anchorHeight);

    // Give ThreadMain a chance to fail fast (no runtime, no headset) before
    // settling into the poll loop below.
    while (!runtime.IsRunning() && !runtime.HasFailed()) {
        Sleep(10);
    }
    if (runtime.HasFailed()) {
        return 1;
    }

    while (runtime.IsRunning() &&
           (opts.frames <= 0 || runtime.FramesPresented() < opts.frames)) {
        Sleep(20);
    }

    runtime.Stop();
    std::printf("Presented %d frames.\n", runtime.FramesPresented());

    return runtime.HasFailed() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    // Houdini's DLLs must win over same-named system copies: Windows 11 ships
    // its own, older onnxruntime.dll in System32, which Karma's dependencies
    // would otherwise load and reject. System32 is searched before PATH, but
    // SetDllDirectory slots in ahead of it. houdini.exe never needs this
    // because $HFS/bin is its own directory, which is searched first of all.
    wchar_t hfs[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"HFS", hfs, MAX_PATH) > 0) {
        SetDllDirectoryW((std::wstring(hfs) + L"\\bin").c_str());
    }

    const Options opts = ParseArgs(argc, argv);

    if (opts.probe) {
        return RunProbe();
    }

    UsdStageRefPtr stage;
    if (opts.stagePath.empty()) {
        std::printf("No --stage given; using in-memory smoke-test sphere.\n");
        stage = MakeSmokeTestStage();
    } else {
        stage = UsdStage::Open(opts.stagePath);
        if (!stage) {
            std::fprintf(stderr, "Could not open stage: %s\n", opts.stagePath.c_str());
            return 1;
        }
    }

    // HxrRuntime creates and owns its GL context internally on its own
    // thread, so the XR path needs no context set up here on the main thread.
    if (opts.xr) {
        return RunXr(opts, stage);
    }

    GLContext gl;
    if (!gl.Create()) {
        return 1;
    }
    gl.MakeCurrent();

    if (!GarchGLApiLoad()) {
        std::fprintf(stderr, "GarchGLApiLoad failed -- no usable GL context\n");
        return 1;
    }

    return RunOffscreen(opts, stage);
}
