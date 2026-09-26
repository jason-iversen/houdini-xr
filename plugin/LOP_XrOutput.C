#include "LOP_XrOutput.h"
#include "HydraRenderer.h"
#include "RenderCamera.h"

#include <CH/CH_Manager.h>
#include <HUSD/HUSD_DataHandle.h>
#include <HUSD/HUSD_RendererInfo.h>
#include <OP/OP_DataMicroNode.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_Include.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_HoudiniExecutionContext.h>

// Silences C4003 (macro too few args), which fires from USD's own macros
// under MSVC -- same guard SideFX uses in their own LOP samples.
#include <pxr/base/arch/pragmas.h>
ARCH_PRAGMA_PUSH
ARCH_PRAGMA_MACRO_TOO_FEW_ARGUMENTS
#include <HUSD/XUSD_Data.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/xformOp.h>
#include <pxr/usd/usdRender/settings.h>
ARCH_PRAGMA_POP

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Equivalent to hou.applicationName() == "happrentice", without a round trip
// through Python: Apprentice runs as its own executable.
bool RunningAsApprentice()
{
    static const bool apprentice = [] {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring exe(path);
        const size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            exe = exe.substr(slash + 1);
        }
        std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
        return exe == L"happrentice.exe";
    }();
    return apprentice;
}

// BRAY_HdKarma and BRAY_HdKarmaXPU.
bool IsKarma(std::string const& pluginId)
{
    return pluginId.rfind("BRAY_HdKarma", 0) == 0;
}

// Tightest of two caps, where 0 means uncapped.
int TightestCap(int a, int b)
{
    if (a <= 0) return b;
    if (b <= 0) return a;
    return std::min(a, b);
}

// Apprentice refuses Karma renders above this rather than clamping them.
constexpr int kApprenticeKarmaMaxWidth  = 1280;
constexpr int kApprenticeKarmaMaxHeight = 720;

// Placement rotate parms are XYZ Euler in Gf's row-vector composition:
// R = Rx * Ry * Rz. Decompose and recompose must agree on this, so both go
// through here.
GfMatrix4d RotationFromEuler(GfVec3d const& degrees)
{
    GfMatrix4d m;
    m.SetRotate(GfRotation(GfVec3d::XAxis(), degrees[0]) *
                GfRotation(GfVec3d::YAxis(), degrees[1]) *
                GfRotation(GfVec3d::ZAxis(), degrees[2]));
    return m;
}

GfVec3d EulerFromMatrix(GfMatrix4d const& m)
{
    // ExtractRotation wants orthonormal axes; the stage-space camera matrix
    // carries the inverse of the stage's metersPerUnit as a uniform scale.
    GfMatrix4d rotation = m;
    rotation.SetTranslateOnly(GfVec3d(0.0));
    rotation.Orthonormalize(/*issueWarning*/ false);
    return rotation.ExtractRotation().Decompose(
        GfVec3d::XAxis(), GfVec3d::YAxis(), GfVec3d::ZAxis());
}

} // namespace

void
newLopOperator(OP_OperatorTable* table)
{
    table->addOperator(new OP_Operator(
        "hxr_output",
        "XR Output",
        LOP_XrOutput::myConstructor,
        LOP_XrOutput::myTemplateList,
        (unsigned)1,    // min inputs -- always sits downstream of some stage
        (unsigned)1));  // max inputs
}

static PRM_Name theLiveName("live", "Live");
static PRM_Name theInteractiveName("interactive", "Interactive Placement");
static PRM_Name theRendererName("renderer", "Renderer");
static PRM_Name theMaxResName("maxres", "Max Render Resolution");
static PRM_Name theConvergeName("converge", "Convergence Time");
static PRM_Name theFrozenName("frozen", "Freeze Pose");
static PRM_Name theRefreezeName("refreeze", "Refreeze Pose");
static PRM_Name theDistName("dist", "Anchor Distance");
static PRM_Name theHeightName("height", "Anchor Height");
static PRM_Name theResyncName("resync", "Resync Camera");
static PRM_Name theMoveSpeedName("movespeed", "Move Speed");
static PRM_Name theShowReticleName("showreticle", "Show Reticle");
static PRM_Name theApplyPlacementName("applyplacement", "Apply Camera Placement");
static PRM_Name thePlaceTranslateName("pt", "Placement Translate");
static PRM_Name thePlaceRotateName("pr", "Placement Rotate");

