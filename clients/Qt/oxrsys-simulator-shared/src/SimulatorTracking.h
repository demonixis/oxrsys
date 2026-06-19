// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QPointF>
#include <QSet>

#include <cstdint>

#include <oxrsys/protocol/Protocol.h>

class QKeyEvent;

namespace oxrsys::qt_simulator
{

constexpr int LeftShiftKey = -1001;
constexpr int RightShiftKey = -1002;

struct SimulatorTrackingPose
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float headPosition[3] = {0.0f, 1.6f, 0.0f};
    float leftControllerPosition[3] = {-0.2f, 1.3f, -0.4f};
    float rightControllerPosition[3] = {0.2f, 1.3f, -0.4f};
};

void advanceSimulatorTracking(SimulatorTrackingPose& pose,
                              const QPointF& mouseDelta,
                              const QSet<int>& pressedKeys,
                              float deltaTime);

void fillSimulatorTrackingPacket(const SimulatorTrackingPose& pose,
                                 const QSet<int>& pressedKeys,
                                 int64_t timestampNs,
                                 float verticalFovDegrees,
                                 float eyeAspect,
                                 oxr::protocol::TrackingPacket& packet);

int simulatorKeyIdentifier(const QKeyEvent& event);

} // namespace oxrsys::qt_simulator
