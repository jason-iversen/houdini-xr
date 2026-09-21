#pragma once

#include <LOP/LOP_Node.h>

#include <pxr/usd/usd/stage.h>

#include <memory>

#include "HxrRuntime.h"

PXR_NAMESPACE_USING_DIRECTIVE

// A sink LOP: passes its input stage through unmodified, and while the
// "Live" toggle is on, hands each cook's stage to a background HxrRuntime
// that presents it to a connected headset. cookMyLop always runs on
// Houdini's main/cook thread, which is exactly the context HUSD_AutoReadLock
// is meant to be used from -- the background render thread never touches
// Houdini's cook engine itself, only the UsdStageRefPtr handed to it.
class LOP_XrOutput : public LOP_Node
{
public:
    static PRM_Template myTemplateList[];
    static OP_Node*      myConstructor(OP_Network* net, const char* name, OP_Operator* op);

    LOP_XrOutput(OP_Network* net, const char* name, OP_Operator* op);
    ~LOP_XrOutput() override;

protected:
    OP_ERROR cookMyLop(OP_Context& context) override;
    bool     updateParmsFlags() override;

private:
    static int onResyncCamera(void* data, int index, fpreal t, const PRM_Template* tplate);
    static int onRefreezePose(void* data, int index, fpreal t, const PRM_Template* tplate);

    // Main thread only. Records a head pose as keyframes on the placement
    // parms at the current playbar time; cookMyLop authors the camera from
    // those parms.
    void applyPlacement(HxrRuntime::CameraPlacement const& placement);

    std::unique_ptr<HxrRuntime> myRuntime;

    // Houdini's own "data got dirtied" counter for our input, used to decide
    // when the flattened snapshot needs rebuilding. See cookMyLop.
    OP_VERSION myLastInputVersion = -1;

    // Set by the Resync Camera button, consumed by the next cook, so the
    // resync is always evaluated against a freshly flattened stage.
    bool myResyncPending = false;

    // Set whenever this node's own output changed for reasons the input's
    // version can't see (a camera placement authored here), so the next
    // cook reflattens regardless.
    bool myOutputDirty = false;
};
