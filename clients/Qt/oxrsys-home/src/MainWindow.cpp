// SPDX-License-Identifier: MPL-2.0

#include "MainWindow.h"

#include "PlatformSupport.h"
#include "SimulatorWidget.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <functional>

namespace
{

class ElidedLabel final : public QLabel
{
public:
    explicit ElidedLabel(QWidget* parent = nullptr)
        : QLabel(parent)
    {
        setWordWrap(false);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        setStyleSheet("color: palette(mid);");
    }

    void setFullText(const QString& text)
    {
        fullText_ = text;
        setToolTip(text);
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(120, QLabel::sizeHint().height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(40, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(),
                         alignment() | Qt::AlignVCenter,
                         fontMetrics().elidedText(fullText_, Qt::ElideMiddle, width()));
    }

private:
    QString fullText_;
};

class FlowLayout final : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 0, int spacing = 12)
        : QLayout(parent)
        , spacing_(spacing)
    {
        setContentsMargins(margin, margin, margin, margin);
    }

    ~FlowLayout() override
    {
        QLayoutItem* item = nullptr;
        while ((item = takeAt(0)) != nullptr)
        {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override
    {
        items_.append(item);
    }

    int count() const override
    {
        return items_.size();
    }

    QLayoutItem* itemAt(int index) const override
    {
        return items_.value(index);
    }

    QLayoutItem* takeAt(int index) override
    {
        if (index < 0 || index >= items_.size())
        {
            return nullptr;
        }
        return items_.takeAt(index);
    }

    Qt::Orientations expandingDirections() const override
    {
        return {};
    }

    bool hasHeightForWidth() const override
    {
        return true;
    }

    int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    QSize minimumSize() const override
    {
        QSize size;
        for (const QLayoutItem* item : items_)
        {
            size = size.expandedTo(item->minimumSize());
        }
        const QMargins margins = contentsMargins();
        size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        return size;
    }

    QSize sizeHint() const override
    {
        return minimumSize();
    }

    void setGeometry(const QRect& rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect effectiveRect = rect.adjusted(
            margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = effectiveRect.x();
        int y = effectiveRect.y();
        int lineHeight = 0;

        for (QLayoutItem* item : items_)
        {
            const QSize itemSize = item->sizeHint();
            const int nextX = x + itemSize.width() + spacing_;
            if (nextX - spacing_ > effectiveRect.right() && lineHeight > 0)
            {
                x = effectiveRect.x();
                y += lineHeight + spacing_;
                lineHeight = 0;
            }

            if (!testOnly)
            {
                item->setGeometry(QRect(QPoint(x, y), itemSize));
            }
            x += itemSize.width() + spacing_;
            lineHeight = std::max(lineHeight, itemSize.height());
        }

        return y + lineHeight - rect.y() + margins.bottom();
    }

    QList<QLayoutItem*> items_;
    int spacing_ = 12;
};

class AppDropLabel final : public QLabel
{
public:
    explicit AppDropLabel(QWidget* parent = nullptr)
        : QLabel(parent)
    {
        setAcceptDrops(true);
        setWordWrap(true);
        setFrameShape(QFrame::StyledPanel);
        setMargin(10);
        setStyleSheet("color: palette(mid);");
    }

    std::function<void(const QString&)> onPathDropped;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (hasLocalFile(event->mimeData()))
        {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent* event) override
    {
        const QList<QUrl> urls = event->mimeData()->urls();
        for (const QUrl& url : urls)
        {
            if (url.isLocalFile() && onPathDropped)
            {
                onPathDropped(url.toLocalFile());
            }
        }
        event->acceptProposedAction();
    }

private:
    static bool hasLocalFile(const QMimeData* mimeData)
    {
        if (mimeData == nullptr)
        {
            return false;
        }
        for (const QUrl& url : mimeData->urls())
        {
            if (url.isLocalFile())
            {
                return true;
            }
        }
        return false;
    }
};

QLabel* secondaryLabel(const QString& text = {})
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    return label;
}

QLabel* elidedSecondaryLabel(QWidget* parent = nullptr)
{
    return new ElidedLabel(parent);
}

void setElidedText(QLabel* label, const QString& text)
{
    if (auto* elided = dynamic_cast<ElidedLabel*>(label))
    {
        elided->setFullText(text);
        return;
    }
    label->setText(text);
    label->setToolTip(text);
}

QPushButton* iconButton(QWidget* parent,
                        QStyle::StandardPixmap icon,
                        const QString& text,
                        const QString& tooltip = {})
{
    auto* button = new QPushButton(text, parent);
    button->setIcon(parent->style()->standardIcon(icon));
    if (!tooltip.isEmpty())
    {
        button->setToolTip(tooltip);
    }
    return button;
}

QToolButton* appActionButton(QWidget* parent,
                             QStyle::StandardPixmap icon,
                             const QString& tooltip)
{
    auto* button = new QToolButton(parent);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setAutoRaise(false);
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(28, 26);
    button->setStyleSheet(
        "QToolButton {"
        "  border: 1px solid palette(midlight);"
        "  border-radius: 5px;"
        "  background: palette(button);"
        "}"
        "QToolButton:hover { background: palette(light); }"
        "QToolButton:disabled { color: palette(mid); background: palette(window); }");
    return button;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->deleteLater();
        }
        if (QLayout* childLayout = item->layout())
        {
            clearLayout(childLayout);
        }
        delete item;
    }
}

QString registrationButtonTitle(const HomeModel& model)
{
    const QString selected = normalizedPath(model.runtimeManifestPath());
    const QString current = normalizedPath(model.runtimeRegistrationStatus().activeRuntimeTarget);
    if (!selected.isEmpty() && selected == current)
    {
        return "Update Registration";
    }
    if (model.runtimeRegistrationStatus().activeRuntimeExists)
    {
        return "Update Registration";
    }
    return "Enable Registration";
}

} // namespace

class RuntimeStatsChart final : public QFrame
{
public:
    enum class Kind
    {
        Pipeline,
        Encode,
    };

    explicit RuntimeStatsChart(Kind kind, QWidget* parent = nullptr)
        : QFrame(parent)
        , kind_(kind)
    {
        setMinimumHeight(170);
        setFrameShape(QFrame::StyledPanel);
        setAutoFillBackground(true);
    }

