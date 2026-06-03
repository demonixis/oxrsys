// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QAbstractButton>
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <QWheelEvent>

#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>

namespace
{

QString safeServerName(const oxr::protocol::ServerAnnounce& announce)
{
    return QString::fromUtf8(
        announce.serverName,
        static_cast<int>(strnlen(announce.serverName, sizeof(announce.serverName))));
}

QString platformSimulatorDeviceName()
{
#if defined(Q_OS_MACOS)
    return "OXRSys Qt Simulator macOS";
#elif defined(Q_OS_WIN)
    return "OXRSys Qt Simulator Windows";
#elif defined(Q_OS_LINUX)
    return "OXRSys Qt Simulator Linux";
#else
    return "OXRSys Qt Simulator";
#endif
}

QString ffmpegErrorString(int error)
{
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
#else
    Q_UNUSED(error);
    return "FFmpeg support is not enabled";
#endif
}

QLabel* makeSecondaryLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: #9aa0a6;");
    return label;
}

QFrame* makePanel(QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setFrameShape(QFrame::StyledPanel);
    panel->setStyleSheet("QFrame { background: #171a20; border: 1px solid #30343c; border-radius: 8px; }");
    return panel;
}

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

float radiansToDegrees(float radians)
{
    return radians * 57.2957795131f;
}

} // namespace

