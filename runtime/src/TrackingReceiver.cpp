// SPDX-License-Identifier: MPL-2.0

#include "TrackingReceiver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

glm::quat LoadQuat(const float* src)
{
    return glm::normalize(glm::quat(src[3], src[0], src[1], src[2]));
}

void StoreQuat(float* dst, const glm::quat& quat)
{
    glm::quat normalized = glm::normalize(quat);
    dst[0] = normalized.x;
    dst[1] = normalized.y;
    dst[2] = normalized.z;
    dst[3] = normalized.w;
}

glm::quat CanonicalizeQuat(const glm::quat& quat, const glm::quat* reference)
{
    glm::quat normalized = glm::normalize(quat);
    if (reference != nullptr && glm::dot(normalized, *reference) < 0.0f)
    {
        normalized = -normalized;
    }
    return normalized;
}

glm::vec3 LoadVec3(const float* src)
{
    return glm::vec3(src[0], src[1], src[2]);
}

void StoreVec3(float* dst, const glm::vec3& vec)
{
    dst[0] = vec.x;
    dst[1] = vec.y;
    dst[2] = vec.z;
}

// Predict position using client-reported velocity if available, otherwise finite difference.
glm::vec3 PredictPosition(const glm::vec3& previous, const glm::vec3& current,
                          float dtSeconds, float horizonSeconds, float maxSpeed,
                          const glm::vec3& reportedVelocity = glm::vec3(0.0f))
{
    if (horizonSeconds <= 0.0f)
    {
        return current;
    }

    glm::vec3 velocity;
    float reportedSpeed = glm::length(reportedVelocity);
    if (reportedSpeed > 0.001f)
    {
        // Use client-reported IMU velocity (more accurate than finite difference)
        velocity = reportedVelocity;
    }
    else if (dtSeconds > 0.0001f)
    {
        // Fallback to finite difference
        velocity = (current - previous) / dtSeconds;
    }
    else
    {
        return current;
    }

    float speed = glm::length(velocity);
    if (speed > maxSpeed && speed > 0.0f)
    {
        velocity *= maxSpeed / speed;
    }

    return current + velocity * horizonSeconds;
}

// Predict orientation using client-reported angular velocity if available.
glm::quat PredictOrientationFromVelocity(const glm::quat& current,
                                          const glm::vec3& angularVelocity,
                                          float horizonSeconds, float maxRadians)
{
    if (horizonSeconds <= 0.0f)
    {
        return current;
    }

    float angSpeed = glm::length(angularVelocity);
    if (angSpeed < 0.001f)
    {
        return current;
    }

    glm::vec3 axis = angularVelocity / angSpeed;
    float angle = angSpeed * horizonSeconds;
    angle = std::clamp(angle, -maxRadians, maxRadians);

    glm::quat rotation = glm::angleAxis(angle, axis);
    return glm::normalize(rotation * current);
}

glm::quat PredictOrientation(const glm::quat& previous, const glm::quat& current,
                             float dtSeconds, float horizonSeconds, float maxRadians)
{
    if (dtSeconds <= 0.0001f || horizonSeconds <= 0.0f)
    {
        return current;
    }

    glm::quat prev = glm::normalize(previous);
    glm::quat cur = glm::normalize(current);
    if (glm::dot(prev, cur) < 0.0f)
    {
        prev = -prev;
    }

    glm::quat delta = glm::normalize(cur * glm::inverse(prev));
    float cosHalfAngle = std::clamp(delta.w, -1.0f, 1.0f);
    float halfAngle = std::acos(cosHalfAngle);
    float sinHalfAngle = std::sqrt(std::max(0.0f, 1.0f - cosHalfAngle * cosHalfAngle));
    if (sinHalfAngle < 0.0001f || halfAngle < 0.0001f)
    {
        return cur;
    }

    glm::vec3 axis(delta.x, delta.y, delta.z);
    axis /= sinHalfAngle;

    float angle = halfAngle * 2.0f;
    float extrapolatedAngle = angle * (horizonSeconds / dtSeconds);
    extrapolatedAngle = std::clamp(extrapolatedAngle, -maxRadians, maxRadians);

    glm::quat extra = glm::angleAxis(extrapolatedAngle, glm::normalize(axis));
    return glm::normalize(extra * cur);
}

} // namespace

TrackingReceiver::~TrackingReceiver()
{
    Stop();
}

