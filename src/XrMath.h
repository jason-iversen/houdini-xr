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

// The grip orbit's rotation: how the scene turns, given the controller's
// orientation when the grip began and now (OpenXR quaternions). Returned as a
// row-vector rotation matrix in the reference space; the caller applies it
// about the pivot.
//
// The hand's rotation is split (swing-twist) into yaw about world +Y and the
// swing that remains. Yaw is multiplied by `yawGain`, so a comfortable wrist
// turn can carry the scene all the way round; the swing stays 1:1. With
// `yUp`, the swing is reduced to its pitch about `rightAxis` (the user's
// horizontal right) -- dropping roll, so the horizon never tilts. At gain 1
// without yUp this is exactly the hand's own rotation.
inline GfMatrix4d OrbitRotation(XrQuaternionf const& start, XrQuaternionf const& now,
                                double yawGain, bool yUp, GfVec3d const& rightAxis)
{
    struct Q
    {
        double w, x, y, z;
    };
    // Hamilton product: a * b applies b, then a.
    auto mul = [](Q a, Q b) {
        return Q{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                 a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    };
    auto conj = [](Q q) { return Q{q.w, -q.x, -q.y, -q.z}; };
    auto axisAngle = [](GfVec3d const& axis, double radians) {
        const double s = std::sin(0.5 * radians);
        return Q{std::cos(0.5 * radians), axis[0] * s, axis[1] * s, axis[2] * s};
    };
    // Angle of the rotation about `axis` contained in q (its twist), -pi..pi.
    auto twistAngle = [](Q q, GfVec3d const& axis) {
        double w = q.w;
        double d = q.x * axis[0] + q.y * axis[1] + q.z * axis[2];
        if (w < 0.0) {   // q and -q are the same rotation; take the short way
            w = -w;
            d = -d;
        }
        return (std::abs(w) + std::abs(d) < 1e-12) ? 0.0 : 2.0 * std::atan2(d, w);
    };

    const Q      s{start.w, start.x, start.y, start.z};
    const Q      n{now.w, now.x, now.y, now.z};
    const Q      delta = mul(n, conj(s));   // world-frame rotation, start -> now
    const GfVec3d up(0.0, 1.0, 0.0);
    const double yaw   = twistAngle(delta, up);
    Q            swing = mul(delta, conj(axisAngle(up, yaw)));
    if (yUp) {
        swing = axisAngle(rightAxis, twistAngle(swing, rightAxis));
    }
    const Q result = mul(swing, axisAngle(up, yawGain * yaw));

    GfMatrix4d m(1.0);
    m.SetRotate(GfQuatd(result.w, result.x, result.y, result.z));
    return m;
}