class SimulatorPreviewWidget final : public QWidget
{
public:
    explicit SimulatorPreviewWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(220);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
    }

    void setPose(float yaw,
                 float pitch,
                 float roll,
                 const float position[3],
                 bool mouseCaptured,
                 bool streaming)
    {
        yaw_ = yaw;
        pitch_ = pitch;
        roll_ = roll;
        position_[0] = position[0];
        position_[1] = position[1];
        position_[2] = position[2];
        mouseCaptured_ = mouseCaptured;
        streaming_ = streaming;
        update();
    }

    void setVideoFrame(const QImage& frame)
    {
        videoFrame_ = frame;
        update();
    }

    void setStatusOverlay(const QString& videoStatus,
                          quint64 videoPackets,
                          quint64 videoFrames,
                          quint64 videoDrops,
                          quint64 fecRecoveries,
                          quint64 decodeErrors)
    {
        videoStatus_ = videoStatus;
        videoPackets_ = videoPackets;
        videoFrames_ = videoFrames;
        videoDrops_ = videoDrops;
        fecRecoveries_ = fecRecoveries;
        decodeErrors_ = decodeErrors;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF bounds = rect();
        if (!videoFrame_.isNull())
        {
            painter.fillRect(bounds, QColor(4, 6, 9));
            const QSizeF scaledSize = videoFrame_.size().scaled(bounds.size().toSize(), Qt::KeepAspectRatio);
            const QRectF target(bounds.center().x() - scaledSize.width() * 0.5,
                                bounds.center().y() - scaledSize.height() * 0.5,
                                scaledSize.width(),
                                scaledSize.height());
            painter.drawImage(target, videoFrame_);
            painter.fillRect(bounds, QColor(0, 0, 0, 45));
        }
        else
        {
            const QColor sky(18, 24, 31);
            const QColor floor(13, 17, 22);
            painter.fillRect(bounds, sky);

            const float horizonY = static_cast<float>(bounds.height()) * 0.48f + pitch_ * 72.0f;
            const float yawOffset = std::sin(yaw_) * 95.0f;
            const QPointF vanishing(bounds.center().x() + yawOffset, horizonY);

            QPainterPath floorPath;
            floorPath.moveTo(0.0, horizonY);
            floorPath.lineTo(bounds.width(), horizonY);
            floorPath.lineTo(bounds.width(), bounds.height());
            floorPath.lineTo(0.0, bounds.height());
            floorPath.closeSubpath();
            painter.fillPath(floorPath, floor);

            QPen gridPen(QColor(65, 78, 94, 120), 1.0);
            painter.setPen(gridPen);
            for (int i = -8; i <= 8; ++i)
            {
                const float x = static_cast<float>(bounds.center().x()) +
                    static_cast<float>(i) * 42.0f + yawOffset * 0.25f;
                painter.drawLine(QPointF(x, bounds.height()), vanishing);
            }
            for (int i = 0; i < 8; ++i)
            {
                const float t = static_cast<float>(i + 1) / 8.0f;
                const float y = horizonY + (static_cast<float>(bounds.height()) - horizonY) * t * t;
                painter.drawLine(QPointF(0, y), QPointF(bounds.width(), y));
            }

            painter.setPen(QPen(QColor(126, 231, 135, streaming_ ? 230 : 150), 2.0));
            painter.drawLine(QPointF(0, horizonY), QPointF(bounds.width(), horizonY));

            painter.setPen(QColor(242, 244, 248, 220));
            QFont statusFont = painter.font();
            statusFont.setPointSize(statusFont.pointSize() + 1);
            statusFont.setBold(true);
            painter.setFont(statusFont);
            painter.drawText(bounds.adjusted(24, 24, -24, -24),
                             Qt::AlignCenter,
                             videoStatus_.isEmpty() ? "Waiting for video" : videoStatus_);
        }

        const float centerX = static_cast<float>(bounds.center().x());
        const float centerY = static_cast<float>(bounds.center().y());
        const QPointF reticle(centerX + std::sin(yaw_) * 42.0f,
                              centerY - std::sin(pitch_) * 85.0f);
        painter.setPen(QPen(QColor(242, 244, 248, 210), 1.5));
        painter.drawLine(reticle + QPointF(-16, 0), reticle + QPointF(-4, 0));
        painter.drawLine(reticle + QPointF(4, 0), reticle + QPointF(16, 0));
        painter.drawLine(reticle + QPointF(0, -16), reticle + QPointF(0, -4));
        painter.drawLine(reticle + QPointF(0, 4), reticle + QPointF(0, 16));
        painter.drawEllipse(reticle, 5.0, 5.0);

        const QRectF badge(bounds.left() + 14.0, bounds.bottom() - 58.0, 260.0, 42.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(8, 11, 16, 205));
        painter.drawRoundedRect(badge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(badge.adjusted(12, 6, -12, -22),
                         QString("Yaw %1  Pitch %2")
                             .arg(radiansToDegrees(yaw_), 0, 'f', 1)
                             .arg(radiansToDegrees(pitch_), 0, 'f', 1));
        painter.setPen(QColor(154, 160, 166));
        painter.drawText(badge.adjusted(12, 22, -12, -6),
                         QString("Position %1, %2, %3")
                             .arg(position_[0], 0, 'f', 2)
                             .arg(position_[1], 0, 'f', 2)
                             .arg(position_[2], 0, 'f', 2));

        const QRectF captureBadge(bounds.right() - 158.0, bounds.top() + 14.0, 144.0, 32.0);
        painter.setBrush(mouseCaptured_ ? QColor(42, 145, 72, 220) : QColor(52, 59, 68, 220));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(captureBadge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(captureBadge, Qt::AlignCenter,
                         mouseCaptured_ ? "Mouse captured" : "Mouse free");

        const QRectF videoBadge(bounds.right() - 292.0, bounds.bottom() - 58.0, 278.0, 42.0);
        painter.setBrush(QColor(8, 11, 16, 205));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(videoBadge, 8.0, 8.0);
        painter.setPen(QColor(242, 244, 248));
        painter.drawText(videoBadge.adjusted(12, 6, -12, -22),
                         videoStatus_.isEmpty() ? "Waiting for video" : videoStatus_);
        painter.setPen(QColor(154, 160, 166));
        painter.drawText(videoBadge.adjusted(12, 22, -12, -6),
                         QString("%1 packets  %2 frames  %3 drops  %4 fec  %5 errors")
                             .arg(videoPackets_)
                             .arg(videoFrames_)
                             .arg(videoDrops_)
                             .arg(fecRecoveries_)
                             .arg(decodeErrors_));
    }

private:
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float roll_ = 0.0f;
    float position_[3] = {0.0f, 1.6f, 0.0f};
    QImage videoFrame_;
    QString videoStatus_ = "Waiting for video";
    quint64 videoPackets_ = 0;
    quint64 videoFrames_ = 0;
    quint64 videoDrops_ = 0;
    quint64 fecRecoveries_ = 0;
    quint64 decodeErrors_ = 0;
    bool mouseCaptured_ = false;
    bool streaming_ = false;
};

SimulatorWidget::SimulatorWidget(QWidget* parent)
    : QWidget(parent)
{
    discoverySocket_ = new QUdpSocket(this);
    videoSocket_ = new QUdpSocket(this);
    controlSocket_ = new QUdpSocket(this);
    trackingSocket_ = new QUdpSocket(this);
    trackingTimer_ = new QTimer(this);
    trackingTimer_->setInterval(1000 / 90);
    trackingTimer_->setTimerType(Qt::PreciseTimer);

    buildUi();

    connect(searchButton_, &QPushButton::clicked, this, &SimulatorWidget::startDiscovery);
    connect(connectButton_, &QPushButton::clicked, this, &SimulatorWidget::connectToDiscoveredRuntime);
    connect(disconnectButton_, &QPushButton::clicked, this, &SimulatorWidget::disconnectFromRuntime);
    connect(discoverySocket_, &QUdpSocket::readyRead, this, &SimulatorWidget::readPendingDiscoveryDatagrams);
    connect(videoSocket_, &QUdpSocket::readyRead, this, &SimulatorWidget::readPendingVideoDatagrams);
    connect(trackingTimer_, &QTimer::timeout, this, &SimulatorWidget::sendTrackingSample);

    poseClock_.start();
    trackingTimer_->start();
    setState(State::Disconnected, "Disconnected");
}

SimulatorWidget::~SimulatorWidget()
{
    trackingTimer_->stop();
    disconnectFromRuntime();
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    resetVideoDecoder();
#endif
}

QString SimulatorWidget::stateText() const
{
    switch (state_)
    {
        case State::Disconnected:
            return "Disconnected";
        case State::Discovering:
            return "Searching";
        case State::Discovered:
            return "Runtime discovered";
        case State::Streaming:
            return "Connected";
    }
    return "Unknown";
}

bool SimulatorWidget::isConnected() const
{
    return state_ == State::Streaming;
}

void SimulatorWidget::startDiscovery()
{
    disconnectFromRuntime();
    discoveredServer_ = {};
    serverAddress_ = {};
    trackingPacketsSent_ = 0;

    discoverySocket_->close();
    const bool bound = discoverySocket_->bind(QHostAddress::AnyIPv4,
                                              oxr::protocol::DISCOVERY_PORT,
                                              QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
    {
        setState(State::Disconnected,
                 QString("Failed to bind discovery UDP port %1: %2")
                     .arg(oxr::protocol::DISCOVERY_PORT)
                     .arg(discoverySocket_->errorString()));
        return;
    }

    setState(State::Discovering, "Searching for OXRSys runtime...");
    updateServerSummary();
}

void SimulatorWidget::disconnectFromRuntime()
{
    discoverySocket_->close();
    stopVideoReceiver();
    controlSocket_->close();
    trackingSocket_->close();
    if (state_ != State::Disconnected)
    {
        setState(State::Disconnected, "Disconnected");
    }
    updateTelemetrySummary();
}

void SimulatorWidget::readPendingDiscoveryDatagrams()
{
    while (discoverySocket_->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(discoverySocket_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        discoverySocket_->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        Q_UNUSED(senderPort);

        if (datagram.size() < static_cast<int>(sizeof(oxr::protocol::ServerAnnounce)))
        {
            continue;
        }

        oxr::protocol::ServerAnnounce announce = {};
        std::memcpy(&announce, datagram.constData(), sizeof(announce));
        if (announce.type != oxr::protocol::MessageType::ServerAnnounce)
        {
            continue;
        }

        discoveredServer_ = announce;
        serverAddress_ = sender;
        setState(State::Discovered, "Runtime discovered");
        updateServerSummary();
    }
}

void SimulatorWidget::readPendingVideoDatagrams()
{
    while (videoSocket_->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(videoSocket_->pendingDatagramSize()));
        videoSocket_->readDatagram(datagram.data(), datagram.size());

        if (datagram.size() < static_cast<int>(sizeof(oxr::protocol::VideoPacketHeader)))
        {
            continue;
        }

        oxr::protocol::VideoPacketHeader header = {};
        std::memcpy(&header, datagram.constData(), sizeof(header));
        const char* payload = datagram.constData() + sizeof(header);
        const qsizetype availablePayloadSize = datagram.size() - static_cast<qsizetype>(sizeof(header));
        const qsizetype payloadSize =
            std::min<qsizetype>(static_cast<qsizetype>(header.payloadSize), availablePayloadSize);

        ++videoPacketsReceived_;
        handleVideoPacket(header, payload, payloadSize, monotonicNowNs());
    }
}

void SimulatorWidget::connectToDiscoveredRuntime()
{
    if (state_ != State::Discovered)
    {
        return;
    }

    oxr::protocol::ClientConnect connectPacket = {};
    connectPacket.type = oxr::protocol::MessageType::ClientConnect;
    connectPacket.versionMajor = 1;
    connectPacket.versionMinor = 0;
    connectPacket.preferredCodec = static_cast<uint32_t>(oxr::protocol::VideoCodec::H265);
    connectPacket.maxBitrateMbps = 50;
    connectPacket.refreshRateHz = std::max<uint32_t>(discoveredServer_.refreshRateHz, 60);
    const QByteArray deviceName = platformSimulatorDeviceName().toUtf8();
    std::strncpy(connectPacket.deviceName, deviceName.constData(), sizeof(connectPacket.deviceName) - 1);

    if (!startVideoReceiver())
    {
        setState(State::Discovered, "Video receiver unavailable");
        return;
    }

    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&connectPacket),
        static_cast<qint64>(sizeof(connectPacket)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);

    trackingPacketsSent_ = 0;
    updatePreviewStatus();
    setState(State::Streaming, "Connected; sending synthetic head tracking");
    updateTelemetrySummary();
}

void SimulatorWidget::sendTrackingSample()
{
    const qint64 elapsedMilliseconds = poseClock_.restart();
    const float deltaTime = std::clamp(static_cast<float>(elapsedMilliseconds) / 1000.0f,
                                       0.001f,
                                       0.050f);
    advanceSimulation(deltaTime);
    processAssembledVideoFrames(videoAssembler_.expirePendingFrame(
        monotonicNowNs(),
        500'000'000));

    if (previewWidget_ != nullptr)
    {
        previewWidget_->setPose(trackingPose_.yaw,
                                trackingPose_.pitch,
                                trackingPose_.roll,
                                trackingPose_.headPosition,
                                mouseCaptured_,
                                state_ == State::Streaming);
    }

    if (state_ != State::Streaming)
    {
        return;
    }

    oxr::protocol::TrackingPacket packet = {};
    fillTrackingPacket(packet);

    trackingSocket_->writeDatagram(
        reinterpret_cast<const char*>(&packet),
        static_cast<qint64>(sizeof(packet)),
        serverAddress_,
        oxr::protocol::TRACKING_PORT);

    ++trackingPacketsSent_;
    if ((trackingPacketsSent_ % 30) == 0)
    {
        updateTelemetrySummary();
    }
}

void SimulatorWidget::buildUi()
{
    setMinimumSize(720, 520);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet("SimulatorWidget { background: #0b0d10; color: #f2f4f8; }"
                  "QPushButton { padding: 6px 12px; }"
                  "QLabel { color: #f2f4f8; }");

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(14);

    auto* headerLayout = new QHBoxLayout();
    auto* titleLayout = new QVBoxLayout();
    titleLabel_ = new QLabel("OXRSys Simulator", this);
    titleLabel_->setStyleSheet("font-size: 22px; font-weight: 700;");
    hintLabel_ = makeSecondaryLabel("Synthetic desktop client: discover a runtime, connect, and stream head tracking.", this);
    titleLayout->addWidget(titleLabel_);
    titleLayout->addWidget(hintLabel_);
    headerLayout->addLayout(titleLayout, 1);

    statusLabel_ = new QLabel(this);
    statusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusLabel_->setStyleSheet("font-weight: 600; color: #7ee787;");
    headerLayout->addWidget(statusLabel_);
    rootLayout->addLayout(headerLayout);

    previewWidget_ = new SimulatorPreviewWidget(this);
    previewWidget_->installEventFilter(this);
    rootLayout->addWidget(previewWidget_, 2);

    auto* detailsLayout = new QHBoxLayout();
    auto* serverPanel = makePanel(this);
    auto* serverLayout = new QVBoxLayout(serverPanel);
    serverLayout->addWidget(makeSecondaryLabel("Runtime", serverPanel));
    serverLabel_ = new QLabel(serverPanel);
    serverLabel_->setWordWrap(true);
    serverLayout->addWidget(serverLabel_);
    detailsLayout->addWidget(serverPanel, 2);

    auto* telemetryPanel = makePanel(this);
    auto* telemetryLayout = new QVBoxLayout(telemetryPanel);
    telemetryLayout->addWidget(makeSecondaryLabel("Tracking", telemetryPanel));
    telemetryLabel_ = new QLabel(telemetryPanel);
    telemetryLabel_->setWordWrap(true);
    telemetryLayout->addWidget(telemetryLabel_);
    detailsLayout->addWidget(telemetryPanel, 1);
    rootLayout->addLayout(detailsLayout, 1);

    auto* buttonLayout = new QHBoxLayout();
    searchButton_ = new QPushButton("Search", this);
    connectButton_ = new QPushButton("Connect", this);
    disconnectButton_ = new QPushButton("Disconnect", this);
    buttonLayout->addWidget(searchButton_);
    buttonLayout->addWidget(connectButton_);
    buttonLayout->addWidget(disconnectButton_);
    buttonLayout->addStretch();
    rootLayout->addLayout(buttonLayout);
}

void SimulatorWidget::setState(State state, const QString& status)
{
    state_ = state;
    statusLabel_->setText(status);
    updateControls();
    updateTelemetrySummary();
    updatePreviewStatus();
    emit stateTextChanged(stateText());
}

bool SimulatorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != previewWidget_)
    {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type())
    {
        case QEvent::MouseButtonPress:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            previewWidget_->setFocus(Qt::MouseFocusReason);
            setFocus(Qt::MouseFocusReason);
            lastMousePosition_ = mouseEvent->position();
            hasLastMousePosition_ = true;
            if (mouseEvent->button() == Qt::RightButton)
            {
                toggleMouseCaptured();
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            lastMousePosition_ = mouseEvent->position();
            hasLastMousePosition_ = true;
            break;
        }
        case QEvent::MouseMove:
        {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (!hasLastMousePosition_)
            {
                lastMousePosition_ = mouseEvent->position();
                hasLastMousePosition_ = true;
                return true;
            }

            const QPointF delta = mouseEvent->position() - lastMousePosition_;
            lastMousePosition_ = mouseEvent->position();
            if (mouseCaptured_ || (mouseEvent->buttons() & Qt::LeftButton))
            {
                accumulateMouseDelta(delta);
            }
            event->accept();
            return true;
        }
        case QEvent::Wheel:
        {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            const float wheelSteps = static_cast<float>(wheelEvent->angleDelta().y()) / 120.0f;
            const float forwardX = -std::sin(trackingPose_.yaw);
            const float forwardZ = -std::cos(trackingPose_.yaw);
            trackingPose_.headPosition[0] += forwardX * wheelSteps * 0.25f;
            trackingPose_.headPosition[2] += forwardZ * wheelSteps * 0.25f;
            event->accept();
            return true;
        }
        case QEvent::KeyPress:
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::KeyRelease:
            keyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::FocusOut:
            setMouseCaptured(false);
            resetInputState();
            break;
        default:
            break;
    }

    return QWidget::eventFilter(watched, event);
}

void SimulatorWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        setMouseCaptured(false);
        event->accept();
        return;
    }
    if (!event->isAutoRepeat())
    {
        setKeyPressed(oxrsys::qt_simulator::simulatorKeyIdentifier(*event), true);
    }
    event->accept();
}

void SimulatorWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (!event->isAutoRepeat())
    {
        setKeyPressed(oxrsys::qt_simulator::simulatorKeyIdentifier(*event), false);
    }
    event->accept();
}

void SimulatorWidget::focusOutEvent(QFocusEvent* event)
{
    setMouseCaptured(false);
    resetInputState();
    QWidget::focusOutEvent(event);
}

void SimulatorWidget::updateControls()
{
    searchButton_->setEnabled(state_ == State::Disconnected);
    connectButton_->setEnabled(state_ == State::Discovered);
    disconnectButton_->setEnabled(state_ == State::Discovering ||
                                  state_ == State::Discovered ||
                                  state_ == State::Streaming);
}

void SimulatorWidget::updateServerSummary()
{
    if (state_ == State::Discovering)
    {
        serverLabel_->setText("Waiting for UDP discovery broadcast.");
        return;
    }
    if (state_ == State::Disconnected || serverAddress_.isNull())
    {
        serverLabel_->setText("No runtime discovered.");
        return;
    }

    serverLabel_->setText(QString("%1\n%2\n%3 x %4 @ %5 Hz")
                              .arg(discoveredServerName())
                              .arg(serverAddress_.toString())
                              .arg(discoveredServer_.encodedWidth)
                              .arg(discoveredServer_.encodedHeight)
                              .arg(discoveredServer_.refreshRateHz));
}

