// SPDX-License-Identifier: MPL-2.0

import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @ObservedObject var model: CompanionAppModel

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

            TabView {
                appsTab
                    .tabItem {
                        Label("Apps", systemImage: "square.grid.2x2")
                    }
                runtimeTab
                    .tabItem {
                        Label("Runtime", systemImage: "shippingbox")
                    }
                streamingTab
                    .tabItem {
                        Label("Streaming", systemImage: "antenna.radiowaves.left.and.right")
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
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 4) {
                Text("OpenXR OSX Companion")
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

    private var runtimeTab: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                runtimeInstallSection
                runtimeRegistrationSection
            }
            .padding(.top, 14)
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
                    TextField("Path to OpenXR runtime JSON", text: $model.runtimeManifestPath)
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

                    HStack {
                        Button("Save Configuration") {
                            model.saveStructuredConfig()
                        }
                        Button("Reload From Disk") {
                            model.resetToDisk()
                        }
                        Spacer()
                        Button("Reveal Config") {
                            revealInFinder(model.configFilePath)
                        }
                    }
                }
                .padding(.top, 8)
            }
            .padding(.top, 14)
        }
    }
}

private struct LauncherAppCard: View {
    let app: LauncherApp
    @ObservedObject var model: CompanionAppModel

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
    ContentView(model: CompanionAppModel())
}
