#include "LOP_XrOutput.h"
#include "HxrRuntime.h"
#include "HydraRenderer.h"

#include <HUSD/HUSD_DataHandle.h>
#include <HUSD/HUSD_RendererInfo.h>
#include <OP/OP_DataMicroNode.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_Include.h>
#include <UT/UT_DSOVersion.h>

// Silences C4003 (macro too few args), which fires from USD's own macros
// under MSVC -- same guard SideFX uses in their own LOP samples.
#include <pxr/base/arch/pragmas.h>
ARCH_PRAGMA_PUSH
ARCH_PRAGMA_MACRO_TOO_FEW_ARGUMENTS
#include <HUSD/XUSD_Data.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
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

static PRM_Default theRendererDefault(0, "HdStormRendererPlugin");
static PRM_Default theConvergeDefault(1.0);
static PRM_Default theDistDefault(2.0);
static PRM_Default theHeightDefault(1.2);

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

    // If an upstream LOP bakes a fully time-sampled stage in one cook (common
    // for cached/baked animation), this node may otherwise never cook again
    // during playback -- nothing about its own inputs would have changed.
    // Declaring time-dependence while Live is on forces a recook every frame
    // change, which is what keeps SetTimeCode() below from going stale.
    flags().setTimeDep(live);

    myRuntime->SetAnchor(float(evalFloat(theDistName, 0, t)),
                         float(evalFloat(theHeightName, 0, t)));

    // Interactive Placement substitutes a live Storm view for whatever is
    // configured, so the viewpoint can be placed at full speed; turning it
    // off hands the exact same pose to the slow delegate. The order below
    // matters for that handover: SetFrozen(true) captures the pose on its
    // off->on transition, so it must see the delegate switch in the same cook.
    const bool interactive = evalInt(theInteractiveName, 0, t) != 0;

    UT_String rendererParm;
    evalString(rendererParm, theRendererName, 0, t);
    const std::string rendererId =
        interactive ? HydraRenderer::DefaultRendererId().GetString() : rendererParm.toStdString();
    myRuntime->SetRendererPlugin(rendererId);

    int maxWidth  = int(evalInt(theMaxResName, 0, t));
    int maxHeight = int(evalInt(theMaxResName, 1, t));
    if (RunningAsApprentice() && IsKarma(rendererId)) {
        maxWidth  = TightestCap(maxWidth,  kApprenticeKarmaMaxWidth);
        maxHeight = TightestCap(maxHeight, kApprenticeKarmaMaxHeight);
        addWarning(LOP_MESSAGE, "Apprentice licence: Karma render capped at 1280 x 720 "
                                "per eye and upscaled to the headset");
    }
    myRuntime->SetMaxRenderSize(maxWidth, maxHeight);

    myRuntime->SetConvergeSeconds(interactive ? 0.0f : float(evalFloat(theConvergeName, 0, t)));
    myRuntime->SetFrozen(!interactive && evalInt(theFrozenName, 0, t) != 0);

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
            if (inputVersion != myLastInputVersion) {
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