void SimulatorWidget::updatePreviewStatus()
{
    if (previewWidget_ == nullptr)
    {
        return;
    }

    QString status;
#if !OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    status = "Video preview unavailable: FFmpeg support was not enabled";
#else
    if (state_ == State::Streaming && videoFramesDecoded_ == 0)
    {
        status = "Waiting for video";
    }
    else if (videoFramesDecoded_ > 0)
    {
        status = "Video";
    }
    else
    {
        status = "Waiting for video";
    }
#endif
    previewWidget_->setStatusOverlay(status,
                                     videoPacketsReceived_,
                                     videoFramesDecoded_,
                                     videoFramesDropped_,
                                     videoFecRecoveries_,
                                     decodeErrors_);
}

void SimulatorWidget::advanceSimulation(float deltaTime)
{
    const QPointF mouseDelta = pendingMouseDelta_;
    pendingMouseDelta_ = {};
    oxrsys::qt_simulator::advanceSimulatorTracking(
        trackingPose_,
        mouseDelta,
        pressedKeys_,
        deltaTime);
}

void SimulatorWidget::fillTrackingPacket(oxr::protocol::TrackingPacket& packet) const
{
    oxrsys::qt_simulator::fillSimulatorTrackingPacket(
        trackingPose_,
        pressedKeys_,
        monotonicNowNs(),
        packet);
}