    void setSamples(QList<RuntimeStreamingStats> samples)
    {
        samples_ = std::move(samples);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QFrame::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plotRect = rect().adjusted(16, 28, -16, -26);

        painter.setPen(palette().mid().color());
        painter.drawText(QRectF(14, 6, width() - 28, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         kind_ == Kind::Pipeline ? "Pipeline Latency" : "Encode Latency");

        for (int tick = 0; tick <= 2; ++tick)
        {
            const double y = plotRect.top() + plotRect.height() * tick / 2.0;
            painter.setPen(QColor(128, 128, 128, 45));
            painter.drawLine(QPointF(plotRect.left(), y), QPointF(plotRect.right(), y));
        }

        if (samples_.isEmpty())
        {
            painter.setPen(palette().mid().color());
            painter.drawText(plotRect, Qt::AlignCenter, "No samples");
            return;
        }

        const auto seriesValues = [this](const RuntimeStreamingStats& stats) {
            if (kind_ == Kind::Pipeline)
            {
                return QVector<double>{
                    stats.serverPipelineMs,
                    stats.clientPipelineMs,
                    stats.predictionHorizonMs,
                };
            }
            return QVector<double>{
                stats.encodeTotalP95Ms,
                stats.encodeQueueP95Ms,
            };
        };

        double maxValue = 1.0;
        for (const RuntimeStreamingStats& sample : samples_)
        {
            for (double value : seriesValues(sample))
            {
                maxValue = std::max(maxValue, value);
            }
        }
        maxValue *= 1.1;

        const QVector<QColor> colors = kind_ == Kind::Pipeline
            ? QVector<QColor>{QColor(214, 126, 36), QColor(31, 141, 163), QColor(92, 87, 180)}
            : QVector<QColor>{QColor(190, 57, 57), QColor(60, 110, 190)};

        for (int seriesIndex = 0; seriesIndex < colors.size(); ++seriesIndex)
        {
            QPainterPath path;
            for (int i = 0; i < samples_.size(); ++i)
            {
                const QVector<double> values = seriesValues(samples_.at(i));
                const double value = values.value(seriesIndex);
                const double x = samples_.size() == 1
                    ? plotRect.center().x()
                    : plotRect.left() + plotRect.width() * i / (samples_.size() - 1);
                const double y = plotRect.bottom() - plotRect.height() * std::min(value / maxValue, 1.0);
                if (i == 0)
                {
                    path.moveTo(x, y);
                }
                else
                {
                    path.lineTo(x, y);
                }
            }
            painter.setPen(QPen(colors.at(seriesIndex), 2.0));
            painter.drawPath(path);
        }

        painter.setPen(palette().mid().color());
        painter.drawText(QRectF(14, height() - 22, width() - 28, 16),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString("%1 ms").arg(maxValue, 0, 'f', 1));
    }

private:
    Kind kind_;
    QList<RuntimeStreamingStats> samples_;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , model_(new HomeModel(this))
{
    setWindowTitle("OXRSys Home");
    resize(1120, 780);

    buildUi();

    connect(model_, &HomeModel::changed, this, &MainWindow::refreshUi);
    connect(model_, &HomeModel::errorOccurred, this, [this](const QString& message) {
        QMessageBox box(QMessageBox::Warning, "OXRSys Home", message, QMessageBox::Ok, this);
        QAbstractButton* homebrewButton = nullptr;
        QAbstractButton* adbHelpButton = nullptr;
        if (message.contains("adb-enhanced", Qt::CaseInsensitive))
        {
            homebrewButton = box.addButton("Open Homebrew", QMessageBox::ActionRole);
        }
        else if (message.contains("adb", Qt::CaseInsensitive) ||
                 message.contains("USB", Qt::CaseInsensitive))
        {
            adbHelpButton = box.addButton("ADB Help", QMessageBox::ActionRole);
        }
        box.exec();
        if (homebrewButton != nullptr && box.clickedButton() == homebrewButton)
        {
            QDesktopServices::openUrl(
                QUrl("https://formulae.brew.sh/formula/adb-enhanced#default"));
        }
        else if (adbHelpButton != nullptr && box.clickedButton() == adbHelpButton)
        {
            QDesktopServices::openUrl(QUrl("https://developer.android.com/tools/adb"));
        }
    });

    refreshUi();
    QTimer::singleShot(0, this, &MainWindow::showRuntimeSetupGuidanceIfNeeded);
}

void MainWindow::showRuntimeSetupGuidanceIfNeeded()
{
    if (runtimeSetupGuidancePresented_ || !supportsRuntimeRegistration())
    {
        return;
    }

    const bool selectedActive =
        normalizedPath(model_->runtimeRegistrationStatus().activeRuntimeTarget) ==
        normalizedPath(model_->runtimeManifestPath());
    if (selectedActive)
    {
        return;
    }

    runtimeSetupGuidancePresented_ = true;
    if (tabs_ != nullptr)
    {
        tabs_->setCurrentIndex(1);
    }

    QMessageBox box(QMessageBox::Information,
                    "Runtime is not configured",
                    "OXRSys is not registered as the active OpenXR runtime. Register the selected runtime so compatible apps can find it outside this launcher.",
                    QMessageBox::NoButton,
                    this);
    QAbstractButton* registerButton = nullptr;
    if (QFileInfo(model_->runtimeManifestPath()).isFile())
    {
        registerButton = box.addButton("Register Runtime", QMessageBox::AcceptRole);
    }
    QAbstractButton* chooseButton = box.addButton("Choose Runtime JSON", QMessageBox::ActionRole);
    box.addButton("Later", QMessageBox::RejectRole);
    box.exec();

    if (registerButton != nullptr && box.clickedButton() == registerButton)
    {
        model_->setRuntimeManifestPath(runtimeManifestLineEdit_->text());
        model_->registerRuntime();
    }
    else if (box.clickedButton() == chooseButton)
    {
        chooseRuntimeManifest();
        if (QFileInfo(model_->runtimeManifestPath()).isFile())
        {
            model_->registerRuntime();
        }
    }
}

void MainWindow::buildUi()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 18, 20, 20);
    layout->setSpacing(12);

    layout->addWidget(buildHeader());

    tabs_ = new QTabWidget(central);
    tabs_->addTab(buildAppsTab(), "Apps");
    tabs_->addTab(buildSettingsTab(), "Settings");
    tabs_->addTab(buildStreamingTab(), "Streaming");
    developerTab_ = buildDeveloperTab();
    tabs_->addTab(developerTab_, "Developer");
    layout->addWidget(tabs_, 1);

    setCentralWidget(central);
}

QWidget* MainWindow::buildHeader()
{
    auto* box = new QGroupBox(this);
    auto* layout = new QHBoxLayout(box);
    layout->setSpacing(18);

    auto* titleLayout = new QVBoxLayout();
    auto* title = new QLabel("OXRSys Home", box);
    title->setStyleSheet("font-size: 22px; font-weight: 600;");
    auto* subtitle = secondaryLabel("Launch compatible apps, select a runtime, and tune headset streaming.");
    titleLayout->addWidget(title);
    titleLayout->addWidget(subtitle);
    layout->addLayout(titleLayout, 2);

    const auto addStatusItem = [box, layout](const QString& titleText, QLabel** valueLabel) {
        auto* itemLayout = new QVBoxLayout();
        auto* label = secondaryLabel(titleText);
        *valueLabel = new QLabel(box);
        (*valueLabel)->setStyleSheet("font-weight: 600;");
        (*valueLabel)->setMinimumWidth(130);
        itemLayout->addWidget(label);
        itemLayout->addWidget(*valueLabel);
        layout->addLayout(itemLayout);
    };

    addStatusItem("State", &stateValueLabel_);
    addStatusItem("Device", &deviceValueLabel_);
    addStatusItem("Profile App", &profileValueLabel_);

    auto* transportLayout = new QVBoxLayout();
    auto* transportLabel = secondaryLabel("Transport");
    transportCombo_ = new QComboBox(box);
    transportCombo_->addItem("WiFi", "wifi");
    transportCombo_->addItem("USB", "usb_adb");
    connect(transportCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        model_->setMainTransportSelection(transportCombo_->currentData().toString());
    });
    transportLayout->addWidget(transportLabel);
    transportLayout->addWidget(transportCombo_);
    layout->addLayout(transportLayout);

    readinessPillLabel_ = new QLabel(box);
    readinessMessageLabel_ = secondaryLabel();
    readinessMessageLabel_->setMinimumWidth(240);
    configureTransportButton_ =
        iconButton(box, QStyle::SP_ComputerIcon, "Configure", "Configure USB ADB reverse");
    connect(configureTransportButton_, &QPushButton::clicked,
            model_, &HomeModel::configureQuestUsbReverse);

    auto* readinessLayout = new QVBoxLayout();
    auto* readinessTop = new QHBoxLayout();
    readinessTop->addWidget(readinessPillLabel_);
    readinessTop->addWidget(configureTransportButton_);
    readinessTop->addStretch();
    readinessLayout->addLayout(readinessTop);
    readinessLayout->addWidget(readinessMessageLabel_);
    layout->addLayout(readinessLayout, 2);

    statusMessageLabel_ = secondaryLabel();
    statusMessageLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(statusMessageLabel_, 1);

    return box;
}

