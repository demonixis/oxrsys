// SPDX-License-Identifier: MPL-2.0

import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @ObservedObject var model: HomeAppModel
    @ObservedObject var preferences: HomePreferences
    @Environment(\.openWindow) private var openWindow

    private var selectedLogApp: LauncherApp? {
        guard let selectedLogAppID = model.selectedLogAppID else {
            return nil
        }
        return model.launcherApps.first { $0.id == selectedLogAppID }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
                .padding(.horizontal, 20)
                .padding(.top, 18)
                .padding(.bottom, 10)

            runtimeActivitySummary
                .padding(.horizontal, 20)
                .padding(.bottom, 8)

            TabView {
                appsTab
                    .tabItem {
                        Label("Apps", systemImage: "square.grid.2x2")
                    }
                settingsTab
                    .tabItem {
                        Label("Settings", systemImage: "gearshape")
                    }
                streamingTab
                    .tabItem {
                        Label("Streaming", systemImage: "antenna.radiowaves.left.and.right")
                    }
                if preferences.developerModeEnabled {
                    developerTab
                        .tabItem {
                            Label("Developer", systemImage: "hammer")
                        }
                }
            }
            .padding(.horizontal, 20)
            .padding(.bottom, 20)
        }
        .frame(minWidth: 980, minHeight: 720)
        .alert("Error", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { newValue in
                if !newValue {
                    model.errorMessage = nil
                }
            })
        ) {
            Button("OK", role: .cancel) {
                model.errorMessage = nil
            }
        } message: {
            Text(model.errorMessage ?? "")
        }
        .alert(HomeAdbInstallGuidance.title, isPresented: Binding(
            get: { model.isAdbInstallGuidancePresented },
            set: { newValue in
                if !newValue {
                    model.dismissAdbInstallGuidance()
                }
            })
        ) {
            Button("Open Homebrew") {
                model.openAdbInstallHelp()
            }
            Button("OK", role: .cancel) {
                model.dismissAdbInstallGuidance()
            }
        } message: {
            Text(HomeAdbInstallGuidance.message)
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 4) {
                Text("OXRSys Home")
                    .font(.title)
                    .fontWeight(.semibold)
                Text("Launch compatible apps, install the runtime, and tune headset streaming.")
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if !model.statusMessage.isEmpty {
                Text(model.statusMessage)
                    .font(.subheadline)
                    .foregroundStyle(.green)
                    .lineLimit(2)
                    .multilineTextAlignment(.trailing)
            }
        }
    }

    private var runtimeActivitySummary: some View {
        HStack(spacing: 18) {
            RuntimeStatusItem(
                title: "State",
                value: model.runtimeActivity.stateDisplayName,
                systemImage: model.runtimeActivity.state == .streaming ? "dot.radiowaves.left.and.right" : "pause.circle",
                color: model.runtimeActivity.state == .streaming ? .green : .secondary
            )
            Divider()
                .frame(height: 30)
            RuntimeStatusItem(
                title: "Device",
                value: model.runtimeActivity.deviceDisplayName,
                systemImage: "visionpro",
                color: model.runtimeActivity.state == .streaming ? .accentColor : .secondary
            )
            Divider()
                .frame(height: 30)
            RuntimeStatusItem(
                title: "Profile App",
                value: model.currentProfileAppDisplayName,
                systemImage: "app.badge",
                color: model.currentProfileAppDisplayName == "None" ? .secondary : .accentColor
            )
            Divider()
                .frame(height: 30)
            transportHealthControl
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .background(Color(nsColor: .controlBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.secondary.opacity(0.18))
        )
    }

    private var transportHealthControl: some View {
        let readiness = model.mainTransportReadiness
        return HStack(spacing: 8) {
            Picker("Transport", selection: Binding(
                get: { model.mainTransportSelection },
                set: { selection in
                    guard selection != model.mainTransportSelection else {
                        return
                    }
                    Task { @MainActor in
                        await Task.yield()
                        model.setMainTransportSelection(selection)
                    }
                }
            )) {
                ForEach(HomePrimaryTransport.allCases) { transport in
                    Text(transport.displayName).tag(transport)
                }
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .frame(width: 120)

            StatusPill(
                title: readiness.isReady ? "Ready" : "Action needed",
                color: readiness.isReady ? .green : .red
            )

            Text(readiness.message)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 280, alignment: .leading)

            if readiness.canConfigureUsb {
                Button {
                    model.configureQuestUsbReverse()
                } label: {
                    Label("Configure", systemImage: "cable.connector")
                }
            }
        }
        .frame(minWidth: 360, alignment: .leading)
    }

    private var appsTab: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Apps")
                        .font(.headline)
                    Text("\(model.launcherApps.count) compatible app\(model.launcherApps.count == 1 ? "" : "s")")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    model.reloadLauncherApps()
                } label: {
                    Label("Rescan", systemImage: "arrow.clockwise")
                }
                Button {
                    model.chooseLauncherApp()
                } label: {
                    Label("Add App", systemImage: "plus")
                }
                .buttonStyle(.borderedProminent)
            }

            appDropArea

            ScrollView {
                LazyVGrid(
                    columns: [GridItem(.adaptive(minimum: 260, maximum: 340), spacing: 14)],
                    alignment: .leading,
                    spacing: 14
                ) {
                    ForEach(model.launcherApps) { app in
                        LauncherAppCard(app: app, model: model)
                    }
                }
                .padding(.vertical, 2)
            }

            logsSection
        }
        .padding(.top, 14)
    }

    private var appDropArea: some View {
        HStack(spacing: 10) {
            Image(systemName: "arrow.down.app")
                .font(.title3)
                .foregroundStyle(model.isDropTargeted ? Color.accentColor : Color.secondary)
            Text("Drop a .app bundle here to add it to the launcher.")
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(12)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(model.isDropTargeted ? Color.accentColor.opacity(0.12) : Color.secondary.opacity(0.08))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(model.isDropTargeted ? Color.accentColor : Color.secondary.opacity(0.2))
        )
        .onDrop(of: [UTType.fileURL], isTargeted: $model.isDropTargeted) { providers in
            model.handleAppDrop(providers)
        }
    }

    private var logsSection: some View {
        DisclosureGroup(
            isExpanded: Binding(
                get: { model.isLogPanelVisible },
                set: { model.setLogPanelVisible($0) }
            )
        ) {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    if let selectedLogApp {
                        Picker("App", selection: Binding(
                            get: { model.selectedLogAppID ?? selectedLogApp.id },
                            set: { model.selectedLogAppID = $0 }
                        )) {
                            ForEach(model.launcherApps) { app in
                                Text(app.name).tag(app.id)
                            }
                        }
                        .labelsHidden()
                        .frame(width: 220)

                        Button {
                            model.clearLog(for: selectedLogApp)
                        } label: {
                            Label("Clear", systemImage: "trash")
                        }
                        .disabled((model.appLogs[selectedLogApp.id] ?? "").isEmpty)
                    } else {
                        Text("No app selected.")
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                }

                ScrollView {
                    Text(selectedLogApp.map { model.appLogs[$0.id] ?? "" } ?? "")
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .padding(10)
                }
                .frame(minHeight: 150, maxHeight: 190)
                .background(Color(nsColor: .textBackgroundColor))
                .clipShape(RoundedRectangle(cornerRadius: 8))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(Color.secondary.opacity(0.25))
                )
            }
            .padding(.top, 8)
        } label: {
            HStack {
                Text("Logs")
                    .font(.headline)
                if let selectedLogApp {
                    Text(selectedLogApp.name)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
        }
        .padding(.top, 2)
    }

    private var settingsTab: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                developerSettingsSection
                runtimeInstallSection
                runtimeRegistrationSection
            }
            .padding(.top, 14)
        }
    }

    private var developerSettingsSection: some View {
        GroupBox("Developer") {
            Toggle("Developer Mode", isOn: $preferences.developerModeEnabled)
                .padding(.top, 8)
        }
    }

    private var runtimeInstallSection: some View {
        GroupBox("Runtime Installation") {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 8) {
                        LabeledContent("Bundled runtime") {
                            StatusPill(
                                title: model.runtimeInstallStatus.bundledRuntimeExists ? "Available" : "Not embedded",
                                color: model.runtimeInstallStatus.bundledRuntimeExists ? .green : .secondary
                            )
                        }
                        LabeledContent("Installed runtime") {
                            StatusPill(
                                title: model.runtimeInstallStatus.installedRuntimeExists ? "Installed" : "Not installed",
                                color: model.runtimeInstallStatus.installedRuntimeExists ? .green : .secondary
                            )
                        }
                        LabeledContent("Update state") {
                            StatusPill(
                                title: model.runtimeInstallStatus.installedRuntimeNeedsUpdate ? "Update available" : "Current",
                                color: model.runtimeInstallStatus.installedRuntimeNeedsUpdate ? .orange : .secondary
                            )
                        }
                    }

                    Spacer()

                    VStack(alignment: .trailing, spacing: 8) {
                        Button {
                            model.installBundledRuntimeAndRegister()
                        } label: {
                            Label(model.runtimeInstallButtonTitle, systemImage: "square.and.arrow.down")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!model.canInstallBundledRuntime)

                        Button {
                            model.useInstalledRuntimeManifest()
                        } label: {
                            Label("Use Installed Manifest", systemImage: "checkmark.circle")
                        }
                        .disabled(!model.runtimeInstallStatus.installedManifestExists)

                        Button {
                            revealInFinder(model.runtimeInstallStatus.installedManifestPath)
                        } label: {
                            Label("Reveal Installed Runtime", systemImage: "finder")
                        }
                        .disabled(!model.runtimeInstallStatus.installedRuntimeExists)
                    }
                }

                VStack(alignment: .leading, spacing: 6) {
                    Text(model.runtimeInstallStatus.installedManifestPath)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if let bundledPath = model.runtimeInstallStatus.bundledRuntimePath {
                        Text(bundledPath)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                }
            }
            .padding(.top, 8)
        }
    }

    private var runtimeRegistrationSection: some View {
        GroupBox("Runtime Registration") {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    TextField("Path to OXRSys runtime JSON", text: $model.runtimeManifestPath)
                        .textFieldStyle(.roundedBorder)
                    Button("Browse") {
                        model.chooseRuntimeManifest()
                    }
                    Button("Reveal") {
                        revealInFinder(model.runtimeManifestPath)
                    }
                }

                VStack(alignment: .leading, spacing: 8) {
                    LabeledContent("Registration file", value: model.activeRuntimePath)
                    LabeledContent("Current target", value: model.runtimeStatus.activeRuntimeTarget ?? "Not registered")
                    LabeledContent("Selected target active", value: model.isSelectedRuntimeRegistered ? "Yes" : "No")
                    LabeledContent("Launch target", value: model.activeLaunchRuntimeManifestPath)
                }

                HStack {
                    Button("Refresh") {
                        model.refreshRuntimeStatus()
                        model.refreshRuntimeInstallStatus()
                    }
                    Spacer()
                    Button(model.registrationButtonTitle) {
                        model.toggleRuntimeRegistration()
                    }
                    .buttonStyle(.borderedProminent)
                }
            }
            .padding(.top, 8)
        }
    }

    private var streamingTab: some View {
        ScrollView {
            GroupBox("Streaming Configuration") {
                VStack(alignment: .leading, spacing: 16) {
                    Toggle("Runtime enabled", isOn: $model.serverConfig.runtimeEnabled)
                    Toggle("Write server log file", isOn: $model.serverConfig.fileLogging)
                    Toggle("Capture Quest logcat", isOn: $model.serverConfig.questLogcat)

                    LabeledSlider(
                        title: "Bitrate",
                        value: Binding(
                            get: { Double(model.serverConfig.bitrateMbps) },
                            set: { model.serverConfig.bitrateMbps = Int($0.rounded()) }
                        ),
                        range: 1...200,
                        displayValue: "\(model.serverConfig.bitrateMbps) Mbps"
                    )

                    LabeledSlider(
                        title: "Vertical FOV",
                        value: Binding(
                            get: { Double(model.serverConfig.fovDegrees) },
                            set: { model.serverConfig.fovDegrees = Int($0.rounded()) }
                        ),
                        range: 60...150,
                        displayValue: "\(model.serverConfig.fovDegrees) degrees"
                    )

                    LabeledSlider(
                        title: "Resolution Scale",
                        value: $model.serverConfig.resolutionScale,
                        range: 0.25...1.0,
                        displayValue: String(format: "%.2f", model.serverConfig.resolutionScale)
                    )

                    LabeledSlider(
                        title: "Keyframe Interval",
                        value: Binding(
                            get: { Double(model.serverConfig.keyframeIntervalSec) },
                            set: { model.serverConfig.keyframeIntervalSec = Int($0.rounded()) }
                        ),
                        range: 1...10,
                        displayValue: "\(model.serverConfig.keyframeIntervalSec) s"
                    )

                    Picker("Encoder preset", selection: $model.serverConfig.encoderPreset) {
                        ForEach(EncoderPreset.allCases) { preset in
                            Text(preset.rawValue.capitalized).tag(preset)
                        }
                    }

                    Picker("Transport", selection: $model.serverConfig.transport) {
                        ForEach(StreamingTransportSetting.allCases) { transport in
                            Text(transport.displayName).tag(transport)
                        }
                    }

                    HStack {
                        Button("Save Configuration") {
                            model.saveStructuredConfig()
                        }
                        Button("Reload From Disk") {
                            model.resetToDisk()
                        }
                        Spacer()
                        Button("Reveal Config") {
                            model.revealConfigFile()
                        }
                    }
                }
                .padding(.top, 8)
            }
            .padding(.top, 14)

            GroupBox("Quest USB ADB") {
                VStack(alignment: .leading, spacing: 12) {
                    if model.questUsbDevices.isEmpty {
                        Text("No adb device found.")
                            .foregroundStyle(.secondary)
                    } else {
                        Picker("Quest device", selection: Binding(
                            get: { model.selectedQuestUsbSerial ?? "" },
                            set: { model.selectedQuestUsbSerial = $0.isEmpty ? nil : $0 }
                        )) {
                            Text("Select a device").tag("")
                            ForEach(model.questUsbDevices) { device in
                                Text(device.displayName).tag(device.serial)
                            }
                        }
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Text(model.adbStatus.message)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                        if !model.customAdbPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                            Text("Custom ADB: \(model.customAdbPath)")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }

                    Text(model.questUsbStatus)
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Button("Select ADB") {
                            model.chooseCustomAdbExecutable()
                        }
                        Button("Auto Detect") {
                            model.clearCustomAdbPath()
                        }
                        .disabled(model.customAdbPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                        Divider()
                        Button("Refresh Devices") {
                            model.refreshQuestUsbDevices()
                        }
                        Button("Configure USB Reverse") {
                            model.configureQuestUsbReverse()
                        }
                        .disabled(model.selectedQuestUsbSerial == nil ||
                                  !model.questUsbDevices.contains(where: {
                                      $0.serial == model.selectedQuestUsbSerial && $0.isUsable
                                  }))
                        Spacer()
                    }
                }
                .padding(.top, 8)
            }
            .padding(.top, 14)
        }
    }

    private var developerTab: some View {
        ScrollView {
            GroupBox("Simulator") {
                HStack(spacing: 12) {
                    Image(systemName: "display")
                        .font(.title2)
                        .foregroundStyle(Color.accentColor)
                    VStack(alignment: .leading, spacing: 3) {
                        Text("OXRSys Simulator")
                            .font(.headline)
                        Text("Local streaming client")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button {
                        openWindow(id: HomeWindowID.simulator)
                    } label: {
                        Label("Open Simulator", systemImage: "play.rectangle")
                    }
                    .buttonStyle(.borderedProminent)
                }
                .padding(.top, 8)
            }
            .padding(.top, 14)

            runtimeStatsSection
                .padding(.top, 14)
        }
    }

    private var runtimeStatsSection: some View {
        GroupBox("Runtime Stats") {
            if let latest = model.latestRuntimeStats {
                VStack(alignment: .leading, spacing: 14) {
                    LazyVGrid(
                        columns: Array(repeating: GridItem(.flexible(minimum: 128), spacing: 10), count: 4),
                        alignment: .leading,
                        spacing: 10
                    ) {
                        RuntimeStatsMetric(
                            title: "Refresh",
                            value: latest.refreshRateHz > 0 ? "\(latest.refreshRateHz) Hz" : "Unknown",
                            subtitle: "Display target",
                            systemImage: "speedometer",
                            color: .accentColor
                        )
                        RuntimeStatsMetric(
                            title: "Bitrate",
                            value: "\(latest.currentBitrateMbps) / \(latest.maxBitrateMbps) Mbps",
                            subtitle: "Current / max",
                            systemImage: "gauge.with.dots.needle.67percent",
                            color: .green
                        )
                        RuntimeStatsMetric(
                            title: "Render",
                            value: dimensions(width: latest.renderWidth, height: latest.renderHeight),
                            subtitle: "Stereo source",
                            systemImage: "rectangle.3.group",
                            color: .blue
                        )
                        RuntimeStatsMetric(
                            title: "Encoded",
                            value: dimensions(width: latest.encodedWidth, height: latest.encodedHeight),
                            subtitle: "H.265 stream",
                            systemImage: "rectangle.compress.vertical",
                            color: .purple
                        )
                        RuntimeStatsMetric(
                            title: "Server",
                            value: formatMilliseconds(latest.latency.serverPipelineMs),
                            subtitle: "Pipeline",
                            systemImage: "desktopcomputer",
                            color: .orange
                        )
                        RuntimeStatsMetric(
                            title: "Client",
                            value: formatMilliseconds(latest.latency.clientPipelineMs),
                            subtitle: "Pipeline",
                            systemImage: "visionpro",
                            color: .teal
                        )
                        RuntimeStatsMetric(
                            title: "Horizon",
                            value: formatMilliseconds(latest.latency.predictionHorizonMs),
                            subtitle: "Prediction",
                            systemImage: "scope",
                            color: .indigo
                        )
                        RuntimeStatsMetric(
                            title: "Drops",
                            value: "\(latest.counters.encoderDroppedFramesTotal)",
                            subtitle: "Encoder total",
                            systemImage: latest.counters.encoderDroppedFramesTotal > 0 ?
                                "exclamationmark.triangle" : "checkmark.circle",
                            color: latest.counters.encoderDroppedFramesTotal > 0 ? .red : .green
                        )
                    }

                    HStack(alignment: .top, spacing: 12) {
                        RuntimeStatsChart(
                            title: "Pipeline Latency",
                            unit: "ms",
                            samples: model.runtimeStatsHistory,
                            series: [
                                RuntimeStatsSeries(name: "Server", color: .orange) {
                                    $0.latency.serverPipelineMs
                                },
                                RuntimeStatsSeries(name: "Client", color: .teal) {
                                    $0.latency.clientPipelineMs
                                },
                                RuntimeStatsSeries(name: "Horizon", color: .indigo) {
                                    $0.latency.predictionHorizonMs
                                },
                            ]
                        )
                        RuntimeStatsChart(
                            title: "Encode Latency",
                            unit: "ms",
                            samples: model.runtimeStatsHistory,
                            series: [
                                RuntimeStatsSeries(name: "Total p95", color: .red) {
                                    $0.encode.totalP95Ms
                                },
                                RuntimeStatsSeries(name: "Queue p95", color: .blue) {
                                    $0.encode.queueP95Ms
                                },
                            ]
                        )
                    }
                }
                .padding(.top, 8)
            } else {
                HStack(spacing: 8) {
                    Image(systemName: model.runtimeActivity.state == .streaming ? "clock" : "pause.circle")
                        .foregroundStyle(.secondary)
                    Text(model.runtimeActivity.state == .streaming ?
                         "Waiting for the first telemetry sample." : "Runtime is idle.")
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.top, 8)
            }
        }
    }

    private func dimensions(width: Int, height: Int) -> String {
        guard width > 0 && height > 0 else {
            return "Unknown"
        }
        return "\(width) x \(height)"
    }

    private func formatMilliseconds(_ value: Double) -> String {
        if value >= 100 {
            return String(format: "%.0f ms", value)
        }
        return String(format: "%.1f ms", value)
    }
}