bool SimulatorWidget::startVideoReceiver()
{
#if !OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    updatePreviewStatus();
    return true;
#else
    stopVideoReceiver();
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    consecutiveDecodeErrors_ = 0;
    lastKeyframeRequestTimeNs_ = 0;
    updatePreviewStatus();

    videoSocket_->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8 * 1024 * 1024);
    const bool bound = videoSocket_->bind(QHostAddress::AnyIPv4,
                                          oxr::protocol::VIDEO_PORT,
                                          QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bound)
    {
        setState(State::Discovered,
                 QString("Failed to bind video UDP port %1: %2")
                     .arg(oxr::protocol::VIDEO_PORT)
                     .arg(videoSocket_->errorString()));
        return false;
    }
    return ensureVideoDecoder();
#endif
}

void SimulatorWidget::stopVideoReceiver()
{
    if (videoSocket_ != nullptr)
    {
        videoSocket_->close();
    }
    if (previewWidget_ != nullptr)
    {
        previewWidget_->setVideoFrame(QImage());
    }
    videoAssembler_.reset();
    videoPacketsReceived_ = 0;
    videoFramesDecoded_ = 0;
    videoFramesDropped_ = 0;
    videoFecRecoveries_ = 0;
    decodeErrors_ = 0;
    consecutiveDecodeErrors_ = 0;
    lastKeyframeRequestTimeNs_ = 0;
    updatePreviewStatus();
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    resetVideoDecoder();
#endif
}