QWidget* MainWindow::buildAppsTab()
{
    auto* tab = new QWidget(this);
    auto* layout = new QVBoxLayout(tab);
    layout->setSpacing(12);

    auto* toolbar = new QHBoxLayout();
    auto* headingLayout = new QVBoxLayout();
    auto* heading = new QLabel("Apps", tab);
    heading->setStyleSheet("font-size: 16px; font-weight: 600;");
    appsCountLabel_ = secondaryLabel();
    headingLayout->addWidget(heading);
    headingLayout->addWidget(appsCountLabel_);
    toolbar->addLayout(headingLayout);
    toolbar->addStretch();

    auto* rescanButton = iconButton(tab, QStyle::SP_BrowserReload, "Rescan");
    auto* addButton = iconButton(tab, QStyle::SP_DialogOpenButton, "Add App");
    connect(rescanButton, &QPushButton::clicked, model_, &HomeModel::reloadLauncherApps);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::chooseLauncherApp);
    toolbar->addWidget(rescanButton);
    toolbar->addWidget(addButton);
    layout->addLayout(toolbar);

    auto* dropHint = new AppDropLabel(tab);
    dropHint->setText("Drop Linux executables or .desktop files, macOS .app bundles, or Windows executables here.");
    dropHint->onPathDropped = [this](const QString& path) {
        model_->addLauncherApp(path);
    };
    layout->addWidget(dropHint);

    appsScrollArea_ = new QScrollArea(tab);
    appsScrollArea_->setWidgetResizable(true);
    appsScrollArea_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    appsScrollArea_->setMinimumHeight(260);
    appsListWidget_ = new QWidget(appsScrollArea_);
    appsListLayout_ = new FlowLayout(appsListWidget_, 12, 12);
    appsScrollArea_->setWidget(appsListWidget_);
    layout->addWidget(appsScrollArea_, 4);

    auto* logsBox = new QGroupBox("Logs", tab);
    logsBox->setCheckable(true);
    logsBox->setChecked(false);
    auto* logsLayout = new QVBoxLayout(logsBox);
    auto* logsContent = new QWidget(logsBox);
    auto* logsContentLayout = new QVBoxLayout(logsContent);
    logsContentLayout->setContentsMargins(0, 4, 0, 0);
    auto* logsToolbar = new QHBoxLayout();
    logAppCombo_ = new QComboBox(logsBox);
    clearLogButton_ = iconButton(logsBox, QStyle::SP_TrashIcon, "Clear");
    connect(logAppCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        model_->setSelectedLogAppId(logAppCombo_->currentData().toString());
    });
    connect(clearLogButton_, &QPushButton::clicked, this, [this]() {
        const QString id = model_->selectedLogAppId();
        for (const LauncherApp& app : model_->launcherApps())
        {
            if (app.id() == id)
            {
                model_->clearLog(app);
                break;
            }
        }
    });
    logsToolbar->addWidget(logAppCombo_);
    logsToolbar->addWidget(clearLogButton_);
    logsToolbar->addStretch();
    logsContentLayout->addLayout(logsToolbar);
    logTextEdit_ = new QPlainTextEdit(logsBox);
    logTextEdit_->setReadOnly(true);
    logTextEdit_->setMaximumBlockCount(2000);
    logTextEdit_->setMinimumHeight(150);
    logsContentLayout->addWidget(logTextEdit_);
    logsLayout->addWidget(logsContent);
    logsContent->setVisible(false);
    connect(logsBox, &QGroupBox::toggled, logsContent, &QWidget::setVisible);
    layout->addWidget(logsBox);

    return tab;
}

QWidget* MainWindow::buildSettingsTab()
{
    auto* tab = new QWidget(this);
    auto* outerLayout = new QVBoxLayout(tab);

    auto* scroll = new QScrollArea(tab);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setSpacing(14);

    auto* developerBox = new QGroupBox("Developer", content);
    auto* developerLayout = new QVBoxLayout(developerBox);
    developerModeCheckBox_ = new QCheckBox("Developer Mode", developerBox);
    connect(developerModeCheckBox_, &QCheckBox::toggled,
            model_, &HomeModel::setDeveloperModeEnabled);
    developerLayout->addWidget(developerModeCheckBox_);
    layout->addWidget(developerBox);

    auto* registrationBox = new QGroupBox("Runtime Registration", content);
    auto* registrationLayout = new QVBoxLayout(registrationBox);
    auto* manifestRow = new QHBoxLayout();
    runtimeManifestLineEdit_ = new QLineEdit(registrationBox);
    auto* browseButton = iconButton(registrationBox, QStyle::SP_DialogOpenButton, "Browse");
    auto* revealButton = iconButton(registrationBox, QStyle::SP_DirOpenIcon, "Reveal");
    connect(runtimeManifestLineEdit_, &QLineEdit::editingFinished, this, [this]() {
        model_->setRuntimeManifestPath(runtimeManifestLineEdit_->text());
    });
    connect(browseButton, &QPushButton::clicked, this, &MainWindow::chooseRuntimeManifest);
    connect(revealButton, &QPushButton::clicked, this, [this]() {
        revealPath(model_->runtimeManifestPath(), "runtime manifest");
    });
    manifestRow->addWidget(runtimeManifestLineEdit_, 1);
    manifestRow->addWidget(browseButton);
    manifestRow->addWidget(revealButton);
    registrationLayout->addLayout(manifestRow);

    auto* registrationForm = new QFormLayout();
    registrationFileLabel_ = elidedSecondaryLabel(registrationBox);
    currentRuntimeTargetLabel_ = elidedSecondaryLabel(registrationBox);
    selectedRuntimeActiveLabel_ = new QLabel(registrationBox);
    launchTargetLabel_ = elidedSecondaryLabel(registrationBox);
    registrationForm->addRow("Registration file", registrationFileLabel_);
    registrationForm->addRow("Current target", currentRuntimeTargetLabel_);
    registrationForm->addRow("Selected target active", selectedRuntimeActiveLabel_);
    registrationForm->addRow("Launch target", launchTargetLabel_);
    registrationLayout->addLayout(registrationForm);

    auto* registrationButtons = new QHBoxLayout();
    auto* refreshButton = iconButton(registrationBox, QStyle::SP_BrowserReload, "Refresh");
    registerRuntimeButton_ = iconButton(registrationBox, QStyle::SP_DialogApplyButton, "Enable Registration");
    unregisterRuntimeButton_ = iconButton(registrationBox, QStyle::SP_DialogCancelButton, "Disable Registration");
    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        model_->refreshRuntimeStatus();
    });
    connect(registerRuntimeButton_, &QPushButton::clicked, this, [this]() {
        model_->setRuntimeManifestPath(runtimeManifestLineEdit_->text());
        model_->registerRuntime();
    });
    connect(unregisterRuntimeButton_, &QPushButton::clicked, model_, &HomeModel::unregisterRuntime);
    registrationButtons->addWidget(refreshButton);
    registrationButtons->addStretch();
    registrationButtons->addWidget(registerRuntimeButton_);
    registrationButtons->addWidget(unregisterRuntimeButton_);
    registrationLayout->addLayout(registrationButtons);
    layout->addWidget(registrationBox);

    layout->addStretch();
    scroll->setWidget(content);
    outerLayout->addWidget(scroll);
    return tab;
}

