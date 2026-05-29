// SPDX-License-Identifier: MPL-2.0

#include "SimulatorWidget.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

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

} // namespace

SimulatorWidget::SimulatorWidget(QWidget* parent)
    : QWidget(parent)
{
    discoverySocket_ = new QUdpSocket(this);
    controlSocket_ = new QUdpSocket(this);
    trackingSocket_ = new QUdpSocket(this);
    trackingTimer_ = new QTimer(this);
    trackingTimer_->setInterval(1000 / 90);

    buildUi();

    connect(searchButton_, &QPushButton::clicked, this, &SimulatorWidget::startDiscovery);
    connect(connectButton_, &QPushButton::clicked, this, &SimulatorWidget::connectToDiscoveredRuntime);
    connect(disconnectButton_, &QPushButton::clicked, this, &SimulatorWidget::disconnectFromRuntime);
    connect(discoverySocket_, &QUdpSocket::readyRead, this, &SimulatorWidget::readPendingDiscoveryDatagrams);
    connect(trackingTimer_, &QTimer::timeout, this, &SimulatorWidget::sendTrackingSample);

    setState(State::Disconnected, "Disconnected");
}

SimulatorWidget::~SimulatorWidget()
{
    disconnectFromRuntime();
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
    trackingTimer_->stop();
    discoverySocket_->close();
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

    controlSocket_->writeDatagram(
        reinterpret_cast<const char*>(&connectPacket),
        static_cast<qint64>(sizeof(connectPacket)),
        serverAddress_,
        oxr::protocol::CONTROL_PORT);

    trackingPacketsSent_ = 0;
    trackingTimer_->start();
    setState(State::Streaming, "Connected; sending synthetic head tracking");
    updateTelemetrySummary();
}

void SimulatorWidget::sendTrackingSample()
{
    if (state_ != State::Streaming)
    {
        return;
    }

    oxr::protocol::TrackingPacket packet = {};
    packet.timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000ll;
    packet.trackingFlags = 0;
    packet.headPosition[0] = 0.0f;
    packet.headPosition[1] = 1.65f;
    packet.headPosition[2] = 0.0f;
    packet.headOrientation[3] = 1.0f;
    packet.ipd = 0.064f;

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
    setMinimumSize(640, 360);
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
    emit stateTextChanged(stateText());
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

void SimulatorWidget::updateTelemetrySummary()
{
    if (state_ != State::Streaming)
    {
        telemetryLabel_->setText("Idle. Connect to start 90 Hz synthetic head tracking.");
        return;
    }

    telemetryLabel_->setText(QString("%1 packets sent\nHead pose: 0, 1.65, 0\nIPD: 0.064 m")
                                 .arg(trackingPacketsSent_));
}

QString SimulatorWidget::discoveredServerName() const
{
    const QString name = safeServerName(discoveredServer_);
    return name.isEmpty() ? "OXRSys Runtime" : name;
}