private struct LauncherAppCard: View {
    let app: LauncherApp
    @ObservedObject var model: HomeAppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 12) {
                Image(nsImage: NSWorkspace.shared.icon(forFile: app.bundlePath))
                    .resizable()
                    .frame(width: 48, height: 48)

                VStack(alignment: .leading, spacing: 4) {
                    Text(app.name)
                        .font(.headline)
                        .lineLimit(1)
                    HStack(spacing: 6) {
                        StatusPill(title: app.kind.displayName, color: .accentColor)
                        StatusPill(title: app.source.displayName, color: .secondary)
                    }
                    Text(app.bundlePath)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }

                Spacer()
            }

            HStack(spacing: 8) {
                Button {
                    model.launchApp(app)
                } label: {
                    Label("Launch", systemImage: "play.fill")
                }
                .disabled(model.isAppRunning(app))

                Button {
                    model.stopApp(app)
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
                .disabled(!model.isAppRunning(app))

                Button {
                    model.runAppInTerminal(app)
                } label: {
                    Label("Terminal", systemImage: "terminal")
                }

                Spacer()

                Button {
                    model.showLogs(for: app)
                } label: {
                    Image(systemName: "doc.text")
                }
                .help("Show logs")

                Button {
                    revealInFinder(app.bundlePath)
                } label: {
                    Image(systemName: "finder")
                }
                .help("Reveal in Finder")

                Button(role: .destructive) {
                    model.removeLauncherApp(app)
                } label: {
                    Image(systemName: "minus.circle")
                }
                .help("Remove")
            }
            .labelStyle(.iconOnly)
        }
        .padding(12)
        .frame(minHeight: 154)
        .background(Color(nsColor: .controlBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(model.isAppRunning(app) ? Color.green.opacity(0.6) : Color.secondary.opacity(0.18))
        )
    }
}