QWidget* MainWindow::buildStreamingTab()
{
    auto* tab = new QWidget(this);
    auto* outerLayout = new QVBoxLayout(tab);

    auto* scroll = new QScrollArea(tab);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setSpacing(14);

    auto* configBox = new QGroupBox("Streaming Configuration", content);
    auto* configLayout = new QVBoxLayout(configBox);
    runtimeEnabledCheckBox_ = new QCheckBox("Runtime enabled", configBox);
    fileLoggingCheckBox_ = new QCheckBox("Write server log file", configBox);
    questLogcatCheckBox_ = new QCheckBox("Capture Quest logcat", configBox);
    configLayout->addWidget(runtimeEnabledCheckBox_);
    configLayout->addWidget(fileLoggingCheckBox_);
    configLayout->addWidget(questLogcatCheckBox_);

    const auto addSlider = [configBox, configLayout](const QString& title,
                                                     QSlider** slider,
                                                     QLabel** valueLabel,
                                                     int min,
                                                     int max) {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(title, configBox);
        label->setMinimumWidth(150);
        *slider = new QSlider(Qt::Horizontal, configBox);
        (*slider)->setRange(min, max);
        *valueLabel = new QLabel(configBox);
        (*valueLabel)->setMinimumWidth(72);
        (*valueLabel)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(label);
        row->addWidget(*slider, 1);
        row->addWidget(*valueLabel);
        configLayout->addLayout(row);
    };

    addSlider("Bitrate",
              &bitrateSlider_,
              &bitrateValueLabel_,
              ServerConfig::MinBitrateMbps,
              ServerConfig::MaxBitrateMbps);
    addSlider("Resolution Scale", &resolutionSlider_, &resolutionValueLabel_, 25, 100);
    addSlider("Dynamic Resolution Min",
              &dynamicResolutionSlider_,
              &dynamicResolutionValueLabel_,
              25,
              100);
    addSlider("Keyframe Interval", &keyframeSlider_, &keyframeValueLabel_, 1, 10);

    auto* form = new QFormLayout();
    refreshRateCombo_ = new QComboBox(configBox);
    for (int rate : {60, 72, 80, 90, 120})
    {
        refreshRateCombo_->addItem(QString("%1 Hz").arg(rate), rate);
    }
    encoderPresetCombo_ = new QComboBox(configBox);
    encoderPresetCombo_->addItem("Quality", "quality");
    encoderPresetCombo_->addItem("Balanced", "balanced");
    encoderPresetCombo_->addItem("Speed", "speed");
    foveatedEncodingPresetCombo_ = new QComboBox(configBox);
    foveatedEncodingPresetCombo_->addItem("Off", "off");
    foveatedEncodingPresetCombo_->addItem("Light", "light");
    foveatedEncodingPresetCombo_->addItem("Medium", "medium");
    foveatedEncodingPresetCombo_->addItem("High", "high");
    configTransportCombo_ = new QComboBox(configBox);
    configTransportCombo_->addItem("Auto", "auto");
    configTransportCombo_->addItem("WiFi", "wifi");
    configTransportCombo_->addItem("USB ADB", "usb_adb");
    abrModeCombo_ = new QComboBox(configBox);
    abrModeCombo_->addItem("Off", "off");
    abrModeCombo_->addItem("Bitrate", "bitrate");
    abrModeCombo_->addItem("Full", "full");
    occlusionModeCombo_ = new QComboBox(configBox);
    occlusionModeCombo_->addItem("Off", "off");
    occlusionModeCombo_->addItem("Scene Mesh", "scene_mesh");
    occlusionModeCombo_->addItem("Environment Depth", "environment_depth");
    passthroughCheckBox_ = new QCheckBox("Passthrough", configBox);
    form->addRow("Refresh rate", refreshRateCombo_);
    form->addRow("Encoder preset", encoderPresetCombo_);
    form->addRow("Foveated encoding", foveatedEncodingPresetCombo_);
    form->addRow("Transport", configTransportCombo_);
    form->addRow("ABR mode", abrModeCombo_);
    form->addRow("Occlusion", occlusionModeCombo_);
    configLayout->addLayout(form);

    configLayout->addWidget(passthroughCheckBox_);
    spatialEnabledCheckBox_ = new QCheckBox("Spatial features", configBox);
    spatialAnchorsCheckBox_ = new QCheckBox("Spatial anchors", configBox);
    spatialSceneCheckBox_ = new QCheckBox("Scene scan", configBox);
    spatialPersistenceCheckBox_ = new QCheckBox("Spatial persistence", configBox);
    configLayout->addWidget(spatialEnabledCheckBox_);
    configLayout->addWidget(spatialAnchorsCheckBox_);
    configLayout->addWidget(spatialSceneCheckBox_);
    configLayout->addWidget(spatialPersistenceCheckBox_);

    auto* headsetBox = new QGroupBox("Headset Client", content);
    auto* headsetLayout = new QVBoxLayout(headsetBox);
    auto* headsetForm = new QFormLayout();
    clientFoveationPresetCombo_ = new QComboBox(headsetBox);
    clientFoveationPresetCombo_->addItem("Auto", "auto");
    clientFoveationPresetCombo_->addItem("Off", "off");
    clientFoveationPresetCombo_->addItem("Light", "light");
    clientFoveationPresetCombo_->addItem("Medium", "medium");
    clientFoveationPresetCombo_->addItem("High", "high");
    headsetForm->addRow("Client foveation", clientFoveationPresetCombo_);
    clientReprojectionCombo_ = new QComboBox(headsetBox);
    clientReprojectionCombo_->addItem("Off", "off");
    clientReprojectionCombo_->addItem("Pose", "pose");
    clientReprojectionCombo_->addItem("Pose Warp", "pose_warp");
    headsetForm->addRow("Client reprojection", clientReprojectionCombo_);
    headsetLayout->addLayout(headsetForm);
    clientUpscalingCheckBox_ = new QCheckBox("Quest shader upscaling", headsetBox);
    headsetAudioCheckBox_ = new QCheckBox("Headset audio", headsetBox);
    headsetLayout->addWidget(clientUpscalingCheckBox_);
    headsetLayout->addWidget(headsetAudioCheckBox_);

    const auto connectConfigChanged = [this]() {
        updateConfigFromControls();
    };
    connect(runtimeEnabledCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(fileLoggingCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(questLogcatCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(clientUpscalingCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(headsetAudioCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(spatialEnabledCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(spatialAnchorsCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(spatialSceneCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(spatialPersistenceCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(bitrateSlider_, &QSlider::valueChanged, this, connectConfigChanged);
    connect(resolutionSlider_, &QSlider::valueChanged, this, connectConfigChanged);
    connect(dynamicResolutionSlider_, &QSlider::valueChanged, this, connectConfigChanged);
    connect(keyframeSlider_, &QSlider::valueChanged, this, connectConfigChanged);
    connect(refreshRateCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(encoderPresetCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(foveatedEncodingPresetCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(clientFoveationPresetCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(clientReprojectionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(abrModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(occlusionModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);
    connect(passthroughCheckBox_, &QCheckBox::toggled, this, connectConfigChanged);
    connect(configTransportCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, connectConfigChanged);

    auto* configButtons = new QHBoxLayout();
    auto* defaultButton = iconButton(configBox, QStyle::SP_BrowserReload, "Default");
    auto* reloadButton = iconButton(configBox, QStyle::SP_BrowserReload, "Reload From Disk");
    auto* revealConfigButton = iconButton(configBox, QStyle::SP_DirOpenIcon, "Reveal Config");
    auto* revealRuntimeLogsButton =
        iconButton(configBox, QStyle::SP_DirOpenIcon, "Reveal Runtime Logs");
    connect(defaultButton, &QPushButton::clicked,
            model_, &HomeModel::resetStreamingConfigToDefaults);
    connect(reloadButton, &QPushButton::clicked, model_, &HomeModel::resetConfigFromDisk);
    connect(revealConfigButton, &QPushButton::clicked, this, [this]() {
        if (!QFileInfo(model_->paths().configFilePath).exists())
        {
            model_->saveStructuredConfig();
        }
        revealPath(model_->paths().configFilePath, "runtime configuration");
    });
    connect(revealRuntimeLogsButton, &QPushButton::clicked, this, [this]() {
        QDir().mkpath(model_->paths().stateRoot);
        revealPath(model_->paths().stateRoot, "runtime logs");
    });
    configButtons->addWidget(defaultButton);
    configButtons->addWidget(reloadButton);
    configButtons->addStretch();
    configButtons->addWidget(revealConfigButton);
    configButtons->addWidget(revealRuntimeLogsButton);
    configLayout->addLayout(configButtons);
    layout->addWidget(configBox);
    layout->addWidget(headsetBox);

    auto* usbBox = new QGroupBox("Quest USB ADB", content);
    auto* usbLayout = new QVBoxLayout(usbBox);
    auto* usbForm = new QFormLayout();
    usbDeviceCombo_ = new QComboBox(usbBox);
    usbForm->addRow("Quest device", usbDeviceCombo_);
    usbLayout->addLayout(usbForm);
    adbStatusLabel_ = secondaryLabel();
    adbStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    usbLayout->addWidget(adbStatusLabel_);
    auto* adbButtons = new QHBoxLayout();
    auto* selectAdbButton = iconButton(usbBox, QStyle::SP_DialogOpenButton, "Select ADB");
    clearAdbPathButton_ = iconButton(usbBox, QStyle::SP_BrowserReload, "Auto Detect");
    connect(selectAdbButton, &QPushButton::clicked, this, &MainWindow::chooseCustomAdbExecutable);
    connect(clearAdbPathButton_, &QPushButton::clicked, model_, &HomeModel::clearCustomAdbPath);
    adbButtons->addWidget(selectAdbButton);
    adbButtons->addWidget(clearAdbPathButton_);
    adbButtons->addStretch();
    usbLayout->addLayout(adbButtons);
    usbStatusLabel_ = secondaryLabel();
    usbLayout->addWidget(usbStatusLabel_);
    auto* usbButtons = new QHBoxLayout();
    auto* refreshDevicesButton = iconButton(usbBox, QStyle::SP_BrowserReload, "Refresh Devices");
    configureUsbButton_ = iconButton(usbBox, QStyle::SP_ComputerIcon, "Configure USB Reverse");
    connect(usbDeviceCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        model_->setSelectedQuestUsbSerial(usbDeviceCombo_->currentData().toString());
    });
    connect(refreshDevicesButton, &QPushButton::clicked, model_, &HomeModel::refreshQuestUsbDevices);
    connect(configureUsbButton_, &QPushButton::clicked, model_, &HomeModel::configureQuestUsbReverse);
    usbButtons->addWidget(refreshDevicesButton);
    usbButtons->addWidget(configureUsbButton_);
    usbButtons->addStretch();
    usbLayout->addLayout(usbButtons);
    layout->addWidget(usbBox);

    layout->addStretch();
    scroll->setWidget(content);
    outerLayout->addWidget(scroll);
    return tab;
}

QWidget* MainWindow::buildDeveloperTab()
{
    auto* tab = new QWidget(this);
    auto* scroll = new QScrollArea(tab);
    scroll->setWidgetResizable(true);
    auto* outerLayout = new QVBoxLayout(tab);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setSpacing(14);

    auto* simulatorBox = new QGroupBox("Simulator", content);
    auto* simulatorLayout = new QHBoxLayout(simulatorBox);
    auto* simulatorIcon = new QLabel(simulatorBox);
    simulatorIcon->setPixmap(style()->standardIcon(QStyle::SP_ComputerIcon).pixmap(32, 32));
    simulatorLayout->addWidget(simulatorIcon);
    auto* simulatorText = new QVBoxLayout();
    auto* simulatorTitle = new QLabel("OXRSys Simulator", simulatorBox);
    simulatorTitle->setStyleSheet("font-weight: 600;");
    simulatorText->addWidget(simulatorTitle);
    simulatorText->addWidget(secondaryLabel("Local streaming client"));
    simulatorLayout->addLayout(simulatorText, 1);
    auto* openSimulatorButton =
        iconButton(simulatorBox, QStyle::SP_MediaPlay, "Open Simulator");
    connect(openSimulatorButton, &QPushButton::clicked,
            this, &MainWindow::openSimulatorWindow);
    simulatorLayout->addWidget(openSimulatorButton);
    layout->addWidget(simulatorBox);

    auto* statsBox = new QGroupBox("Runtime Stats", content);
    auto* statsLayout = new QVBoxLayout(statsBox);
    auto* metricsGrid = new QGridLayout();
    metricsGrid->addWidget(buildMetric("Refresh", &refreshRateMetricLabel_, "Display target"), 0, 0);
    metricsGrid->addWidget(buildMetric("Bitrate", &bitrateMetricLabel_, "Current / max"), 0, 1);
    metricsGrid->addWidget(buildMetric("Render", &renderMetricLabel_, "Stereo source"), 0, 2);
    metricsGrid->addWidget(buildMetric("Encoded", &encodedMetricLabel_, "H.265 stream"), 0, 3);
    metricsGrid->addWidget(buildMetric("Scale", &scaleMetricLabel_, "Current / min"), 1, 0);
    metricsGrid->addWidget(buildMetric("Passthrough", &passthroughMetricLabel_, "State / occlusion"), 1, 1);
    metricsGrid->addWidget(buildMetric("Spatial", &spatialMetricLabel_, "State / config"), 1, 2);
    metricsGrid->addWidget(buildMetric("Server", &serverMetricLabel_, "Pipeline"), 1, 3);
    metricsGrid->addWidget(buildMetric("Client", &clientMetricLabel_, "Pipeline"), 2, 0);
    metricsGrid->addWidget(buildMetric("Horizon", &horizonMetricLabel_, "Prediction"), 2, 1);
    metricsGrid->addWidget(buildMetric("Drops", &dropsMetricLabel_, "Encoder total"), 2, 2);
    metricsGrid->addWidget(buildMetric("Frame Age", &frameAgeMetricLabel_, "Displayed"), 2, 3);
    metricsGrid->addWidget(buildMetric("ABR", &abrMetricLabel_, "State / profile"), 3, 0);
    metricsGrid->addWidget(buildMetric("Reprojection", &reprojectionMetricLabel_, "Frames / stale"), 3, 1);
    statsLayout->addLayout(metricsGrid);
    auto* chartsLayout = new QHBoxLayout();
    pipelineChart_ = new RuntimeStatsChart(RuntimeStatsChart::Kind::Pipeline, statsBox);
    encodeChart_ = new RuntimeStatsChart(RuntimeStatsChart::Kind::Encode, statsBox);
    chartsLayout->addWidget(pipelineChart_);
    chartsLayout->addWidget(encodeChart_);
    statsLayout->addLayout(chartsLayout);
    runtimeStatsEmptyLabel_ = secondaryLabel();
    statsLayout->addWidget(runtimeStatsEmptyLabel_);
    layout->addWidget(statsBox);
    layout->addStretch();

    scroll->setWidget(content);
    outerLayout->addWidget(scroll);
    return tab;
}

QWidget* MainWindow::buildAppCard(const LauncherApp& app)
{
    auto* card = new QFrame(appsListWidget_);
    card->setFrameShape(QFrame::StyledPanel);
    card->setFixedWidth(320);
    card->setMinimumHeight(154);
    auto* layout = new QVBoxLayout(card);

    auto* top = new QHBoxLayout();
    QFileIconProvider iconProvider;
    auto* iconLabel = new QLabel(card);
    iconLabel->setPixmap(iconProvider.icon(QFileInfo(app.path)).pixmap(42, 42));
    top->addWidget(iconLabel);

    auto* textLayout = new QVBoxLayout();
    auto* nameLabel = new QLabel(app.name, card);
    nameLabel->setStyleSheet("font-weight: 600;");
    nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textLayout->addWidget(nameLabel);
    auto* metaLabel = secondaryLabel(QString("%1 · %2").arg(app.kindDisplayName(), app.sourceDisplayName()));
    textLayout->addWidget(metaLabel);
    auto* pathLabel = elidedSecondaryLabel(card);
    setElidedText(pathLabel, app.path);
    textLayout->addWidget(pathLabel);
    top->addLayout(textLayout, 1);
    layout->addLayout(top);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);
    buttons->setContentsMargins(0, 4, 0, 0);
    auto* launchButton = appActionButton(card, QStyle::SP_MediaPlay, "Launch");
#if !defined(Q_OS_WIN)
    auto* terminalButton = appActionButton(card, QStyle::SP_ComputerIcon, "Open in terminal");
#endif
    auto* stopButton = appActionButton(card, QStyle::SP_MediaStop, "Stop");
    auto* logsButton = appActionButton(card, QStyle::SP_FileDialogDetailedView, "Show logs");
    auto* revealButton = appActionButton(card, QStyle::SP_DirOpenIcon, "Reveal in file manager");
    auto* removeButton = appActionButton(card, QStyle::SP_TrashIcon, "Remove");
    connect(launchButton, &QToolButton::clicked, model_, [this, app]() { model_->launchApp(app); });
#if !defined(Q_OS_WIN)
    connect(terminalButton, &QToolButton::clicked, model_, [this, app]() {
        model_->runAppInTerminal(app);
    });
#endif
    connect(stopButton, &QToolButton::clicked, model_, [this, app]() { model_->stopApp(app); });
    connect(logsButton, &QToolButton::clicked, model_, [this, app]() { model_->showLogs(app); });
    connect(revealButton, &QToolButton::clicked, this, [app]() { revealInFileManager(app.path); });
    connect(removeButton, &QToolButton::clicked, model_, [this, app]() { model_->removeLauncherApp(app); });
    launchButton->setEnabled(!model_->isAppRunning(app));
    stopButton->setEnabled(model_->isAppRunning(app));
    buttons->addWidget(launchButton);
#if !defined(Q_OS_WIN)
    buttons->addWidget(terminalButton);
#endif
    buttons->addWidget(stopButton);
    buttons->addWidget(logsButton);
    buttons->addWidget(revealButton);
    buttons->addStretch();
    buttons->addWidget(removeButton);
    layout->addLayout(buttons);

    if (model_->isAppRunning(app))
    {
        card->setStyleSheet("QFrame { border: 1px solid #3b8f45; border-radius: 6px; }");
    }
    return card;
}

QWidget* MainWindow::buildMetric(const QString& title, QLabel** valueLabel, const QString& subtitle)
{
    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(secondaryLabel(title));
    *valueLabel = new QLabel(frame);
    (*valueLabel)->setStyleSheet("font-weight: 600;");
    layout->addWidget(*valueLabel);
    layout->addWidget(secondaryLabel(subtitle));
    return frame;
}

void MainWindow::refreshUi()
{
    refreshHeader();
    refreshApps();
    refreshLogs();
    refreshSettings();
    refreshStreaming();
    refreshDeveloper();
    updateDeveloperTab();
}

void MainWindow::refreshHeader()
{
    const RuntimeActivity& activity = model_->runtimeActivity();
    stateValueLabel_->setText(activity.stateDisplayName());
    deviceValueLabel_->setText(activity.deviceDisplayName());
    profileValueLabel_->setText(model_->currentProfileAppDisplayName());
    statusMessageLabel_->setText(model_->statusMessage());

    const int transportIndex = transportCombo_->findData(model_->mainTransportSelection());
    transportCombo_->blockSignals(true);
    transportCombo_->setCurrentIndex(std::max(transportIndex, 0));
    transportCombo_->blockSignals(false);

    const TransportReadiness readiness = model_->mainTransportReadiness();
    setPill(readinessPillLabel_,
            readiness.isReady ? "Ready" : "Action needed",
            readiness.isReady ? QColor(42, 145, 72) : QColor(190, 57, 57));
    readinessMessageLabel_->setText(readiness.message);
    configureTransportButton_->setVisible(readiness.canConfigureUsb);
}

void MainWindow::refreshApps()
{
    appsCountLabel_->setText(QString("%1 compatible app%2")
                                 .arg(model_->launcherApps().size())
                                 .arg(model_->launcherApps().size() == 1 ? "" : "s"));

    clearLayout(appsListLayout_);
    for (const LauncherApp& app : model_->launcherApps())
    {
        appsListLayout_->addWidget(buildAppCard(app));
    }
    if (model_->launcherApps().isEmpty())
    {
        auto* empty = secondaryLabel("No compatible apps found. Add an executable to start launching with XR_RUNTIME_JSON.");
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumWidth(320);
        appsListLayout_->addWidget(empty);
    }
    appsListWidget_->updateGeometry();
}

void MainWindow::refreshLogs()
{
    const QString selectedId = model_->selectedLogAppId();
    logAppCombo_->blockSignals(true);
    logAppCombo_->clear();
    for (const LauncherApp& app : model_->launcherApps())
    {
        logAppCombo_->addItem(app.name, app.id());
        if (app.id() == selectedId)
        {
            logAppCombo_->setCurrentIndex(logAppCombo_->count() - 1);
        }
    }
    logAppCombo_->blockSignals(false);

    const QString effectiveId = selectedId.isEmpty()
        ? logAppCombo_->currentData().toString()
        : selectedId;
    logTextEdit_->setPlainText(model_->appLogs().value(effectiveId));
    clearLogButton_->setEnabled(!model_->appLogs().value(effectiveId).isEmpty());
}

void MainWindow::refreshSettings()
{
    developerModeCheckBox_->blockSignals(true);
    developerModeCheckBox_->setChecked(model_->developerModeEnabled());
    developerModeCheckBox_->blockSignals(false);

    runtimeManifestLineEdit_->blockSignals(true);
    runtimeManifestLineEdit_->setText(model_->runtimeManifestPath());
    runtimeManifestLineEdit_->blockSignals(false);

    setElidedText(registrationFileLabel_, model_->paths().activeRuntimePath);
    setElidedText(currentRuntimeTargetLabel_,
                  model_->runtimeRegistrationStatus().activeRuntimeTarget.isEmpty()
                      ? "Not registered"
                      : model_->runtimeRegistrationStatus().activeRuntimeTarget);
    const bool selectedActive =
        normalizedPath(model_->runtimeRegistrationStatus().activeRuntimeTarget) ==
        normalizedPath(model_->runtimeManifestPath());
    selectedRuntimeActiveLabel_->setText(selectedActive ? "Yes" : "No");
    setElidedText(launchTargetLabel_, model_->activeLaunchRuntimeManifestPath());
    registerRuntimeButton_->setText(registrationButtonTitle(*model_));
    registerRuntimeButton_->setEnabled(supportsRuntimeRegistration());
    unregisterRuntimeButton_->setEnabled(supportsRuntimeRegistration() &&
                                         model_->runtimeRegistrationStatus().activeRuntimeExists);
}

void MainWindow::refreshStreaming()
{
    const ServerConfig& config = model_->serverConfig();
    const QList<QWidget*> controls = {
        runtimeEnabledCheckBox_, fileLoggingCheckBox_, questLogcatCheckBox_,
        clientUpscalingCheckBox_, headsetAudioCheckBox_, passthroughCheckBox_, spatialEnabledCheckBox_,
        spatialAnchorsCheckBox_, spatialSceneCheckBox_, spatialPersistenceCheckBox_,
        bitrateSlider_, resolutionSlider_, dynamicResolutionSlider_, keyframeSlider_,
        refreshRateCombo_, encoderPresetCombo_, foveatedEncodingPresetCombo_,
        clientFoveationPresetCombo_, clientReprojectionCombo_, abrModeCombo_,
        occlusionModeCombo_, configTransportCombo_, usbDeviceCombo_,
    };
    for (QWidget* control : controls)
    {
        control->blockSignals(true);
    }

    runtimeEnabledCheckBox_->setChecked(config.runtimeEnabled);
    fileLoggingCheckBox_->setChecked(config.fileLogging);
    questLogcatCheckBox_->setChecked(config.questLogcat);
    clientUpscalingCheckBox_->setChecked(config.clientUpscaling);
    headsetAudioCheckBox_->setChecked(config.headsetAudio);
    passthroughCheckBox_->setChecked(config.passthroughEnabled);
    spatialEnabledCheckBox_->setChecked(config.spatialEnabled);
    spatialAnchorsCheckBox_->setChecked(config.spatialAnchors);
    spatialSceneCheckBox_->setChecked(config.spatialScene);
    spatialPersistenceCheckBox_->setChecked(config.spatialPersistence);
    bitrateSlider_->setValue(config.bitrateMbps);
    resolutionSlider_->setValue(qRound(config.resolutionScale * 100.0));
    dynamicResolutionSlider_->setValue(qRound(config.dynamicResolutionMinScale * 100.0));
    keyframeSlider_->setValue(config.keyframeIntervalSec);
    refreshRateCombo_->setCurrentIndex(std::max(refreshRateCombo_->findData(config.refreshRateHz), 0));
    encoderPresetCombo_->setCurrentIndex(std::max(encoderPresetCombo_->findData(config.encoderPreset), 0));
    foveatedEncodingPresetCombo_->setCurrentIndex(
        std::max(foveatedEncodingPresetCombo_->findData(config.foveatedEncodingPreset), 0));
    clientFoveationPresetCombo_->setCurrentIndex(
        std::max(clientFoveationPresetCombo_->findData(config.clientFoveationPreset), 0));
    clientReprojectionCombo_->setCurrentIndex(
        std::max(clientReprojectionCombo_->findData(config.clientReprojection), 0));
    abrModeCombo_->setCurrentIndex(std::max(abrModeCombo_->findData(config.abrMode), 0));
    occlusionModeCombo_->setCurrentIndex(
        std::max(occlusionModeCombo_->findData(config.occlusionMode), 0));
    configTransportCombo_->setCurrentIndex(std::max(configTransportCombo_->findData(config.transport), 0));

    usbDeviceCombo_->clear();
    usbDeviceCombo_->addItem("Select a device", QString());
    for (const AdbDevice& device : model_->questUsbDevices())
    {
        usbDeviceCombo_->addItem(device.displayName(), device.serial);
        if (device.serial == model_->selectedQuestUsbSerial())
        {
            usbDeviceCombo_->setCurrentIndex(usbDeviceCombo_->count() - 1);
        }
    }

    for (QWidget* control : controls)
    {
        control->blockSignals(false);
    }

    bitrateValueLabel_->setText(QString("%1 Mbps").arg(config.bitrateMbps));
    resolutionValueLabel_->setText(QString::number(config.resolutionScale, 'f', 2));
    dynamicResolutionValueLabel_->setText(
        QString::number(config.dynamicResolutionMinScale, 'f', 2));
    keyframeValueLabel_->setText(QString("%1 s").arg(config.keyframeIntervalSec));
    adbStatusLabel_->setText(model_->adbStatus().message);
    clearAdbPathButton_->setEnabled(!model_->customAdbPath().isEmpty());
    usbStatusLabel_->setText(model_->questUsbStatus());
    configureUsbButton_->setEnabled(!model_->selectedQuestUsbSerial().isEmpty());
}

void MainWindow::refreshDeveloper()
{
    const RuntimeActivity& activity = model_->runtimeActivity();
    const bool hasStats = activity.hasStreamingStats;
    const RuntimeStreamingStats stats = activity.streamingStats;

    refreshRateMetricLabel_->setText(stats.refreshRateHz > 0
                                         ? QString("%1 Hz").arg(stats.refreshRateHz)
                                         : "Unknown");
    bitrateMetricLabel_->setText(QString("%1 / %2 Mbps")
                                     .arg(stats.currentBitrateMbps)
                                     .arg(stats.maxBitrateMbps));
    renderMetricLabel_->setText(dimensionsText(stats.renderWidth, stats.renderHeight));
    encodedMetricLabel_->setText(dimensionsText(stats.encodedWidth, stats.encodedHeight));
    scaleMetricLabel_->setText(QString("%1 / %2")
                                   .arg(stats.resolutionScale, 0, 'f', 2)
                                   .arg(stats.dynamicResolutionMinScale, 0, 'f', 2));
    const QString passthroughState = !stats.passthroughEnabled
        ? QStringLiteral("off")
        : (stats.passthroughReady ? QStringLiteral("ready") : QStringLiteral("unsupported"));
    const QString passthroughDetail =
        stats.passthroughEnabled && !stats.passthroughSupported
            ? QStringLiteral("headset")
            : (stats.occlusionMode.isEmpty() ? QStringLiteral("off") : stats.occlusionMode);
    passthroughMetricLabel_->setText(QString("%1 / %2").arg(passthroughState, passthroughDetail));
    spatialMetricLabel_->setText(QString("%1 / #%2")
                                     .arg(stats.spatialEnabled ? QStringLiteral("on")
                                                               : QStringLiteral("off"))
                                     .arg(stats.streamConfigSequence));
    serverMetricLabel_->setText(millisecondsText(stats.serverPipelineMs));
    clientMetricLabel_->setText(millisecondsText(stats.clientPipelineMs));
    horizonMetricLabel_->setText(millisecondsText(stats.predictionHorizonMs));
    frameAgeMetricLabel_->setText(millisecondsText(stats.displayedFrameAgeMs));
    abrMetricLabel_->setText(stats.abrState.isEmpty()
                                 ? QStringLiteral("Off")
                                 : QString("%1 / %2")
                                       .arg(stats.abrState, stats.abrProfile.isEmpty()
                                           ? stats.abrMode
                                           : stats.abrProfile));
    reprojectionMetricLabel_->setText(QString("%1 / %2")
                                          .arg(stats.reprojectedFramesDelta)
                                          .arg(stats.staleFrameReusesDelta));
    dropsMetricLabel_->setText(QString::number(stats.encoderDroppedFramesTotal));
    runtimeStatsEmptyLabel_->setText(hasStats ? QString() :
        (activity.isStreaming() ? "Waiting for the first telemetry sample." : "Runtime is idle."));
    pipelineChart_->setSamples(model_->runtimeStatsHistory());
    encodeChart_->setSamples(model_->runtimeStatsHistory());
}

void MainWindow::updateDeveloperTab()
{
    const int index = tabs_->indexOf(developerTab_);
    if (model_->developerModeEnabled() && index < 0)
    {
        tabs_->addTab(developerTab_, "Developer");
    }
    else if (!model_->developerModeEnabled() && index >= 0)
    {
        tabs_->removeTab(index);
    }
}

void MainWindow::setPill(QLabel* label, const QString& text, const QColor& color)
{
    label->setText(text);
    label->setStyleSheet(QString(
        "QLabel { padding: 3px 8px; border-radius: 8px; color: %1; background: rgba(%2,%3,%4,36); font-weight: 600; }")
        .arg(color.name())
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue()));
}

void MainWindow::revealPath(const QString& path, const QString& label)
{
    if (path.isEmpty())
    {
        QMessageBox::warning(this, "OXRSys Home",
                             QString("No %1 path is selected.").arg(label));
        return;
    }

    if (!revealInFileManager(path))
    {
        QMessageBox::warning(this, "OXRSys Home",
                             QString("Could not reveal the %1:\n%2").arg(label, path));
    }
}

void MainWindow::chooseLauncherApp()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Add App",
        QDir::homePath(),
#if defined(Q_OS_WIN)
        "Applications (*.exe *.bat *.cmd);;All files (*)"
#elif defined(Q_OS_MACOS)
        "Applications and Executables (*.app *);;All files (*)"
#else
        "Applications and Executables (*.desktop *);;All files (*)"
#endif
    );
    if (!path.isEmpty())
    {
        model_->addLauncherApp(path);
    }
}

void MainWindow::chooseRuntimeManifest()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Choose Runtime JSON",
        QFileInfo(model_->runtimeManifestPath()).absolutePath(),
        "OpenXR runtime manifests (*.json);;All files (*)");
    if (!path.isEmpty())
    {
        model_->setRuntimeManifestPath(path);
    }
}

void MainWindow::chooseCustomAdbExecutable()
{
    const QString startPath = model_->customAdbPath().isEmpty()
        ? QDir::homePath()
        : QFileInfo(model_->customAdbPath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this,
        "Choose ADB",
        startPath,
#if defined(Q_OS_WIN)
        "ADB executable (adb.exe);;All files (*)"
#else
        "ADB executable (adb);;All files (*)"
#endif
    );
    if (!path.isEmpty())
    {
        model_->setCustomAdbPath(path);
    }
}

void MainWindow::openSimulatorWindow()
{
    if (!simulatorWindow_.isNull())
    {
        simulatorWindow_->show();
        simulatorWindow_->raise();
        simulatorWindow_->activateWindow();
        return;
    }

    auto* window = new QMainWindow(this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle("OXRSys Simulator");
    auto* simulator = new SimulatorWidget(window);
    window->setCentralWidget(simulator);
    window->resize(1280, 720);
    simulatorWindow_ = window;
    window->show();
}

void MainWindow::updateConfigFromControls()
{
    ServerConfig& config = model_->mutableServerConfig();
    config.runtimeEnabled = runtimeEnabledCheckBox_->isChecked();
    config.fileLogging = fileLoggingCheckBox_->isChecked();
    config.questLogcat = questLogcatCheckBox_->isChecked();
    config.clientUpscaling = clientUpscalingCheckBox_->isChecked();
    config.headsetAudio = headsetAudioCheckBox_->isChecked();
    config.passthroughEnabled = passthroughCheckBox_->isChecked();
    config.spatialEnabled = spatialEnabledCheckBox_->isChecked();
    config.spatialAnchors = spatialAnchorsCheckBox_->isChecked();
    config.spatialScene = spatialSceneCheckBox_->isChecked();
    config.spatialPersistence = spatialPersistenceCheckBox_->isChecked();
    config.bitrateMbps = bitrateSlider_->value();
    config.refreshRateHz = refreshRateCombo_->currentData().toInt();
    config.resolutionScale = resolutionSlider_->value() / 100.0;
    config.dynamicResolutionMinScale = dynamicResolutionSlider_->value() / 100.0;
    config.keyframeIntervalSec = keyframeSlider_->value();
    config.encoderPreset = encoderPresetCombo_->currentData().toString();
    config.foveatedEncodingPreset = foveatedEncodingPresetCombo_->currentData().toString();
    config.clientFoveationPreset = clientFoveationPresetCombo_->currentData().toString();
    config.clientReprojection = clientReprojectionCombo_->currentData().toString();
    config.abrMode = abrModeCombo_->currentData().toString();
    config.occlusionMode = occlusionModeCombo_->currentData().toString();
    config.transport = configTransportCombo_->currentData().toString();

    bitrateValueLabel_->setText(QString("%1 Mbps").arg(config.bitrateMbps));
    resolutionValueLabel_->setText(QString::number(config.resolutionScale, 'f', 2));
    dynamicResolutionValueLabel_->setText(
        QString::number(config.dynamicResolutionMinScale, 'f', 2));
    keyframeValueLabel_->setText(QString("%1 s").arg(config.keyframeIntervalSec));
    model_->scheduleStructuredConfigSave();
}
