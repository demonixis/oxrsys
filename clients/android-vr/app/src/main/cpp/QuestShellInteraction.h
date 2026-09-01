// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>

namespace oxr::quest_shell
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Ray
{
    Vec3 origin;
    Vec3 direction;
};

enum class ButtonId
{
    None,
    Reset,
    TogglePassthrough,
};

struct ButtonRect
{
    ButtonId id = ButtonId::None;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool enabled = true;
};

struct PanelLayout
{
    Vec3 center;
    Vec3 right = {1.0f, 0.0f, 0.0f};
    Vec3 up = {0.0f, 1.0f, 0.0f};
    Vec3 normal = {0.0f, 0.0f, 1.0f};
    float width = 0.78f;
    float height = 0.44f;
    std::array<ButtonRect, 2> buttons;
};

struct ButtonHit
{
    ButtonId id = ButtonId::None;
    float distance = 0.0f;
    float localX = 0.0f;
    float localY = 0.0f;
    bool enabled = false;
};

struct ControllerClickState
{
    bool pressed = false;
};

struct ControllerResult
{
    ButtonHit hover;
    ButtonId click = ButtonId::None;
};

PanelLayout MakeDefaultPanelLayout(const Vec3& center,
                                   const Vec3& right,
                                   const Vec3& up,
                                   const Vec3& normal,
                                   bool passthroughSupported);

ButtonHit RaycastButtons(const PanelLayout& panel, const Ray& ray, float maxDistanceMeters = 5.0f);

ControllerResult UpdateController(const PanelLayout& panel,
                                  const Ray& ray,
                                  float triggerValue,
                                  ControllerClickState* state,
                                  float pressThreshold = 0.55f,
                                  float releaseThreshold = 0.35f);

} // namespace oxr::quest_shell