private struct StatusPill: View {
    let title: String
    let color: Color

    var body: some View {
        Text(title)
            .font(.caption)
            .fontWeight(.medium)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(color.opacity(0.14))
            .foregroundStyle(color)
            .clipShape(Capsule())
    }
}

private struct RuntimeStatusItem: View {
    let title: String
    let value: String
    let systemImage: String
    let color: Color

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: systemImage)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(value)
                    .font(.subheadline)
                    .fontWeight(.medium)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .frame(minWidth: 150, alignment: .leading)
    }
}

private struct RuntimeStatsMetric: View {
    let title: String
    let value: String
    let subtitle: String
    let systemImage: String
    let color: Color

    var body: some View {
        HStack(spacing: 9) {
            Image(systemName: systemImage)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 24)

            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(value)
                    .font(.subheadline)
                    .fontWeight(.semibold)
                    .monospacedDigit()
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
                Text(subtitle)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 9)
        .frame(maxWidth: .infinity, minHeight: 70, alignment: .leading)
        .background(Color(nsColor: .controlBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.secondary.opacity(0.16))
        )
    }
}

private struct RuntimeStatsSeries {
    let name: String
    let color: Color
    let value: (HomeRuntimeStreamingStats) -> Double
}

private struct RuntimeStatsChart: View {
    let title: String
    let unit: String
    let samples: [HomeRuntimeStreamingStats]
    let series: [RuntimeStatsSeries]

