// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "HomeModel.h"

#include <QMainWindow>
#include <QPointer>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGridLayout;
class QLabel;
class QLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QTabWidget;
class QVBoxLayout;

class RuntimeStatsChart;
class SimulatorWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi();
    QWidget* buildHeader();
    QWidget* buildAppsTab();
    QWidget* buildSettingsTab();
    QWidget* buildStreamingTab();
    QWidget* buildDeveloperTab();
    QWidget* buildAppCard(const LauncherApp& app);
    QWidget* buildMetric(const QString& title, QLabel** valueLabel, const QString& subtitle);
    void refreshUi();
    void refreshHeader();
    void refreshApps();
    void refreshLogs();
    void refreshSettings();
    void refreshStreaming();
    void refreshDeveloper();
    void updateDeveloperTab();
    void setPill(QLabel* label, const QString& text, const QColor& color);
    void revealPath(const QString& path, const QString& label);
    void chooseLauncherApp();
    void chooseRuntimeManifest();
    void chooseCustomAdbExecutable();
    void openSimulatorWindow();
    void updateConfigFromControls();

    HomeModel* model_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* developerTab_ = nullptr;

    QLabel* statusMessageLabel_ = nullptr;
    QLabel* stateValueLabel_ = nullptr;
    QLabel* deviceValueLabel_ = nullptr;
    QLabel* profileValueLabel_ = nullptr;
    QComboBox* transportCombo_ = nullptr;
    QLabel* readinessPillLabel_ = nullptr;
    QLabel* readinessMessageLabel_ = nullptr;
    QPushButton* configureTransportButton_ = nullptr;

    QLabel* appsCountLabel_ = nullptr;
    QScrollArea* appsScrollArea_ = nullptr;
    QWidget* appsListWidget_ = nullptr;
    QLayout* appsListLayout_ = nullptr;
    QComboBox* logAppCombo_ = nullptr;
    QPushButton* clearLogButton_ = nullptr;
    QPlainTextEdit* logTextEdit_ = nullptr;

    QCheckBox* developerModeCheckBox_ = nullptr;
    QLineEdit* runtimeManifestLineEdit_ = nullptr;
    QLabel* registrationFileLabel_ = nullptr;
    QLabel* currentRuntimeTargetLabel_ = nullptr;
    QLabel* selectedRuntimeActiveLabel_ = nullptr;
    QLabel* launchTargetLabel_ = nullptr;
    QPushButton* registerRuntimeButton_ = nullptr;
    QPushButton* unregisterRuntimeButton_ = nullptr;

    QCheckBox* runtimeEnabledCheckBox_ = nullptr;
    QCheckBox* fileLoggingCheckBox_ = nullptr;
    QCheckBox* questLogcatCheckBox_ = nullptr;
    QCheckBox* clientUpscalingCheckBox_ = nullptr;
    QCheckBox* headsetAudioCheckBox_ = nullptr;
    QSlider* bitrateSlider_ = nullptr;
    QLabel* bitrateValueLabel_ = nullptr;
    QSlider* resolutionSlider_ = nullptr;
    QLabel* resolutionValueLabel_ = nullptr;
    QSlider* dynamicResolutionSlider_ = nullptr;
    QLabel* dynamicResolutionValueLabel_ = nullptr;
    QSlider* keyframeSlider_ = nullptr;
    QLabel* keyframeValueLabel_ = nullptr;
    QComboBox* refreshRateCombo_ = nullptr;
    QComboBox* encoderPresetCombo_ = nullptr;
    QComboBox* foveatedEncodingPresetCombo_ = nullptr;
    QComboBox* clientFoveationPresetCombo_ = nullptr;
    QComboBox* clientReprojectionCombo_ = nullptr;
    QComboBox* abrModeCombo_ = nullptr;
    QComboBox* occlusionModeCombo_ = nullptr;
    QComboBox* configTransportCombo_ = nullptr;
    QCheckBox* passthroughCheckBox_ = nullptr;
    QCheckBox* spatialEnabledCheckBox_ = nullptr;
    QCheckBox* spatialAnchorsCheckBox_ = nullptr;
    QCheckBox* spatialSceneCheckBox_ = nullptr;
    QCheckBox* spatialPersistenceCheckBox_ = nullptr;
    QComboBox* usbDeviceCombo_ = nullptr;
    QLabel* adbStatusLabel_ = nullptr;
    QLabel* usbStatusLabel_ = nullptr;
    QPushButton* clearAdbPathButton_ = nullptr;
    QPushButton* configureUsbButton_ = nullptr;

    QLabel* refreshRateMetricLabel_ = nullptr;
    QLabel* bitrateMetricLabel_ = nullptr;
    QLabel* renderMetricLabel_ = nullptr;
    QLabel* encodedMetricLabel_ = nullptr;
    QLabel* scaleMetricLabel_ = nullptr;
    QLabel* passthroughMetricLabel_ = nullptr;
    QLabel* spatialMetricLabel_ = nullptr;
    QLabel* serverMetricLabel_ = nullptr;
    QLabel* clientMetricLabel_ = nullptr;
    QLabel* horizonMetricLabel_ = nullptr;
    QLabel* frameAgeMetricLabel_ = nullptr;
    QLabel* abrMetricLabel_ = nullptr;
    QLabel* reprojectionMetricLabel_ = nullptr;
    QLabel* dropsMetricLabel_ = nullptr;
    QLabel* runtimeStatsEmptyLabel_ = nullptr;
    RuntimeStatsChart* pipelineChart_ = nullptr;
    RuntimeStatsChart* encodeChart_ = nullptr;
    QPointer<QMainWindow> simulatorWindow_;
};
