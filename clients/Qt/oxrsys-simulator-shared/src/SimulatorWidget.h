// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "SimulatorTracking.h"
#include "VideoFrameAssembler.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointF>
#include <QSet>
#include <QWidget>
#include <QByteArray>
#include <QVector>

#include <oxrsys/protocol/Protocol.h>

#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
#endif

class QLabel;
class QPushButton;
class QTimer;
class QUdpSocket;
class SimulatorPreviewWidget;

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
    void readPendingVideoDatagrams();
    void connectToDiscoveredRuntime();
    void sendTrackingSample();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

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
    void updatePreviewStatus();
    void advanceSimulation(float deltaTime);
    void fillTrackingPacket(oxr::protocol::TrackingPacket& packet) const;
    bool startVideoReceiver();
    void stopVideoReceiver();
    void handleVideoPacket(const oxr::protocol::VideoPacketHeader& header,
                           const char* payload,
                           qsizetype payloadSize,
                           int64_t receiveTimeNs);
    void processAssembledVideoFrames(const QList<AssembledVideoFrame>& frames);
    void sendKeyframeRequest(uint32_t reasonFlags, uint32_t detail);
    void sendLatencyReport(const AssembledVideoFrame& frame,
                           int64_t decodeStartNs,
                           int64_t decodeEndNs);
    int64_t monotonicNowNs() const;
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    bool ensureVideoDecoder();
    void resetVideoDecoder();
    bool decodeVideoFrame(const AssembledVideoFrame& frame);
#endif
    void setMouseCaptured(bool captured);
    void toggleMouseCaptured();
    void accumulateMouseDelta(const QPointF& delta);
    void setKeyPressed(int key, bool pressed);
    void resetInputState();
    QString discoveredServerName() const;

    QLabel* titleLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* serverLabel_ = nullptr;
    QLabel* telemetryLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    SimulatorPreviewWidget* previewWidget_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QUdpSocket* discoverySocket_ = nullptr;
    QUdpSocket* videoSocket_ = nullptr;
    QUdpSocket* controlSocket_ = nullptr;
    QUdpSocket* trackingSocket_ = nullptr;
    QTimer* trackingTimer_ = nullptr;

    QHostAddress serverAddress_;
    oxr::protocol::ServerAnnounce discoveredServer_{};
    State state_ = State::Disconnected;
    quint64 trackingPacketsSent_ = 0;
    quint64 videoPacketsReceived_ = 0;
    quint64 videoFramesDecoded_ = 0;
    quint64 videoFramesDropped_ = 0;
    quint64 videoFecRecoveries_ = 0;
    quint64 decodeErrors_ = 0;
    int consecutiveDecodeErrors_ = 0;
    uint64_t lastKeyframeRequestTimeNs_ = 0;
    VideoFrameAssembler videoAssembler_;
#if OXRSYS_QT_SIMULATOR_HAS_FFMPEG
    AVCodecContext* videoDecoder_ = nullptr;
    AVFrame* decodedFrame_ = nullptr;
    AVPacket* decodePacket_ = nullptr;
    SwsContext* swsContext_ = nullptr;
#endif
    QElapsedTimer poseClock_;
    QSet<int> pressedKeys_;
    QPointF lastMousePosition_;
    QPointF pendingMouseDelta_;
    bool hasLastMousePosition_ = false;
    bool mouseCaptured_ = false;
    oxrsys::qt_simulator::SimulatorTrackingPose trackingPose_;
};
