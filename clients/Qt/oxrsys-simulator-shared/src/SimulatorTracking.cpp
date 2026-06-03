// SPDX-License-Identifier: MPL-2.0

#include "SimulatorTracking.h"

#include <QKeyEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace
{

struct Quaternion
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion multiply(const Quaternion& lhs, const Quaternion& rhs)
{
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

Quaternion axisAngle(float x, float y, float z, float angle)
{
    const float halfAngle = angle * 0.5f;
    const float sine = std::sin(halfAngle);
    return {x * sine, y * sine, z * sine, std::cos(halfAngle)};
}

Quaternion headQuaternion(float yaw, float pitch, float roll)
{
    return multiply(multiply(axisAngle(0.0f, 1.0f, 0.0f, yaw),
                            axisAngle(1.0f, 0.0f, 0.0f, pitch)),
                    axisAngle(0.0f, 0.0f, 1.0f, roll));
}

bool containsAny(const QSet<int>& keys, std::initializer_list<int> values)
{
    for (int value : values)
    {
        if (keys.contains(value))
        {
            return true;
        }
    }
    return false;
}

} // namespace

namespace oxrsys::qt_simulator
{

void advanceSimulatorTracking(SimulatorTrackingPose& pose,
                              const QPointF& mouseDelta,
                              const QSet<int>& pressedKeys,
                              float deltaTime)
{
    constexpr float MouseSensitivity = 0.003f;
    constexpr float ArrowSensitivity = 2.0f;
    constexpr float MoveSpeed = 2.0f;

    pose.yaw -= static_cast<float>(mouseDelta.x()) * MouseSensitivity;
    pose.pitch -= static_cast<float>(mouseDelta.y()) * MouseSensitivity;

    if (pressedKeys.contains(Qt::Key_Left))
    {
        pose.yaw += ArrowSensitivity * deltaTime;
    }
    if (pressedKeys.contains(Qt::Key_Right))
    {
        pose.yaw -= ArrowSensitivity * deltaTime;
    }
    if (pressedKeys.contains(Qt::Key_Up))
    {
        pose.pitch += ArrowSensitivity * deltaTime;
    }
    if (pressedKeys.contains(Qt::Key_Down))
    {
        pose.pitch -= ArrowSensitivity * deltaTime;
    }
    pose.pitch = std::clamp(pose.pitch, -1.5f, 1.5f);

    if (pressedKeys.contains(Qt::Key_E))
    {
        pose.roll -= 1.5f * deltaTime;
    }
    if (pressedKeys.contains(Qt::Key_R))
    {
        pose.roll += 1.5f * deltaTime;
    }

    float forwardAmount = 0.0f;
    float strafeAmount = 0.0f;
    if (containsAny(pressedKeys, {Qt::Key_W, Qt::Key_Z}))
    {
        forwardAmount += 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_S))
    {
        forwardAmount -= 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_D))
    {
        strafeAmount += 1.0f;
    }
    if (containsAny(pressedKeys, {Qt::Key_A, Qt::Key_Q}))
    {
        strafeAmount -= 1.0f;
    }
    forwardAmount = std::clamp(forwardAmount, -1.0f, 1.0f);
    strafeAmount = std::clamp(strafeAmount, -1.0f, 1.0f);

    const float forwardX = -std::sin(pose.yaw);
    const float forwardZ = -std::cos(pose.yaw);
    const float rightX = std::cos(pose.yaw);
    const float rightZ = -std::sin(pose.yaw);
    float moveX = forwardX * forwardAmount + rightX * strafeAmount;
    float moveZ = forwardZ * forwardAmount + rightZ * strafeAmount;
    const float moveLength = std::sqrt(moveX * moveX + moveZ * moveZ);
    if (moveLength <= 0.001f)
    {
        return;
    }

    moveX = moveX / moveLength * MoveSpeed * deltaTime;
    moveZ = moveZ / moveLength * MoveSpeed * deltaTime;

    const bool leftShift = pressedKeys.contains(LeftShiftKey);
    const bool rightShift = pressedKeys.contains(RightShiftKey);
    float* target = pose.headPosition;
    if (leftShift && !rightShift)
    {
        target = pose.leftControllerPosition;
    }
    else if (rightShift && !leftShift)
    {
        target = pose.rightControllerPosition;
    }
    target[0] += moveX;
    target[2] += moveZ;
}

void fillSimulatorTrackingPacket(const SimulatorTrackingPose& pose,
                                 const QSet<int>& pressedKeys,
                                 int64_t timestampNs,
                                 oxr::protocol::TrackingPacket& packet)
{
    packet = {};
    packet.timestampNs = timestampNs;
    packet.trackingFlags =
        oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE |
        oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE;
    std::copy(std::begin(pose.headPosition),
              std::end(pose.headPosition),
              std::begin(packet.headPosition));

    const Quaternion orientation = headQuaternion(pose.yaw, pose.pitch, pose.roll);
    packet.headOrientation[0] = orientation.x;
    packet.headOrientation[1] = orientation.y;
    packet.headOrientation[2] = orientation.z;
    packet.headOrientation[3] = orientation.w;

    std::copy(std::begin(pose.leftControllerPosition),
              std::end(pose.leftControllerPosition),
              std::begin(packet.leftControllerPos));
    std::copy(std::begin(pose.rightControllerPosition),
              std::end(pose.rightControllerPosition),
              std::begin(packet.rightControllerPos));
    packet.leftControllerRot[0] = orientation.x;
    packet.leftControllerRot[1] = orientation.y;
    packet.leftControllerRot[2] = orientation.z;
    packet.leftControllerRot[3] = orientation.w;
    packet.rightControllerRot[0] = orientation.x;
    packet.rightControllerRot[1] = orientation.y;
    packet.rightControllerRot[2] = orientation.z;
    packet.rightControllerRot[3] = orientation.w;

    if (pressedKeys.contains(Qt::Key_F))
    {
        packet.buttonState |= oxr::protocol::BUTTON_LEFT_GRIP;
        packet.leftGrip = 1.0f;
    }
    if (pressedKeys.contains(Qt::Key_G))
    {
        packet.buttonState |= oxr::protocol::BUTTON_RIGHT_GRIP;
        packet.rightGrip = 1.0f;
    }
    packet.ipd = 0.064f;
}

int simulatorKeyIdentifier(const QKeyEvent& event)
{
    if (event.key() != Qt::Key_Shift)
    {
        return event.key();
    }

#if defined(Q_OS_MACOS)
    if (event.nativeVirtualKey() == 60)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#elif defined(Q_OS_WIN)
    if (event.nativeVirtualKey() == 0xA1)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#else
    if (event.nativeScanCode() == 54)
    {
        return RightShiftKey;
    }
    return LeftShiftKey;
#endif
}

} // namespace oxrsys::qt_simulator
