#pragma once

#include "XrPlatform.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/range1d.h>
#include <pxr/base/gf/range2d.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>

#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

// An XrPosef as a pose-local -> reference-space transform.
inline GfMatrix4d XrPoseToMatrix(XrPosef const& pose)
{
    GfMatrix4d m(1.0);
    m.SetRotate(GfQuatd(double(pose.orientation.w),
                        double(pose.orientation.x),
                        double(pose.orientation.y),
                        double(pose.orientation.z)));
    m.SetTranslateOnly(GfVec3d(double(pose.position.x),
                               double(pose.position.y),
                               double(pose.position.z)));
    return m;
}

// xrLocateViews reports where each eye sits in the reference space, so the pose
// is world-from-eye and the view matrix is its inverse.
inline GfMatrix4d XrPoseToViewMatrix(XrPosef const& pose)
{
    return XrPoseToMatrix(pose).GetInverse();
}

// OpenXR gives signed half-angles for an asymmetric frustum. Their tangents are
// the window extents on the unit-distance reference plane GfFrustum works in,
// so letting GfFrustum build the matrix keeps USD's row-vector convention right.
inline GfMatrix4d XrFovToProjectionMatrix(XrFovf const& fov, double nearDist, double farDist)
{
    GfFrustum frustum;
    frustum.SetProjectionType(GfFrustum::Perspective);
    frustum.SetWindow(GfRange2d(
        GfVec2d(std::tan(double(fov.angleLeft)),  std::tan(double(fov.angleDown))),
        GfVec2d(std::tan(double(fov.angleRight)), std::tan(double(fov.angleUp)))));
    frustum.SetNearFar(GfRange1d(nearDist, farDist));
    return frustum.ComputeProjectionMatrix();
}

// How far a controller has rolled about its own pointing axis (local Z) since
// `start`, in radians, -pi..pi, right-handed about +Z. Only the twist
// component of the relative rotation counts (swing-twist decomposition), so
// pointing the controller somewhere else while twisting doesn't register as
// twist. Quaternions are OpenXR's: local -> reference, Hamilton product.
inline double TwistAboutLocalZ(XrQuaternionf const& start, XrQuaternionf const& now)
{
    // relative = conj(start) * now, i.e. `now` expressed in `start`'s frame.
    // Only its w and z components are needed.
    double w = double(start.w) * now.w + double(start.x) * now.x +
               double(start.y) * now.y + double(start.z) * now.z;
    double z = double(start.w) * now.z - double(start.x) * now.y +
               double(start.y) * now.x - double(start.z) * now.w;
    // q and -q are the same rotation; pick the one with w >= 0 so the angle
    // comes out in -pi..pi rather than wrapping the long way round.
    if (w < 0.0) {
        w = -w;
        z = -z;
    }
    return 2.0 * std::atan2(z, w);
}
