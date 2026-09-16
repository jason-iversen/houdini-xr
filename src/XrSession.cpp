#include "XrSession.h"

#include "GLContext.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr XrViewConfigurationType kViewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

bool Failed(XrResult result, const char* what)
{
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "%s failed (XrResult %d)\n", what, int(result));
        return true;
    }
    return false;
}

} // namespace

bool XrViewportSession::Init(GLContext const& gl)
{
    const bool ok = _InitImpl(gl);
    if (!ok) {
        Shutdown();
    }
    return ok;
}

bool XrViewportSession::_InitImpl(GLContext const& gl)
{
    const char* extensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    std::strncpy(instanceInfo.applicationInfo.applicationName, "hxr",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(instanceInfo.applicationInfo.engineName, "hydra-storm",
                 XR_MAX_ENGINE_NAME_SIZE - 1);

    const XrResult created = xrCreateInstance(&instanceInfo, &_instance);
    if (Failed(created, "xrCreateInstance")) {
        if (created == XR_ERROR_LIMIT_REACHED) {
            // The loader allows one XrInstance per process. Failed Inits
            // clean up after themselves, so reaching this means another
            // session is genuinely alive -- e.g. a second XR Output node
            // with Live on.
            std::fprintf(stderr, "  (an XR session already exists in this process; "
                                 "only one can run at a time)\n");
        } else {
            std::fprintf(stderr, "  (run with --probe to list what the active runtime supports)\n");
        }
        return false;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (Failed(xrGetSystem(_instance, &systemInfo, &_systemId), "xrGetSystem")) {
        return false;
    }

    // The spec requires this call before xrCreateSession, even though we ignore
    // the reported version range.
    PFN_xrGetOpenGLGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
    if (Failed(xrGetInstanceProcAddr(_instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getGraphicsRequirements)),
               "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (Failed(getGraphicsRequirements(_instance, _systemId, &requirements),
               "xrGetOpenGLGraphicsRequirementsKHR")) {
        return false;
    }

    XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC   = gl.Dc();
    binding.hGLRC = gl.Rc();

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next     = &binding;
    sessionInfo.systemId = _systemId;
    if (Failed(xrCreateSession(_instance, &sessionInfo, &_session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (XR_FAILED(xrCreateReferenceSpace(_session, &spaceInfo, &_space))) {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (Failed(xrCreateReferenceSpace(_session, &spaceInfo, &_space),
                   "xrCreateReferenceSpace")) {
            return false;
        }
        std::printf("Reference space: LOCAL (STAGE unavailable)\n");
    } else {
        std::printf("Reference space: STAGE\n");
    }

    uint32_t viewCount = 0;
    if (Failed(xrEnumerateViewConfigurationViews(_instance, _systemId, kViewConfig,
                                                 0, &viewCount, nullptr),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    _viewConfigs.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (Failed(xrEnumerateViewConfigurationViews(_instance, _systemId, kViewConfig,
                                                 viewCount, &viewCount, _viewConfigs.data()),
               "xrEnumerateViewConfigurationViews")) {
        return false;
    }
    _views.assign(viewCount, {XR_TYPE_VIEW});

    _eyeWidth  = _viewConfigs[0].recommendedImageRectWidth;
    _eyeHeight = _viewConfigs[0].recommendedImageRectHeight;

    return _presenter.CreateSwapchains(_session, _eyeWidth, _eyeHeight, viewCount);
}

bool XrViewportSession::PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

    while (xrPollEvent(_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            _quit = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);

            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = kViewConfig;
                if (!Failed(xrBeginSession(_session, &beginInfo), "xrBeginSession")) {
                    _running = true;
                    std::printf("Session running.\n");
                }
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                _running = false;
                xrEndSession(_session);
            } else if (changed.state == XR_SESSION_STATE_EXITING ||
                       changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                _quit = true;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    return !_quit;
}

bool XrViewportSession::RenderFrame(RenderEyeFn const& renderEye)
{
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (Failed(xrWaitFrame(_session, &waitInfo, &frameState), "xrWaitFrame")) {
        return false;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (Failed(xrBeginFrame(_session, &beginInfo), "xrBeginFrame")) {
        return false;
    }

    std::vector<XrCompositionLayerProjectionView> projectionViews;

    if (frameState.shouldRender == XR_TRUE) {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = kViewConfig;
        locateInfo.displayTime           = frameState.predictedDisplayTime;
        locateInfo.space                 = _space;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t located = 0;
        const XrResult result = xrLocateViews(_session, &locateInfo, &viewState,
                                              uint32_t(_views.size()), &located, _views.data());

        const bool posesValid =
            XR_SUCCEEDED(result) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

        if (posesValid) {
            projectionViews.resize(located);

            for (uint32_t i = 0; i < located; ++i) {
                const EyeImage image = renderEye(i, _views[i]);
                if (!_presenter.PresentEye(i, image.texture, image.width, image.height)) {
                    return false;
                }

                projectionViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projectionViews[i].pose = image.pose;
                projectionViews[i].fov  = image.fov;
                projectionViews[i].subImage.swapchain = _presenter.Swapchain(i);
                projectionViews[i].subImage.imageRect.offset = {0, 0};
                projectionViews[i].subImage.imageRect.extent = {int32_t(_eyeWidth),
                                                                int32_t(_eyeHeight)};
                projectionViews[i].subImage.imageArrayIndex = 0;
            }
        }
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space      = _space;
    layer.viewCount  = uint32_t(projectionViews.size());
    layer.views      = projectionViews.data();

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime          = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount           = projectionViews.empty() ? 0u : 1u;
    endInfo.layers               = layers;

    return !Failed(xrEndFrame(_session, &endInfo), "xrEndFrame");
}

void XrViewportSession::Shutdown()
{
    _presenter.Destroy();

    if (_space != XR_NULL_HANDLE) {
        xrDestroySpace(_space);
        _space = XR_NULL_HANDLE;
    }
    if (_session != XR_NULL_HANDLE) {
        xrDestroySession(_session);
        _session = XR_NULL_HANDLE;
    }
    if (_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(_instance);
        _instance = XR_NULL_HANDLE;
    }
}