static PRM_Default theRendererDefault(0, "HdStormRendererPlugin");
static PRM_Default theConvergeDefault(1.0);
static PRM_Default theDistDefault(2.0);
static PRM_Default theHeightDefault(1.2);
static PRM_Default theMoveSpeedDefault(1.5);

static PRM_Range theConvergeRange(PRM_RANGE_RESTRICTED, 0.0, PRM_RANGE_UI, 10.0);

// Populated each time the menu opens: every delegate in the Hydra registry,
// filtered and labelled the way Houdini's own viewport menu does it, via the
// same UsdRenderers.json data (HUSD_RendererInfo). That drops the env-gated
// Hydra debugger, anything invalid on this platform, and Houdini's native
// viewport delegate (isNativeRenderer) -- a viewport, not a general renderer.
static void
buildRendererMenu(void* /*data*/, PRM_Name* names, int listSize,
                  const PRM_SpareData* /*spare*/, const PRM_Parm* /*parm*/)
{
    struct Entry
    {
        std::string id;
        std::string label;
        int         priority;
    };
    std::vector<Entry> entries;

    for (HydraRenderer::RendererInfo const& info : HydraRenderer::AvailableRenderers()) {
        const HUSD_RendererInfo houdini = HUSD_RendererInfo::getRendererInfo(
            UT_StringHolder(info.id.GetText()), UT_StringHolder(info.displayName.c_str()));

        if (!houdini.isValid() || !houdini.showInViewportMenu() || houdini.isNativeRenderer()) {
            continue;
        }
        entries.push_back({info.id.GetString(), std::string(houdini.menuLabel().c_str()),
                           houdini.menuPriority()});
    }

    // Higher priority sits higher in the menu, as in Houdini's own.
    std::stable_sort(entries.begin(), entries.end(),
                     [](Entry const& a, Entry const& b) { return a.priority > b.priority; });

    int n = 0;
    for (Entry const& entry : entries) {
        if (n >= listSize - 1) {
            break;   // keep room for the sentinel
        }
        // Deep copies: the PRM_Name(const char*) constructor only references
        // its strings, and these are locals.
        names[n].setTokenAndLabel(entry.id.c_str(), entry.label.c_str());
        ++n;
    }
    names[n].setAsSentinel();
}

static PRM_ChoiceList theRendererMenu(PRM_CHOICELIST_SINGLE, buildRendererMenu);

PRM_Template
LOP_XrOutput::myTemplateList[] = {
    PRM_Template(PRM_TOGGLE, 1, &theLiveName, PRMzeroDefaults),
    // Overrides Renderer -> Storm, Convergence Time -> 0, Freeze Pose -> off,
    // so the view can be placed interactively; turning it off hands over to
    // the configured delegate and settings at that exact pose.
    PRM_Template(PRM_TOGGLE, 1, &theInteractiveName, PRMzeroDefaults),
    PRM_Template(PRM_STRING, 1, &theRendererName, &theRendererDefault, &theRendererMenu),
    // 0 x 0 = uncapped. Set to your licence's render limit (Apprentice is
    // 1280 x 720), or lower still to make a progressive delegate responsive.
    PRM_Template(PRM_INT,    2, &theMaxResName, PRMzeroDefaults),
    // Seconds to hold the head pose so a progressive delegate can accumulate
    // before re-capturing. Has no visible effect on Storm, which converges
    // in one pass and so re-captures every frame regardless.
    PRM_Template(PRM_FLT,    1, &theConvergeName, &theConvergeDefault, 0, &theConvergeRange),
    // Hold the pose indefinitely and render to convergence.
    PRM_Template(PRM_TOGGLE, 1, &theFrozenName, PRMzeroDefaults),
    PRM_Template(PRM_CALLBACK, 1, &theRefreezeName, 0, 0, 0,
                &LOP_XrOutput::onRefreezePose),
    PRM_Template(PRM_FLT,    1, &theDistName,   &theDistDefault),
    PRM_Template(PRM_FLT,    1, &theHeightName, &theHeightDefault),
    PRM_Template(PRM_CALLBACK, 1, &theResyncName, 0, 0, 0,
                &LOP_XrOutput::onResyncCamera),
    // Right-thumbstick locomotion, metres per second at full deflection.
    PRM_Template(PRM_FLT,    1, &theMoveSpeedName, &theMoveSpeedDefault),
    // Gaze reticle; also marks the pivot a right-grip orbit turns about.
    // Worth turning off for a clean look at a converged frame.
    PRM_Template(PRM_TOGGLE, 1, &theShowReticleName, PRMoneDefaults),
    // Camera placement. Clicking the right thumbstick in the headset keys
    // the head's stage-space pose onto pt/pr at the current frame and turns
    // Apply on; cookMyLop then authors the RenderSettings camera from them.
    // Keyframes rather than hidden state, so the placements survive a .hip
    // save, show in the channel editor, and can be edited or deleted there.
    PRM_Template(PRM_TOGGLE, 1, &theApplyPlacementName, PRMzeroDefaults),
    PRM_Template(PRM_XYZ,    3, &thePlaceTranslateName, PRMzeroDefaults),
    PRM_Template(PRM_XYZ,    3, &thePlaceRotateName,    PRMzeroDefaults),
    PRM_Template(),
};

