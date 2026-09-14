#include "LOP_XrOutput.h"
#include "HxrRuntime.h"

#include <HUSD/HUSD_DataHandle.h>
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

#include <chrono>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

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
static PRM_Name theDistName("dist", "Anchor Distance");
static PRM_Name theHeightName("height", "Anchor Height");
static PRM_Name theResyncName("resync", "Resync Camera");

static PRM_Default theDistDefault(2.0);
static PRM_Default theHeightDefault(1.2);

PRM_Template
LOP_XrOutput::myTemplateList[] = {
    PRM_Template(PRM_TOGGLE, 1, &theLiveName, PRMzeroDefaults),
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
