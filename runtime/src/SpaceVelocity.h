// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <openxr/openxr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

namespace oxrsys::space_velocity
{

inline bool IsFinite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

inline glm::vec3 Vector(const XrVector3f& value)
{
    return {value.x, value.y, value.z};
}

inline XrSpaceVelocity AtOffset(const XrSpaceVelocity& velocity, const glm::vec3& worldOffset)
{
    XrSpaceVelocity result = {XR_TYPE_SPACE_VELOCITY};
    const bool angularValid =
        (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0 &&
        IsFinite(Vector(velocity.angularVelocity));
    if (angularValid)
    {
        result.velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        result.angularVelocity = velocity.angularVelocity;
    }

    const bool hasOffset = worldOffset != glm::vec3(0.0f);
    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0 &&
        IsFinite(worldOffset) && (!hasOffset || angularValid))
    {
        glm::vec3 linear = Vector(velocity.linearVelocity);
        if (hasOffset)
        {
            linear += glm::cross(Vector(velocity.angularVelocity), worldOffset);
        }
        if (IsFinite(linear))
        {
            result.velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
            result.linearVelocity = {linear.x, linear.y, linear.z};
        }
    }
    return result;
}

inline XrSpaceVelocity Relative(const XrSpaceVelocity& velocity, const XrSpaceVelocity& baseVelocity,
                                const glm::quat& baseOrientation, const glm::vec3& worldOffset)
{
    XrSpaceVelocity result = {XR_TYPE_SPACE_VELOCITY};
    const float magnitudeSquared = glm::dot(baseOrientation, baseOrientation);
    if (!std::isfinite(magnitudeSquared) || magnitudeSquared <= 0.0f)
    {
        return result;
    }
    const glm::quat baseRotInv = glm::inverse(glm::normalize(baseOrientation));
    const bool baseAngularValid =
        (baseVelocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0 &&
        IsFinite(Vector(baseVelocity.angularVelocity));
    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0 && baseAngularValid)
    {
        const glm::vec3 angular = baseRotInv *
            (Vector(velocity.angularVelocity) - Vector(baseVelocity.angularVelocity));
        if (IsFinite(angular))
        {
            result.velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            result.angularVelocity = {angular.x, angular.y, angular.z};
        }
    }

    const bool hasOffset = worldOffset != glm::vec3(0.0f);
    if ((velocity.velocityFlags & baseVelocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0 &&
        IsFinite(worldOffset) && (!hasOffset || baseAngularValid))
    {
        glm::vec3 linear = Vector(velocity.linearVelocity) - Vector(baseVelocity.linearVelocity);
        if (hasOffset)
        {
            // Differentiating coordinates in a rotating base adds this transport term.
            linear -= glm::cross(Vector(baseVelocity.angularVelocity), worldOffset);
        }
        linear = baseRotInv * linear;
        if (IsFinite(linear))
        {
            result.velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
            result.linearVelocity = {linear.x, linear.y, linear.z};
        }
    }
    return result;
}

} // namespace oxrsys::space_velocity