void SimulatorWidget::handleVideoPacket(const oxr::protocol::VideoPacketHeader& header,
                                        const char* payload,
                                        qsizetype payloadSize,
                                        int64_t receiveTimeNs)
{
    processAssembledVideoFrames(
        videoAssembler_.addPacket(header, payload, payloadSize, receiveTimeNs));
}

void SimulatorWidget::processAssembledVideoFrames(const QList<AssembledVideoFrame>& frames)
{
    const quint64 previousDrops = videoFramesDropped_;
    videoFramesDropped_ = videoAssembler_.droppedFrames();
    videoFecRecoveries_ = videoAssembler_.fecRecoveries();
    if (videoFramesDropped_ > previousDrops)
    {
        sendKeyframeRequest(oxr::protocol::KEYFRAME_REASON_FRAME_LOSS,
                            static_cast<uint32_t>(videoFramesDropped_ - previousDrops));
    }
    if (frames.isEmpty())
    {
        updatePreviewStatus();
        return;
    }

    for (const AssembledVideoFrame& frame : frames)
    {
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
        const int64_t decodeStartNs = monotonicNowNs();
        if (decodeVideoFrame(frame))
        {
            consecutiveDecodeErrors_ = 0;
            sendLatencyReport(frame, decodeStartNs, monotonicNowNs());
        }
        else
        {
            ++decodeErrors_;
            ++consecutiveDecodeErrors_;
            if (consecutiveDecodeErrors_ >= 3)
            {
                sendKeyframeRequest(oxr::protocol::KEYFRAME_REASON_DECODE_STALL,
                                    static_cast<uint32_t>(consecutiveDecodeErrors_));
                consecutiveDecodeErrors_ = 0;
            }
        }
#else
        Q_UNUSED(frame);
#endif
    }
    updatePreviewStatus();
}

