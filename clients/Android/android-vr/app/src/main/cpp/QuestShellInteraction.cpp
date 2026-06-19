// SPDX-License-Identifier: MPL-2.0

#include "QuestShellInteraction.h"

#include <algorithm>
#include <cmath>

namespace oxr::quest_shell
{

namespace
{

Vec3 Sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

ButtonHit FindButtonAtLocal(const PanelLayout& panel,
                            float localX,
                            float localY,
                            float distance)
{
    for (const ButtonRect& button : panel.buttons)
    {
        if (button.id == ButtonId::None)
        {
            continue;
        }
        const float halfWidth = button.width * 0.5f;
        const float halfHeight = button.height * 0.5f;
        if (localX >= button.centerX - halfWidth &&
            localX <= button.centerX + halfWidth &&
            localY >= button.centerY - halfHeight &&
            localY <= button.centerY + halfHeight)
        {
            return {button.id, distance, localX, localY, button.enabled};
        }
    }
    return {};
}

} // namespace

PanelLayout MakeDefaultPanelLayout(const Vec3& center,
                                   const Vec3& right,
                                   const Vec3& up,
                                   const Vec3& normal,
                                   bool passthroughSupported)
{
    PanelLayout panel = {};
    panel.center = center;
    panel.right = right;
    panel.up = up;
    panel.normal = normal;
    panel.width = 0.78f;
    panel.height = 0.44f;
    panel.buttons = {{
        {ButtonId::Reset, -0.19f, -0.155f, 0.30f, 0.095f, true},
        {ButtonId::TogglePassthrough, 0.19f, -0.155f, 0.30f, 0.095f, passthroughSupported},
    }};
    return panel;
}

ButtonHit RaycastButtons(const PanelLayout& panel, const Ray& ray, float maxDistanceMeters)
{
    const float denom = Dot(ray.direction, panel.normal);
    if (std::abs(denom) < 0.0001f)
    {
        return {};
    }

    const float distance = -Dot(Sub(ray.origin, panel.center), panel.normal) / denom;
    if (!std::isfinite(distance) || distance <= 0.0f || distance > maxDistanceMeters)
    {
        return {};
    }

    Vec3 hitPoint = {
        ray.origin.x + ray.direction.x * distance,
        ray.origin.y + ray.direction.y * distance,
        ray.origin.z + ray.direction.z * distance,
    };
    const Vec3 relative = Sub(hitPoint, panel.center);
    const float localX = Dot(relative, panel.right);
    const float localY = Dot(relative, panel.up);
    return FindButtonAtLocal(panel, localX, localY, distance);
}

ControllerResult UpdateController(const PanelLayout& panel,
                                  const Ray& ray,
                                  float triggerValue,
                                  ControllerClickState* state,
                                  float pressThreshold,
                                  float releaseThreshold)
{
    ControllerResult result = {};
    result.hover = RaycastButtons(panel, ray);

    if (state == nullptr)
    {
        return result;
    }

    const bool pressedNow = triggerValue >= pressThreshold;
    const bool releasedNow = triggerValue <= releaseThreshold;
    if (!state->pressed && pressedNow)
    {
        if (result.hover.id != ButtonId::None && result.hover.enabled)
        {
            result.click = result.hover.id;
        }
        state->pressed = true;
    }
    else if (state->pressed && releasedNow)
    {
        state->pressed = false;
    }

    return result;
}

} // namespace oxr::quest_shell