bool TrackingReceiver::Start()
{
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0)
    {
        spdlog::error("TrackingReceiver: Failed to create socket");
        return false;
    }

    int opt = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(oxr::protocol::TRACKING_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(socket_, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        spdlog::error("TrackingReceiver: Failed to bind on port {}",
                       oxr::protocol::TRACKING_PORT);
        close(socket_);
        socket_ = -1;
        return false;
    }

    running_.store(true);
    receiveThread_ = std::thread(&TrackingReceiver::ReceiveThread, this);

    spdlog::info("TrackingReceiver: Listening on port {}", oxr::protocol::TRACKING_PORT);
    return true;
}

void TrackingReceiver::Stop()
{
    running_.store(false);

    if (socket_ >= 0)
    {
        close(socket_);
        socket_ = -1;
    }

    if (receiveThread_.joinable())
    {
        receiveThread_.join();
    }

    spdlog::info("TrackingReceiver: Stopped ({} packets received)", packetCount_.load());
}

void TrackingReceiver::ReceiveThread()
{
    uint8_t buffer[sizeof(oxr::protocol::TrackingPacket)];

    while (running_.load())
    {
        timeval tv = {0, 5000}; // 5ms timeout for responsive shutdown
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t received = recv(socket_, buffer, sizeof(buffer), 0);
        if (received < (ssize_t)sizeof(oxr::protocol::TrackingPacket))
        {
            continue;
        }

        oxr::protocol::TrackingPacket packet = {};
        memcpy(&packet, buffer, sizeof(packet));
        StorePacket(packet, SteadyClockNowNs());
    }
}

void TrackingReceiver::InjectPacket(const uint8_t* data, size_t size)
{
    if (size < sizeof(oxr::protocol::TrackingPacket))
    {
        return;
    }

    oxr::protocol::TrackingPacket packet = {};
    memcpy(&packet, data, sizeof(packet));
    StorePacket(packet, SteadyClockNowNs());
}

bool TrackingReceiver::GetLatestPose(oxr::protocol::TrackingPacket& outPacket) const
{
    if (!hasData_.load())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(poseMutex_);
    outPacket = latestPacket_;
    return true;
}