void SimulatorWidget::sendKeyframeRequest(uint32_t reasonFlags, uint32_t detail)
{
    if (state_ != State::Streaming)
    {
        return;
    }

    const uint64_t nowNs = static_cast<uint64_t>(monotonicNowNs());
    constexpr uint64_t CooldownNs = 1'000'000'000;
    if (lastKeyframeRequestTimeNs_ != 0 &&
        nowNs - lastKeyframeRequestTimeNs_ < CooldownNs)
    {
        return;
    }
    lastKeyframeRequestTimeNs_ = nowNs;

    oxr::protocol::RequestKeyframe request = {};
    request.reasonFlags = reasonFlags;
    request.detail = detail;
    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&request),
        static_cast<qint64>(sizeof(request)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);
}

void SimulatorWidget::sendLatencyReport(const AssembledVideoFrame& frame,
                                        int64_t decodeStartNs,
                                        int64_t decodeEndNs)
{
    if (state_ != State::Streaming)
    {
        return;
    }

    oxr::protocol::LatencyReport report = {};
    report.receiveToDecoderSubmitMs =
        static_cast<float>(std::max<int64_t>(0, decodeStartNs - frame.receiveTimeNs)) /
        1'000'000.0f;
    report.decodeLatencyMs =
        static_cast<float>(std::max<int64_t>(0, decodeEndNs - decodeStartNs)) /
        1'000'000.0f;
    report.compositorLatencyMs = 0.0f;
    report.totalClientLatencyMs = report.receiveToDecoderSubmitMs + report.decodeLatencyMs;

    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&report),
        static_cast<qint64>(sizeof(report)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);
}

