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