    private var maximumValue: Double {
        let values = samples.flatMap { sample in
            series.map { max(0, $0.value(sample)) }
        }
        return max(values.max() ?? 1, 1)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title)
                    .font(.subheadline)
                    .fontWeight(.semibold)
                Spacer()
                Text(String(format: "%.1f", maximumValue) + " \(unit)")
                    .font(.caption)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            Canvas { context, size in
                let maxValue = maximumValue * 1.1
                let plotRect = CGRect(
                    x: 0,
                    y: 6,
                    width: max(size.width, 1),
                    height: max(size.height - 12, 1)
                )

                for tick in 0...2 {
                    let y = plotRect.minY + plotRect.height * CGFloat(tick) / 2.0
                    var gridPath = Path()
                    gridPath.move(to: CGPoint(x: plotRect.minX, y: y))
                    gridPath.addLine(to: CGPoint(x: plotRect.maxX, y: y))
                    context.stroke(gridPath, with: .color(.secondary.opacity(0.16)), lineWidth: 1)
                }

                guard !samples.isEmpty else {
                    return
                }

                for item in series {
                    var path = Path()
                    for (index, sample) in samples.enumerated() {
                        let x: CGFloat
                        if samples.count == 1 {
                            x = plotRect.midX
                        } else {
                            x = plotRect.minX +
                                plotRect.width * CGFloat(index) / CGFloat(samples.count - 1)
                        }
                        let value = max(0, item.value(sample))
                        let normalized = min(value / maxValue, 1)
                        let y = plotRect.maxY - plotRect.height * CGFloat(normalized)
                        let point = CGPoint(x: x, y: y)
                        if index == 0 {
                            path.move(to: point)
                        } else {
                            path.addLine(to: point)
                        }
                    }

                    context.stroke(path, with: .color(item.color), lineWidth: 2)

                    if samples.count == 1, let sample = samples.first {
                        let value = max(0, item.value(sample))
                        let normalized = min(value / maxValue, 1)
                        let point = CGPoint(
                            x: plotRect.midX,
                            y: plotRect.maxY - plotRect.height * CGFloat(normalized)
                        )
                        context.fill(
                            Path(ellipseIn: CGRect(x: point.x - 2, y: point.y - 2, width: 4, height: 4)),
                            with: .color(item.color)
                        )
                    }
                }
            }
            .frame(height: 118)

            HStack(spacing: 12) {
                ForEach(Array(series.enumerated()), id: \.offset) { _, item in
                    HStack(spacing: 5) {
                        RoundedRectangle(cornerRadius: 1)
                            .fill(item.color)
                            .frame(width: 14, height: 3)
                        Text(item.name)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(nsColor: .controlBackgroundColor))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.secondary.opacity(0.16))
        )
    }
}

private struct LabeledSlider<Value: BinaryFloatingPoint>: View where Value.Stride: BinaryFloatingPoint {
    let title: String
    @Binding var value: Value
    let range: ClosedRange<Value>
    let displayValue: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title)
                Spacer()
                Text(displayValue)
                    .foregroundStyle(.secondary)
            }
            Slider(value: $value, in: range)
        }
    }
}

#Preview {
    ContentView(model: HomeAppModel(), preferences: HomePreferences())
}