int64_t SimulatorWidget::monotonicNowNs() const
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
bool SimulatorWidget::ensureVideoDecoder()
{
    if (videoDecoder_ != nullptr)
    {
        return true;
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (codec == nullptr)
    {
        setState(State::Discovered, "H.265 decoder not found");
        return false;
    }

    videoDecoder_ = avcodec_alloc_context3(codec);
    decodedFrame_ = av_frame_alloc();
    decodePacket_ = av_packet_alloc();
    if (videoDecoder_ == nullptr || decodedFrame_ == nullptr || decodePacket_ == nullptr)
    {
        resetVideoDecoder();
        setState(State::Discovered, "Failed to allocate video decoder");
        return false;
    }

    const int result = avcodec_open2(videoDecoder_, codec, nullptr);
    if (result < 0)
    {
        const QString error = ffmpegErrorString(result);
        resetVideoDecoder();
        setState(State::Discovered, "Failed to open H.265 decoder: " + error);
        return false;
    }
    return true;
}

void SimulatorWidget::resetVideoDecoder()
{
    if (swsContext_ != nullptr)
    {
        sws_freeContext(swsContext_);
        swsContext_ = nullptr;
    }
    if (decodePacket_ != nullptr)
    {
        av_packet_free(&decodePacket_);
    }
    if (decodedFrame_ != nullptr)
    {
        av_frame_free(&decodedFrame_);
    }
    if (videoDecoder_ != nullptr)
    {
        avcodec_free_context(&videoDecoder_);
    }
}

bool SimulatorWidget::decodeVideoFrame(const AssembledVideoFrame& frame)
{
    if (!ensureVideoDecoder() || frame.nalUnit.isEmpty())
    {
        return false;
    }

    av_packet_unref(decodePacket_);
    const int packetResult = av_new_packet(decodePacket_, static_cast<int>(frame.nalUnit.size()));
    if (packetResult < 0)
    {
        return false;
    }
    std::memcpy(decodePacket_->data,
                frame.nalUnit.constData(),
                static_cast<size_t>(frame.nalUnit.size()));
    decodePacket_->pts = frame.presentationTimeNs;

    const int sendResult = avcodec_send_packet(videoDecoder_, decodePacket_);
    av_packet_unref(decodePacket_);
    if (sendResult < 0)
    {
        return false;
    }

    bool decodedAnyFrame = false;
    while (true)
    {
        const int receiveResult = avcodec_receive_frame(videoDecoder_, decodedFrame_);
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF)
        {
            break;
        }
        if (receiveResult < 0)
        {
            return decodedAnyFrame;
        }

        QImage image(decodedFrame_->width, decodedFrame_->height, QImage::Format_RGB888);
        uint8_t* destinationData[4] = {image.bits(), nullptr, nullptr, nullptr};
        int destinationLinesize[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
        swsContext_ = sws_getCachedContext(swsContext_,
                                            decodedFrame_->width,
                                            decodedFrame_->height,
                                            static_cast<AVPixelFormat>(decodedFrame_->format),
                                            decodedFrame_->width,
                                            decodedFrame_->height,
                                            AV_PIX_FMT_RGB24,
                                            SWS_BILINEAR,
                                            nullptr,
                                            nullptr,
                                            nullptr);
        if (swsContext_ == nullptr)
        {
            av_frame_unref(decodedFrame_);
            return decodedAnyFrame;
        }

        sws_scale(swsContext_,
                  decodedFrame_->data,
                  decodedFrame_->linesize,
                  0,
                  decodedFrame_->height,
                  destinationData,
                  destinationLinesize);

        if (previewWidget_ != nullptr)
        {
            previewWidget_->setVideoFrame(image.copy());
        }
        ++videoFramesDecoded_;
        decodedAnyFrame = true;
        av_frame_unref(decodedFrame_);
    }
    return decodedAnyFrame;
}
#endif

void SimulatorWidget::setMouseCaptured(bool captured)
{
    if (mouseCaptured_ == captured)
    {
        return;
    }

    mouseCaptured_ = captured;
    hasLastMousePosition_ = false;
    pendingMouseDelta_ = {};
    if (previewWidget_ != nullptr)
    {
        if (mouseCaptured_)
        {
            previewWidget_->grabMouse();
            previewWidget_->setCursor(Qt::BlankCursor);
            previewWidget_->setFocus(Qt::MouseFocusReason);
        }
        else
        {
            previewWidget_->releaseMouse();
            previewWidget_->setCursor(Qt::CrossCursor);
        }
    }
}

void SimulatorWidget::toggleMouseCaptured()
{
    setMouseCaptured(!mouseCaptured_);
}

void SimulatorWidget::accumulateMouseDelta(const QPointF& delta)
{
    pendingMouseDelta_ += delta;
}

void SimulatorWidget::setKeyPressed(int key, bool pressed)
{
    if (pressed)
    {
        pressedKeys_.insert(key);
    }
    else
    {
        pressedKeys_.remove(key);
    }
}

void SimulatorWidget::resetInputState()
{
    pressedKeys_.clear();
    pendingMouseDelta_ = {};
    hasLastMousePosition_ = false;
}

void SimulatorWidget::updateTelemetrySummary()
{
    if (state_ != State::Streaming)
    {
        telemetryLabel_->setText("Idle. Connect to start 90 Hz synthetic head tracking.");
        return;
    }

    telemetryLabel_->setText(QString("%1 tracking packets sent\n%2 video packets, %3 frames, %4 drops, %5 fec, %6 decode errors\nHead pose: %7, %8, %9\nYaw/pitch: %10 / %11 deg\nIPD: 0.064 m")
                                 .arg(trackingPacketsSent_)
                                 .arg(videoPacketsReceived_)
                                 .arg(videoFramesDecoded_)
                                 .arg(videoFramesDropped_)
                                 .arg(videoFecRecoveries_)
                                 .arg(decodeErrors_)
                                 .arg(trackingPose_.headPosition[0], 0, 'f', 2)
                                 .arg(trackingPose_.headPosition[1], 0, 'f', 2)
                                 .arg(trackingPose_.headPosition[2], 0, 'f', 2)
                                 .arg(radiansToDegrees(trackingPose_.yaw), 0, 'f', 1)
                                 .arg(radiansToDegrees(trackingPose_.pitch), 0, 'f', 1));
}

QString SimulatorWidget::discoveredServerName() const
{
    const QString name = safeServerName(discoveredServer_);
    return name.isEmpty() ? "OXRSys Runtime" : name;
}