OP_Node*
LOP_XrOutput::myConstructor(OP_Network* net, const char* name, OP_Operator* op)
{
    return new LOP_XrOutput(net, name, op);
}

LOP_XrOutput::LOP_XrOutput(OP_Network* net, const char* name, OP_Operator* op)
    : LOP_Node(net, name, op)
    , myRuntime(std::make_unique<HxrRuntime>())
{
    // The callback fires on the render thread. Everything it needs to do --
    // set keyframes, dirty the node -- must happen on Houdini's main thread,
    // so it's posted to the event loop. The node is looked up by id when the
    // event runs rather than captured by pointer: an event can outlive the
    // node if the user deletes it in between.
    const int nodeId = getUniqueId();
    myRuntime->SetPlacementCallback([nodeId](HxrRuntime::CameraPlacement const& placement) {
        if (!UT_HoudiniExecutionContext::hasInstance()) {
            return;
        }
        UT_HoudiniExecutionContext::instance()->post([nodeId, placement]() {
            if (auto* node = dynamic_cast<LOP_XrOutput*>(OP_Node::lookupNode(nodeId))) {
                node->applyPlacement(placement);
            }
        });
    });
}

LOP_XrOutput::~LOP_XrOutput()
{
    // Blocks until the render thread exits. Necessary here: the DSO can be
    // unloaded shortly after node destruction, and HxrRuntime's thread
    // function lives in this DSO's code.
    myRuntime->Stop();
}