bool TrackingReceiver::GetPredictedPose(oxr::protocol::TrackingPacket& outPacket) const
{
    if (!hasData_.load())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(poseMutex_);
    if (history_.empty())
    {
        return false;
    }

    outPacket = history_.back().packet;
    if (history_.size() < 2)
    {
        return true;
    }

    float horizonMs = std::clamp(predictionHorizonMs_.load(), 0.0f, 80.0f);
    if (horizonMs <= 0.01f)
    {
        return true;
    }

    const HistorySample& previous = history_[history_.size() - 2];
    const HistorySample& current = history_.back();

    int64_t packetDeltaNs = current.packet.timestampNs - previous.packet.timestampNs;
    if (packetDeltaNs <= 0)
    {
        packetDeltaNs = current.receiveTimeNs - previous.receiveTimeNs;
    }

    float dtSeconds = static_cast<float>(packetDeltaNs) / 1.0e9f;
    glm::vec3 headLinVel = LoadVec3(current.packet.headLinearVelocity);
    glm::vec3 headAngVel = LoadVec3(current.packet.headAngularVelocity);
    float headAngularSpeed = glm::length(headAngVel);
    bool hasHeadAngularVelocity = headAngularSpeed > 0.001f;

    float totalHorizonSeconds = horizonMs / 1000.0f;
    float headRotationHorizonSeconds = hasHeadAngularVelocity
        ? totalHorizonSeconds
        : totalHorizonSeconds * 0.5f;
    float controllerRotationHorizonSeconds = totalHorizonSeconds;
    float positionHorizonSeconds = totalHorizonSeconds * 0.5f;

    int64_t nowNs = SteadyClockNowNs();
    int64_t lastLogNs = lastPredictionDiagnosticNs_.load();
    if (nowNs - lastLogNs >= 5LL * 1000LL * 1000LL * 1000LL &&
        lastPredictionDiagnosticNs_.compare_exchange_strong(lastLogNs, nowNs))
    {
        spdlog::info("TrackingReceiver: prediction horizon={:.1f}ms head_ang_vel={} speed={:.2f}rad/s",
                     horizonMs, hasHeadAngularVelocity ? "yes" : "no", headAngularSpeed);
    }

    StoreVec3(outPacket.headPosition,
              PredictPosition(LoadVec3(previous.packet.headPosition),
                              LoadVec3(current.packet.headPosition),
                              dtSeconds, positionHorizonSeconds, 3.0f, headLinVel));

    if (hasHeadAngularVelocity)
    {
        StoreQuat(outPacket.headOrientation,
                  PredictOrientationFromVelocity(LoadQuat(current.packet.headOrientation),
                                                  headAngVel, headRotationHorizonSeconds,
                                                  glm::radians(35.0f)));
    }
    else
    {
        StoreQuat(outPacket.headOrientation,
                  PredictOrientation(LoadQuat(previous.packet.headOrientation),
                                     LoadQuat(current.packet.headOrientation),
                                     dtSeconds, headRotationHorizonSeconds,
                                     glm::radians(35.0f)));
    }

    const bool previousLeftControllerActive =
        (previous.packet.trackingFlags &
         oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0;
    const bool currentLeftControllerActive =
        (current.packet.trackingFlags &
         oxr::protocol::TRACKING_FLAG_LEFT_CONTROLLER_ACTIVE) != 0;
    if (previousLeftControllerActive && currentLeftControllerActive)
    {
        StoreVec3(outPacket.leftControllerPos,
                  PredictPosition(LoadVec3(previous.packet.leftControllerPos),
                                  LoadVec3(current.packet.leftControllerPos),
                                  dtSeconds, positionHorizonSeconds, 6.0f));
        StoreQuat(outPacket.leftControllerRot,
                  PredictOrientation(LoadQuat(previous.packet.leftControllerRot),
                                     LoadQuat(current.packet.leftControllerRot),
                                     dtSeconds, controllerRotationHorizonSeconds,
                                     glm::radians(45.0f)));
    }

    const bool previousRightControllerActive =
        (previous.packet.trackingFlags &
         oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0;
    const bool currentRightControllerActive =
        (current.packet.trackingFlags &
         oxr::protocol::TRACKING_FLAG_RIGHT_CONTROLLER_ACTIVE) != 0;
    if (previousRightControllerActive && currentRightControllerActive)
    {
        StoreVec3(outPacket.rightControllerPos,
                  PredictPosition(LoadVec3(previous.packet.rightControllerPos),
                                  LoadVec3(current.packet.rightControllerPos),
                                  dtSeconds, positionHorizonSeconds, 6.0f));
        StoreQuat(outPacket.rightControllerRot,
                  PredictOrientation(LoadQuat(previous.packet.rightControllerRot),
                                     LoadQuat(current.packet.rightControllerRot),
                                     dtSeconds, controllerRotationHorizonSeconds,
                                     glm::radians(45.0f)));
    }

    return true;
}

void TrackingReceiver::SetPredictionHorizonMs(float predictionHorizonMs)
{
    predictionHorizonMs_.store(std::max(predictionHorizonMs, 0.0f));
}

void TrackingReceiver::StorePacket(const oxr::protocol::TrackingPacket& packet, int64_t receiveTimeNs)
{
    oxr::protocol::TrackingPacket normalizedPacket = packet;

    {
        std::lock_guard<std::mutex> lock(poseMutex_);
        const glm::quat* headReference = nullptr;
        const glm::quat* leftControllerReference = nullptr;
        const glm::quat* rightControllerReference = nullptr;

        glm::quat latestHeadQuat;
        glm::quat latestLeftControllerQuat;
        glm::quat latestRightControllerQuat;
        if (hasData_.load())
        {
            latestHeadQuat = LoadQuat(latestPacket_.headOrientation);
            latestLeftControllerQuat = LoadQuat(latestPacket_.leftControllerRot);
            latestRightControllerQuat = LoadQuat(latestPacket_.rightControllerRot);
            headReference = &latestHeadQuat;
            leftControllerReference = &latestLeftControllerQuat;
            rightControllerReference = &latestRightControllerQuat;
        }

        StoreQuat(normalizedPacket.headOrientation,
                  CanonicalizeQuat(LoadQuat(packet.headOrientation), headReference));
        StoreQuat(normalizedPacket.leftControllerRot,
                  CanonicalizeQuat(LoadQuat(packet.leftControllerRot), leftControllerReference));
        StoreQuat(normalizedPacket.rightControllerRot,
                  CanonicalizeQuat(LoadQuat(packet.rightControllerRot), rightControllerReference));

        latestPacket_ = normalizedPacket;
        history_.push_back({normalizedPacket, receiveTimeNs});
        while (history_.size() > MaxHistorySamples)
        {
            history_.pop_front();
        }
    }

    hasData_.store(true);
    packetCount_.fetch_add(1);
}
