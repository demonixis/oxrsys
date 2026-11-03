// SPDX-License-Identifier: MPL-2.0

#pragma once

// Geometry for quad and cylinder composition layers. The runtime streams one
// projection image per eye, so every quad or cylinder the application submits
// is drawn into that image before encode, from the same eye pose and field of
// view the projection view was rendered with. That makes the overlay part of
// the picture the headset reprojects: it inherits the stream's latency and the
// headset's rotation warp, exactly like the scene behind it.
//
// Kept free of Objective-C so the math is unit-testable in C++.

#include <openxr/openxr.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace oxrsys::runtime
{

// Near clip distance for overlays, in metres. Anything closer to the eye is
// clipped rather than projected through the eye.
constexpr float kOverlayNearClip = 0.01f;

inline glm::mat4 OverlayPoseMatrix(const XrPosef& pose)
{
    const glm::quat q(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    glm::mat4 m = glm::mat4_cast(glm::normalize(q));
    m[3] = glm::vec4(pose.position.x, pose.position.y, pose.position.z, 1.0f);
    return m;
}

// Clip-space projection for an OpenXR fov (view space: +X right, +Y up,
// looking down -Z) in Metal's convention: x,y in [-w, w], z in [0, w]. Far is
// at infinity; clip z is the constant near distance, so the near plane is the
// only depth clip (z <= w  <=>  near <= -z_view). Depth is never tested.
inline glm::mat4 OverlayProjectionMatrix(const XrFovf& fov, float nearZ = kOverlayNearClip)
{
    const float l = std::tan(fov.angleLeft);
    const float r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp);
    const float d = std::tan(fov.angleDown);

    glm::mat4 p(0.0f); // glm is column-major: p[column][row]
    p[0][0] = 2.0f / (r - l);
    p[2][0] = (r + l) / (r - l);
    p[1][1] = 2.0f / (u - d);
    p[2][1] = (u + d) / (u - d);
    p[3][2] = nearZ;
    p[2][3] = -1.0f;
    return p;
}

// clip <- layer-local metres, for one eye.
//   eyePose        the projection view's pose, in the projection layer's space
//   layerSpaceInProjectionSpace  the overlay's space located in that same space
//   layerPose      the overlay's own pose inside its space
inline glm::mat4 OverlayClipFromLayer(const XrPosef& eyePose,
                                      const XrFovf& eyeFov,
                                      const XrPosef& layerSpaceInProjectionSpace,
                                      const XrPosef& layerPose)
{
    const glm::mat4 projectionFromLayer =
        OverlayPoseMatrix(layerSpaceInProjectionSpace) * OverlayPoseMatrix(layerPose);
    const glm::mat4 eyeFromProjection = glm::inverse(OverlayPoseMatrix(eyePose));
    return OverlayProjectionMatrix(eyeFov) * eyeFromProjection * projectionFromLayer;
}

// Layer-local position of the surface point at (s, t): s runs left to right and
// t top to bottom across the image, both in [0, 1]. Mirrors the vertex shader.
//   quad:     the XY plane, centred on the pose, facing +Z (toward a viewer on +Z)
//   cylinder: axis along local Y through the pose; the image centre sits at
//             -Z, at the radius, seen from the inside; height = r*angle/aspect
inline glm::vec3 OverlayQuadPoint(float width, float height, float s, float t)
{
    return glm::vec3((s - 0.5f) * width, (0.5f - t) * height, 0.0f);
}

inline glm::vec3 OverlayCylinderPoint(float radius, float centralAngle, float height, float s, float t)
{
    const float theta = (s - 0.5f) * centralAngle;
    return glm::vec3(radius * std::sin(theta), (0.5f - t) * height, -radius * std::cos(theta));
}

// Cylinders with radius 0 or +inf are "infinite" in the spec: the layer sits at
// infinity, so only the eye's and the layer's orientations place it.
inline bool IsInfiniteCylinder(float radius)
{
    return radius == 0.0f || std::isinf(radius);
}

inline bool IsDrawableCylinder(float radius, float centralAngle, float aspectRatio)
{
    return radius >= 0.0f &&
           std::isfinite(centralAngle) && centralAngle > 0.0f &&
           std::isfinite(aspectRatio) && aspectRatio > 0.0f;
}

// clip <- layer-local metres for a layer at infinity: OverlayClipFromLayer with
// every position dropped. A unit-radius cylinder drawn with it is centred on the
// eye, so it covers exactly the directions of the infinite one.
inline glm::mat4 OverlayClipFromLayerAtInfinity(XrPosef eyePose,
                                                const XrFovf& eyeFov,
                                                XrPosef layerSpaceInProjectionSpace,
                                                XrPosef layerPose)
{
    eyePose.position = {0.0f, 0.0f, 0.0f};
    layerSpaceInProjectionSpace.position = {0.0f, 0.0f, 0.0f};
    layerPose.position = {0.0f, 0.0f, 0.0f};
    return OverlayClipFromLayer(eyePose, eyeFov, layerSpaceInProjectionSpace, layerPose);
}

inline float CylinderHeight(float radius, float centralAngle, float aspectRatio)
{
    return radius * centralAngle / aspectRatio;
}

} // namespace oxrsys::runtime