OP_ERROR
LOP_XrOutput::cookMyLop(OP_Context& context)
{
    // Cook our input and soft-copy the result into our own HUSD_DataHandle.
    // We make no further edits, so this alone is a correct passthrough.
    if (cookModifyInput(context) >= UT_ERROR_FATAL) {
        return error();
    }

    const fpreal t = context.getTime();
    const bool live = evalInt(theLiveName, 0, t) != 0;

    // Camera placement: author the RenderSettings camera's transform from
    // the keyed parms. This is the one place this node edits the stage, and
    // it must finish (write lock released) before the read lock below.
    const bool applyPlacement = evalInt(theApplyPlacementName, 0, t) != 0;
    if (applyPlacement) {
        HUSD_AutoWriteLock writelock(editableDataHandle());
        HUSD_AutoLayerLock layerlock(writelock);
        UsdStageRefPtr     stage = writelock.data()->stage();

        SdfPath cameraPath;
        if (!FindRenderCameraPath(stage, &cameraPath)) {
            addWarning(LOP_MESSAGE, "Apply Camera Placement is on, but the stage has no "
                                    "RenderSettings camera to place");
        } else {
            const UsdTimeCode frame(context.getFloatFrame());
            UsdGeomXformable  camera(stage->GetPrimAtPath(cameraPath));

            GfVec3d translate, rotate;
            for (int i = 0; i < 3; ++i) {
                translate[i] = evalFloat(thePlaceTranslateName, i, t);
                rotate[i]    = evalFloat(thePlaceRotateName, i, t);
            }
            GfMatrix4d stageXform = RotationFromEuler(rotate);
            stageXform.SetTranslateOnly(translate);

            // The parms hold the pose in stage space, which is what the user
            // sees; the op is local to whatever the camera is parented under.
            const GfMatrix4d local =
                stageXform * camera.ComputeParentToWorldTransform(frame).GetInverse();

            // A single matrix op, overriding the camera's own op stack in
            // this node's layer; the value is a time sample at this frame.
            camera.MakeMatrixXform().Set(local, frame);
            setLastModifiedPrims(UT_StringRef(cameraPath.GetText()));
        }
    }

    // If an upstream LOP bakes a fully time-sampled stage in one cook (common
    // for cached/baked animation), this node may otherwise never cook again
    // during playback -- nothing about its own inputs would have changed.
    // Declaring time-dependence while Live is on forces a recook every frame
    // change, which is what keeps SetTimeCode() below from going stale.
    flags().setTimeDep(live);

    myRuntime->SetAnchor(float(evalFloat(theDistName, 0, t)),
                         float(evalFloat(theHeightName, 0, t)));

    // The node only ever hands over what's *configured*. Interactive
    // Placement -- from this toggle, or from a controller trigger held in the
    // headset -- is resolved on the render thread, where it can take effect
    // the same frame rather than after a cook.
    UT_String rendererParm;
    evalString(rendererParm, theRendererName, 0, t);
    const std::string rendererId = rendererParm.toStdString();
    myRuntime->SetRendererPlugin(rendererId);
    myRuntime->SetInteractive(evalInt(theInteractiveName, 0, t) != 0);

    myRuntime->SetMaxRenderSize(int(evalInt(theMaxResName, 0, t)),
                                int(evalInt(theMaxResName, 1, t)));

    // The licence cap is tied to the configured delegate, so it's passed
    // separately: while Storm is substituted for placement it doesn't apply.
    if (RunningAsApprentice() && IsKarma(rendererId)) {
        myRuntime->SetRendererMaxSize(kApprenticeKarmaMaxWidth, kApprenticeKarmaMaxHeight);
        addWarning(LOP_MESSAGE, "Apprentice licence: Karma render capped at 1280 x 720 "
                                "per eye and upscaled to the headset");
    } else {
        myRuntime->SetRendererMaxSize(0, 0);
    }

    myRuntime->SetConvergeSeconds(float(evalFloat(theConvergeName, 0, t)));
    myRuntime->SetFrozen(evalInt(theFrozenName, 0, t) != 0);
    myRuntime->SetMoveSpeed(float(evalFloat(theMoveSpeedName, 0, t)));
    myRuntime->SetShowReticle(evalInt(theShowReticleName, 0, t) != 0);

    // HUSD authors USD time samples using the Houdini frame number (not
    // context.getTime(), which is seconds), so this is what keeps the
    // headset in sync with the playbar rather than a fixed pose per second.
    myRuntime->SetTimeCode(context.getFloatFrame());

    if (live) {
        // Houdini bumps this every time the input node's data is dirtied, so
        // it reflects actual content changes. The previous version of this
        // compared UsdStageRefPtr identity instead, which was unsound: HUSD
        // reuses the same stage object across recooks and mutates it in
        // place, so the pointer stays equal while the content changes --
        // which is why a newly added camera was never picked up.
        OP_Node* inputNode = getInput(0);
        const OP_VERSION inputVersion =
            inputNode ? inputNode->dataMicroNode().modVersion() : OP_VERSION(-1);

        HUSD_AutoReadLock readlock(editableDataHandle());
        if (readlock.isStageValid()) {
            UsdStageRefPtr rawStage = readlock.data()->stage();

            // HUSD does not guarantee this stage stays unchanged if this node
            // (or anything else) recooks again -- HUSD_LockedStage's own doc
            // comment says as much. We force a recook every playback frame
            // (flags().setTimeDep above), so without this, the render thread
            // would be reading a stage Houdini's cook engine can concurrently
            // mutate out from under it on the very next frame -- that's what
            // crashed Houdini when the playbar moved.
            //
            // Flatten() severs the connection entirely: the result shares
            // nothing with HUSD's own objects, so it's safe for the render
            // thread to hold and read indefinitely afterward. It must happen
            // HERE, synchronously, while our read lock is still held and
            // Houdini's cook engine is serialized with us -- deferring it to
            // the render thread would only shrink the race window, not close
            // it, since another cook could still land first.
            //
            // The version check is what keeps this affordable: it only
            // flattens when the input's data actually changed, not on every
            // forced per-frame recook. The timing is printed so the real cost
            // is measured rather than assumed.
            //
            // The input's version can't see edits made by this node itself,
            // so a fresh placement forces one, and so does a Resync while a
            // placement is applied -- that's when the snapshot's camera is
            // actually read.
            const bool outputChanged = myOutputDirty || (applyPlacement && myResyncPending);
            if (inputVersion != myLastInputVersion || outputChanged) {
                myOutputDirty = false;
                using Clock = std::chrono::steady_clock;
                const Clock::time_point start = Clock::now();

                SdfLayerRefPtr flatLayer = rawStage->Flatten(/*addSourceFileComment*/ false);
                myRuntime->SetStage(UsdStage::Open(flatLayer));
                myLastInputVersion = inputVersion;

                const double ms = std::chrono::duration<double, std::milli>(
                                      Clock::now() - start).count();
                std::printf("hxr_output: input changed (v%lld) -- reflattened in %.1f ms\n",
                            (long long)inputVersion, ms);
            } else {
                std::printf("hxr_output: recook fired, input unchanged (v%lld) -- reused snapshot\n",
                            (long long)inputVersion);
            }
        }

        // Deferred from the button so it lands after the flatten above --
        // otherwise a resync could latch onto a snapshot predating the
        // camera the user just added.
        if (myResyncPending) {
            myRuntime->RequestResyncCamera();
            myResyncPending = false;
        }

        myRuntime->Start();

        if (myRuntime->HasFailed()) {
            addWarning(LOP_MESSAGE, "XR session failed to start -- "
                                    "see the console for details "
                                    "(no headset connected? wrong OpenXR runtime active?)");
        }
    } else {
        myRuntime->Stop();
    }

    return error();
}

