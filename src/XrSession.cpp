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

    if (!_presenter.CreateSwapchains(_session, _eyeWidth, _eyeHeight, viewCount)) {
        return false;
    }

    // Controller input is a convenience, not a requirement: a runtime with no
    // controller profile still gets a working (trigger-less) session.
    if (!_InitInput()) {
        std::fprintf(stderr, "Controller input unavailable; trigger override disabled\n");
    }
    return true;
}

bool XrViewportSession::_InitInput()
{
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "hxr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "Houdini XR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (Failed(xrCreateActionSet(_instance, &setInfo, &_actionSet), "xrCreateActionSet")) {
        return false;
    }

    // A float action rather than boolean: Touch exposes the trigger as
    // /trigger/value with no /click, and a boolean source can still be bound
    // to a float action where a profile only has that.
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(actionInfo.actionName, "interactive_placement", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(actionInfo.localizedActionName, "Interactive Placement",
                 XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &actionInfo, &_triggerAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo stickInfo{XR_TYPE_ACTION_CREATE_INFO};
    stickInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(stickInfo.actionName, "move", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(stickInfo.localizedActionName, "Move", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &stickInfo, &_thumbstickAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo clickInfo{XR_TYPE_ACTION_CREATE_INFO};
    clickInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(clickInfo.actionName, "place_camera", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(clickInfo.localizedActionName, "Place Camera", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &clickInfo, &_thumbClickAction), "xrCreateAction")) {
        return false;
    }

    auto path = [this](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(_instance, s, &p);
        return p;
    };

    // Suggest per profile; a runtime ignores profiles it doesn't know. The
    // trigger action binds to both hands; the stick is right-hand only. The
    // Khronos simple profile has no thumbstick, so it only gets the trigger.
    bool anyAccepted = false;

    {
        const XrActionSuggestedBinding bindings[] = {
            {_triggerAction,    path("/user/hand/left/input/trigger/value")},
            {_triggerAction,    path("/user/hand/right/input/trigger/value")},
            {_thumbstickAction, path("/user/hand/right/input/thumbstick")},
            {_thumbClickAction, path("/user/hand/right/input/thumbstick/click")},
        };
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile     = path("/interaction_profiles/oculus/touch_controller");
        suggested.countSuggestedBindings = 4;
        suggested.suggestedBindings      = bindings;
        anyAccepted |= XR_SUCCEEDED(xrSuggestInteractionProfileBindings(_instance, &suggested));
    }
    {
        const XrActionSuggestedBinding bindings[] = {
            {_triggerAction, path("/user/hand/left/input/select/click")},
            {_triggerAction, path("/user/hand/right/input/select/click")},
        };
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile     = path("/interaction_profiles/khr/simple_controller");
        suggested.countSuggestedBindings = 2;
        suggested.suggestedBindings      = bindings;
        anyAccepted |= XR_SUCCEEDED(xrSuggestInteractionProfileBindings(_instance, &suggested));
    }
    if (!anyAccepted) {
        std::fprintf(stderr, "No interaction profile accepted the trigger bindings\n");
        return false;
    }

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets      = &_actionSet;
    return !Failed(xrAttachSessionActionSets(_session, &attachInfo), "xrAttachSessionActionSets");
}

void XrViewportSession::_SyncInput()
{
    _triggerValue           = 0.0f;
    _rightThumbstick        = {0.0f, 0.0f};
    _rightThumbstickPressed = false;
    if (_actionSet == XR_NULL_HANDLE) {
        return;
    }

    XrActiveActionSet active{_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets      = &active;
    // XR_SESSION_NOT_FOCUSED is routine (another app has input); not an error.
    if (XR_FAILED(xrSyncActions(_session, &syncInfo))) {
        return;
    }

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};

    getInfo.action = _triggerAction;
    XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(_session, &getInfo, &trigger)) && trigger.isActive) {
        _triggerValue = trigger.currentState;
    }

    getInfo.action = _thumbstickAction;
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(_session, &getInfo, &stick)) && stick.isActive) {
        _rightThumbstick = stick.currentState;
    }

    // Edge, not level: the runtime tracks changes between syncs, so this is
    // true for exactly one frame per press.
    getInfo.action = _thumbClickAction;
    XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &click)) && click.isActive) {
        _rightThumbstickPressed = click.changedSinceLastSync && click.currentState;
    }
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
    _SyncInput();

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

    // Destroying the set destroys its actions.
    if (_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(_actionSet);
        _actionSet        = XR_NULL_HANDLE;
        _triggerAction    = XR_NULL_HANDLE;
        _thumbstickAction = XR_NULL_HANDLE;
        _thumbClickAction = XR_NULL_HANDLE;
    }

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
