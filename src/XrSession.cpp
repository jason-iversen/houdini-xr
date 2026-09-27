#include "XrSession.h"

#include "GLContext.h"

#include <cstdio>
#include <cstring>
#include <iterator>

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
    // controller profile still gets a working (controller-less) session.
    if (!_InitInput()) {
        std::fprintf(stderr, "Controller input unavailable; controls disabled\n");
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

    auto path = [this](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(_instance, s, &p);
        return p;
    };

    // The trigger, grip and aim actions declare both hands as subaction
    // paths, so each hand can be read on its own. Queried without one, a
    // float action resolves to whichever hand is pulled further.
    _leftHand  = path("/user/hand/left");
    _rightHand = path("/user/hand/right");
    const XrPath hands[] = {_leftHand, _rightHand};

    // A float action rather than boolean: Touch exposes the trigger as
    // /trigger/value with no /click, and a boolean source can still be bound
    // to a float action where a profile only has that.
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    actionInfo.countSubactionPaths = uint32_t(std::size(hands));
    actionInfo.subactionPaths      = hands;
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

    XrActionCreateInfo turnInfo{XR_TYPE_ACTION_CREATE_INFO};
    turnInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(turnInfo.actionName, "turn", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(turnInfo.localizedActionName, "Turn", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &turnInfo, &_turnAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo clickInfo{XR_TYPE_ACTION_CREATE_INFO};
    clickInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(clickInfo.actionName, "place_camera", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(clickInfo.localizedActionName, "Place Camera", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &clickInfo, &_thumbClickAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo menuInfo{XR_TYPE_ACTION_CREATE_INFO};
    menuInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(menuInfo.actionName, "toggle_desktop", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(menuInfo.localizedActionName, "Toggle Desktop", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &menuInfo, &_menuAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo downInfo{XR_TYPE_ACTION_CREATE_INFO};
    downInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(downInfo.actionName, "move_down", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(downInfo.localizedActionName, "Move Down", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &downInfo, &_buttonAAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo upInfo{XR_TYPE_ACTION_CREATE_INFO};
    upInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(upInfo.actionName, "move_up", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(upInfo.localizedActionName, "Move Up", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &upInfo, &_buttonBAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo xInfo{XR_TYPE_ACTION_CREATE_INFO};
    xInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(xInfo.actionName, "toggle_renderer", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(xInfo.localizedActionName, "Toggle Renderer", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &xInfo, &_buttonXAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo yInfo{XR_TYPE_ACTION_CREATE_INFO};
    yInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(yInfo.actionName, "toggle_stereo", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(yInfo.localizedActionName, "Toggle Stereo", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &yInfo, &_buttonYAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo gripInfo{XR_TYPE_ACTION_CREATE_INFO};
    gripInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    gripInfo.countSubactionPaths = uint32_t(std::size(hands));
    gripInfo.subactionPaths      = hands;
    std::strncpy(gripInfo.actionName, "grip", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(gripInfo.localizedActionName, "Grip", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &gripInfo, &_gripAction), "xrCreateAction")) {
        return false;
    }

    XrActionCreateInfo aimInfo{XR_TYPE_ACTION_CREATE_INFO};
    aimInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aimInfo.countSubactionPaths = uint32_t(std::size(hands));
    aimInfo.subactionPaths      = hands;
    std::strncpy(aimInfo.actionName, "aim", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aimInfo.localizedActionName, "Aim", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (Failed(xrCreateAction(_actionSet, &aimInfo, &_aimPoseAction), "xrCreateAction")) {
        return false;
    }

    // Suggest per profile; a runtime ignores profiles it doesn't know. The
    // trigger, grip and aim actions bind to both hands; move is the right stick,
    // turn the left. The Khronos simple profile has no thumbstick, so it only gets
    // the trigger.
    bool anyAccepted = false;

    {
        const XrActionSuggestedBinding bindings[] = {
            {_triggerAction,    path("/user/hand/left/input/trigger/value")},
            {_triggerAction,    path("/user/hand/right/input/trigger/value")},
            {_thumbstickAction, path("/user/hand/right/input/thumbstick")},
            {_turnAction,       path("/user/hand/left/input/thumbstick")},
            {_thumbClickAction, path("/user/hand/right/input/thumbstick/click")},
            {_menuAction,       path("/user/hand/left/input/menu/click")},
            {_buttonAAction,    path("/user/hand/right/input/a/click")},
            {_buttonBAction,    path("/user/hand/right/input/b/click")},
            {_gripAction,       path("/user/hand/right/input/squeeze/value")},
            {_gripAction,       path("/user/hand/left/input/squeeze/value")},
            {_buttonXAction,    path("/user/hand/left/input/x/click")},
            {_buttonYAction,    path("/user/hand/left/input/y/click")},
            {_aimPoseAction,    path("/user/hand/right/input/aim/pose")},
            {_aimPoseAction,    path("/user/hand/left/input/aim/pose")},
        };
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile     = path("/interaction_profiles/oculus/touch_controller");
        suggested.countSuggestedBindings = uint32_t(std::size(bindings));
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
    if (Failed(xrAttachSessionActionSets(_session, &attachInfo), "xrAttachSessionActionSets")) {
        return false;
    }

    // A pose action is read by locating a space bound to it, not by
    // xrGetActionState -- one space per hand. Failing here only loses orbit
    // (right) or scrub (left); the rest still works.
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = _aimPoseAction;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    spaceInfo.subactionPath = _rightHand;
    if (Failed(xrCreateActionSpace(_session, &spaceInfo, &_rightAimSpace), "xrCreateActionSpace")) {
        _rightAimSpace = XR_NULL_HANDLE;
    }
    spaceInfo.subactionPath = _leftHand;
    if (Failed(xrCreateActionSpace(_session, &spaceInfo, &_leftAimSpace), "xrCreateActionSpace")) {
        _leftAimSpace = XR_NULL_HANDLE;
    }
    return true;
}

bool XrViewportSession::_LocateAim(XrSpace space, XrTime time, XrPosef* pose) const
{
    if (space == XR_NULL_HANDLE) {
        return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, _space, time, &location))) {
        return false;
    }
    const XrSpaceLocationFlags needed =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & needed) != needed) {
        return false;
    }
    *pose = location.pose;
    return true;
}

void XrViewportSession::_LocateControllers(XrTime time)
{
    _rightAimValid = _LocateAim(_rightAimSpace, time, &_rightAimPose);
    _leftAimValid  = _LocateAim(_leftAimSpace, time, &_leftAimPose);
}

void XrViewportSession::_SyncInput()
{
    _triggerValue           = 0.0f;
    _leftGripValue          = 0.0f;
    _buttonXPressed         = false;
    _buttonYPressed         = false;
    _leftTriggerPressed     = false;
    _gripValue              = 0.0f;
    _rightThumbstick        = {0.0f, 0.0f};
    _leftThumbstick         = {0.0f, 0.0f};
    _rightThumbstickPressed = false;
    _leftMenuPressed        = false;
    _buttonA                = false;
    _buttonB                = false;
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

    // The left trigger alone, as a click. Inactive input reads as released.
    getInfo.subactionPath = _leftHand;
    XrActionStateFloat leftTrigger{XR_TYPE_ACTION_STATE_FLOAT};
    const float leftValue =
        (XR_SUCCEEDED(xrGetActionStateFloat(_session, &getInfo, &leftTrigger)) &&
         leftTrigger.isActive) ? leftTrigger.currentState : 0.0f;
    getInfo.subactionPath = XR_NULL_PATH;
    if (!_leftTriggerDown && leftValue > 0.75f) {
        _leftTriggerDown    = true;
        _leftTriggerPressed = true;
    } else if (_leftTriggerDown && leftValue < 0.25f) {
        _leftTriggerDown = false;
    }

    getInfo.action = _thumbstickAction;
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(_session, &getInfo, &stick)) && stick.isActive) {
        _rightThumbstick = stick.currentState;
    }

    getInfo.action = _turnAction;
    XrActionStateVector2f turn{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(_session, &getInfo, &turn)) && turn.isActive) {
        _leftThumbstick = turn.currentState;
    }

    // Edge, not level: the runtime tracks changes between syncs, so this is
    // true for exactly one frame per press.
    getInfo.action = _thumbClickAction;
    XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &click)) && click.isActive) {
        _rightThumbstickPressed = click.changedSinceLastSync && click.currentState;
    }

    getInfo.action = _menuAction;
    XrActionStateBoolean menu{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &menu)) && menu.isActive) {
        _leftMenuPressed = menu.changedSinceLastSync && menu.currentState;
    }

    getInfo.action = _buttonAAction;
    XrActionStateBoolean buttonA{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &buttonA)) && buttonA.isActive) {
        _buttonA = buttonA.currentState != XR_FALSE;
    }
    getInfo.action = _buttonBAction;
    XrActionStateBoolean buttonB{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &buttonB)) && buttonB.isActive) {
        _buttonB = buttonB.currentState != XR_FALSE;
    }

    getInfo.action = _buttonXAction;
    XrActionStateBoolean buttonX{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &buttonX)) && buttonX.isActive) {
        _buttonXPressed = buttonX.changedSinceLastSync && buttonX.currentState;
    }
    getInfo.action = _buttonYAction;
    XrActionStateBoolean buttonY{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_session, &getInfo, &buttonY)) && buttonY.isActive) {
        _buttonYPressed = buttonY.changedSinceLastSync && buttonY.currentState;
    }

    // Per hand, through the subaction paths.
    getInfo.action = _gripAction;
    getInfo.subactionPath = _rightHand;
    XrActionStateFloat grip{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(_session, &getInfo, &grip)) && grip.isActive) {
        _gripValue = grip.currentState;
    }
    getInfo.subactionPath = _leftHand;
    XrActionStateFloat leftGrip{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_SUCCEEDED(xrGetActionStateFloat(_session, &getInfo, &leftGrip)) && leftGrip.isActive) {
        _leftGripValue = leftGrip.currentState;
    }
    getInfo.subactionPath = XR_NULL_PATH;
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

    // Needs the predicted display time, so it can't live in _SyncInput.
    _LocateControllers(frameState.predictedDisplayTime);

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (Failed(xrBeginFrame(_session, &beginInfo), "xrBeginFrame")) {
        return false;
    }

    std::vector<XrCompositionLayerProjectionView> projectionViews;
    bool viewsRendered = false;   // this frame's views were located and rendered

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
            const bool billboard = _billboard.visible;
            if (!billboard) {
                projectionViews.resize(located);
            }

            for (uint32_t i = 0; i < located; ++i) {
                const EyeImage image = renderEye(i, _views[i]);
                if (billboard) {
                    continue;
                }
                if (!_presenter.PresentEye(i, image.texture, image.width, image.height,
                                           image.reticleVisible,
                                           image.reticleNdcX, image.reticleNdcY)) {
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
            viewsRendered = true;
        }
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space      = _space;
    layer.viewCount  = uint32_t(projectionViews.size());
    layer.views      = projectionViews.data();

    const XrCompositionLayerBaseHeader* layers[3] = {};
    uint32_t layerCount = 0;
    if (!projectionViews.empty()) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    }

    auto quadLayer = [this](XrCompositionLayerQuad& quad, XrPresenterGL::Quad which,
                            XrPosef const& pose, XrExtent2Df const& size) {
        quad.space         = _space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = _presenter.QuadSwapchain(which);
        quad.subImage.imageRect.offset = {0, 0};
        quad.subImage.imageRect.extent = {int32_t(_presenter.QuadWidth(which)),
                                          int32_t(_presenter.QuadHeight(which))};
        quad.pose = pose;
        quad.size = size;
    };

    // In place of the projection layer, when single-eye rendering is on.
    XrCompositionLayerQuad billboardQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    EyeImage const&        billboardImage = _billboard.image;
    if (viewsRendered && _billboard.visible && billboardImage.texture != 0 &&
        billboardImage.width > 0 && billboardImage.height > 0) {
        bool created = false;
        if (_presenter.EnsureQuadSwapchain(XrPresenterGL::Quad::Billboard,
                                           uint32_t(billboardImage.width),
                                           uint32_t(billboardImage.height), &created) &&
            _presenter.PresentQuad(XrPresenterGL::Quad::Billboard, [&](int w, int h) {
                _presenter.BlitLinear(billboardImage.texture, billboardImage.width,
                                      billboardImage.height, w, h,
                                      billboardImage.reticleVisible,
                                      billboardImage.reticleNdcX, billboardImage.reticleNdcY);
            })) {
            quadLayer(billboardQuad, XrPresenterGL::Quad::Billboard, _billboard.pose,
                      _billboard.size);
            layers[layerCount++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&billboardQuad);
        }
    }

    // The panel goes last, so it draws on top -- even over scene geometry
    // that's nearer. A panel failure only drops the panel; the scene still
    // presents.
    XrCompositionLayerQuad panelQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (viewsRendered && _panel.visible && _panel.draw && _panel.width > 0 && _panel.height > 0) {
        bool created = false;
        bool ready   = _presenter.EnsureQuadSwapchain(XrPresenterGL::Quad::Panel, _panel.width,
                                                      _panel.height, &created);
        if (ready && (created || _panelDirty)) {
            ready       = _presenter.PresentQuad(XrPresenterGL::Quad::Panel, _panel.draw);
            _panelDirty = !ready;
        }
        if (ready) {
            quadLayer(panelQuad, XrPresenterGL::Quad::Panel, _panel.pose,
                      {_panel.widthMetres,
                       _panel.widthMetres * float(_panel.height) / float(_panel.width)});
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panelQuad);
        }
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime          = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount           = layerCount;
    endInfo.layers               = layers;

    return !Failed(xrEndFrame(_session, &endInfo), "xrEndFrame");
}

void XrViewportSession::Shutdown()
{
    _presenter.Destroy();

    if (_rightAimSpace != XR_NULL_HANDLE) {
        xrDestroySpace(_rightAimSpace);
        _rightAimSpace = XR_NULL_HANDLE;
    }
    if (_leftAimSpace != XR_NULL_HANDLE) {
        xrDestroySpace(_leftAimSpace);
        _leftAimSpace = XR_NULL_HANDLE;
    }

    // Destroying the set destroys its actions.
    if (_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(_actionSet);
        _actionSet        = XR_NULL_HANDLE;
        _triggerAction    = XR_NULL_HANDLE;
        _thumbstickAction = XR_NULL_HANDLE;
        _turnAction       = XR_NULL_HANDLE;
        _thumbClickAction = XR_NULL_HANDLE;
        _menuAction       = XR_NULL_HANDLE;
        _buttonXAction    = XR_NULL_HANDLE;
        _buttonYAction    = XR_NULL_HANDLE;
        _buttonAAction    = XR_NULL_HANDLE;
        _buttonBAction    = XR_NULL_HANDLE;
        _gripAction       = XR_NULL_HANDLE;
        _aimPoseAction    = XR_NULL_HANDLE;
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
