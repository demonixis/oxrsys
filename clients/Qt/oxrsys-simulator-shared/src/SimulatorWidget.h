// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QHostAddress>
#include <QWidget>

#include <oxrsys/protocol/Protocol.h>

class QLabel;
class QPushButton;
class QTimer;
class QUdpSocket;

class SimulatorWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit SimulatorWidget(QWidget* parent = nullptr);
    ~SimulatorWidget() override;

    QString stateText() const;
    bool isConnected() const;

public slots:
    void startDiscovery();
    void disconnectFromRuntime();

signals:
    void stateTextChanged(const QString& state);

private slots:
    void readPendingDiscoveryDatagrams();
    void connectToDiscoveredRuntime();
    void sendTrackingSample();

private:
    enum class State
    {
        Disconnected,
        Discovering,
        Discovered,
        Streaming,
    };

    void buildUi();
    void setState(State state, const QString& status);
    void updateControls();
    void updateServerSummary();
    void updateTelemetrySummary();
    QString discoveredServerName() const;

    QLabel* titleLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* serverLabel_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QUdpSocket* discoverySocket_ = nullptr;
    QUdpSocket* controlSocket_ = nullptr;
    QUdpSocket* trackingSocket_ = nullptr;
    QTimer* trackingTimer_ = nullptr;

    QHostAddress serverAddress_;
    oxr::protocol::ServerAnnounce discoveredServer_{};
    State state_ = State::Disconnected;
    quint64 trackingPacketsSent_ = 0;
};