void
LOP_XrOutput::applyPlacement(HxrRuntime::CameraPlacement const& placement)
{
    const GfVec3d position = placement.cameraToStage.ExtractTranslation();
    const GfVec3d degrees  = EulerFromMatrix(placement.cameraToStage);

    // A convention mismatch between decompose and recompose would silently
    // place the camera facing the wrong way; make it loud instead.
    {
        GfMatrix4d original = placement.cameraToStage;
        original.SetTranslateOnly(GfVec3d(0.0));
        original.Orthonormalize(false);
        const GfMatrix4d rebuilt = RotationFromEuler(degrees);
        double maxErr = 0.0;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                maxErr = std::max(maxErr, std::fabs(original[r][c] - rebuilt[r][c]));
            }
        }
        if (maxErr > 1e-4) {
            std::fprintf(stderr, "hxr_output: Euler round-trip error %.4f -- "
                                 "camera placement rotation may be wrong\n", maxErr);
        }
    }

    // Key at the playbar's current time, in seconds -- the frame the runtime
    // saw is the same one unless the playbar moved in the last few ms.
    const fpreal t = CHgetEvalTime();
    for (int i = 0; i < 3; ++i) {
        setFloat(thePlaceTranslateName.getToken(), i, t, position[i], PRM_AK_FORCE_KEY);
        setFloat(thePlaceRotateName.getToken(),    i, t, degrees[i],  PRM_AK_FORCE_KEY);
    }
    setInt(theApplyPlacementName.getToken(), 0, t, 1);

    std::printf("hxr_output: camera placed at frame %.1f -- (%.2f, %.2f, %.2f) rot (%.1f, %.1f, %.1f)\n",
                placement.timeCode, position[0], position[1], position[2],
                degrees[0], degrees[1], degrees[2]);

    myOutputDirty = true;
    forceRecook();
}

bool
LOP_XrOutput::updateParmsFlags()
{
    bool changed = LOP_Node::updateParmsFlags();

    // Grey out what Interactive Placement is overriding, so the UI shows
    // what's actually in effect rather than what's merely configured.
    const bool configurable = evalInt(theInteractiveName, 0, 0.0) == 0;
    changed |= enableParm(theRendererName.getToken(), configurable);
    changed |= enableParm(theConvergeName.getToken(), configurable);
    changed |= enableParm(theFrozenName.getToken(),   configurable);
    changed |= enableParm(theRefreezeName.getToken(), configurable);

    return changed;
}

/*static*/ int
LOP_XrOutput::onRefreezePose(void* data, int /*index*/, fpreal /*t*/,
                             const PRM_Template* /*tplate*/)
{
    // Unlike Resync Camera this needs no cook: it only re-captures the head
    // pose, which the render thread already has.
    static_cast<LOP_XrOutput*>(data)->myRuntime->RequestRefreeze();
    return 1;
}

/*static*/ int
LOP_XrOutput::onResyncCamera(void* data, int /*index*/, fpreal /*t*/,
                             const PRM_Template* /*tplate*/)
{
    LOP_XrOutput* node = static_cast<LOP_XrOutput*>(data);

    // Force a cook rather than resyncing straight from here: the snapshot the
    // render thread holds may predate whatever camera change prompted the
    // click, and cookMyLop is the only place it can safely be refreshed.
    node->myResyncPending = true;
    node->forceRecook();

    return 1;
}
