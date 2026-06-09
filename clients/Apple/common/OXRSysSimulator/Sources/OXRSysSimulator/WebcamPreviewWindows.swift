// SPDX-License-Identifier: MPL-2.0

#if os(macOS)
import AppKit
import SwiftUI

final class WebcamPreviewWindowManager {
    var onAllWindowsClosed: (() -> Void)?

    private struct Entry {
        var state: WebcamPreviewWindowState
        var window: NSWindow
        var delegate: WebcamPreviewWindowDelegate
    }

    private var entries: [String: Entry] = [:]

    var isOpen: Bool {
        !entries.isEmpty
    }

    func frame(for cameraID: String) -> WebcamPreviewFrame? {
        entries[cameraID]?.state.frame
    }

    func showWindows(for devices: [DesktopWebcamDevice]) {
        for device in devices {
            _ = ensureWindow(cameraID: device.id, cameraName: device.name)
        }
    }

    func show(frame: WebcamPreviewFrame) {
        guard let entry = entries[frame.cameraID] else { return }
        entry.state.frame = frame
    }

    func closeAll() {
        let windows = entries.values.map(\.window)
        entries.removeAll()
        for window in windows {
            window.delegate = nil
            window.close()
        }
    }

    private func ensureWindow(cameraID: String, cameraName: String) -> WebcamPreviewWindowState {
        if let entry = entries[cameraID] {
            entry.window.makeKeyAndOrderFront(nil)
            return entry.state
        }

        let state = WebcamPreviewWindowState(cameraName: cameraName)
        let view = WebcamPreviewWindowView(state: state)
        let window = NSWindow(
            contentRect: NSRect(x: 160, y: 160, width: 720, height: 480),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Camera Preview - \(cameraName)"
        window.contentMinSize = NSSize(width: 420, height: 280)
        window.isReleasedWhenClosed = false
        window.contentView = NSHostingView(rootView: view)

        let delegate = WebcamPreviewWindowDelegate { [weak self] in
            Task { @MainActor [weak self] in
                self?.entries.removeValue(forKey: cameraID)
                if self?.entries.isEmpty == true {
                    self?.onAllWindowsClosed?()
                }
            }
        }
        window.delegate = delegate
        entries[cameraID] = Entry(state: state, window: window, delegate: delegate)
        window.makeKeyAndOrderFront(nil)
        return state
    }
}

private final class WebcamPreviewWindowState: ObservableObject {
    let cameraName: String
    @Published var frame: WebcamPreviewFrame?

    init(cameraName: String) {
        self.cameraName = cameraName
    }
}

private final class WebcamPreviewWindowDelegate: NSObject, NSWindowDelegate {
    private let onClose: () -> Void

    init(onClose: @escaping () -> Void) {
        self.onClose = onClose
    }

    func windowWillClose(_ notification: Notification) {
        onClose()
    }
}

private struct WebcamPreviewWindowView: View {
    @ObservedObject var state: WebcamPreviewWindowState

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color.black.ignoresSafeArea()

            if let frame = state.frame, let image = frame.image {
                GeometryReader { proxy in
                    let bounds = CGRect(origin: .zero, size: proxy.size)
                    let imageSize = CGSize(width: frame.imageWidth, height: frame.imageHeight)
                    let drawRect = aspectFitRect(imageSize: imageSize, in: bounds)

                    Image(decorative: image, scale: 1)
                        .resizable()
                        .interpolation(.medium)
                        .aspectRatio(imageSize, contentMode: .fit)
                        .frame(width: drawRect.width, height: drawRect.height)
                        .position(x: drawRect.midX, y: drawRect.midY)

                    Canvas { context, _ in
                        drawPreviewOverlay(frame: frame, drawRect: drawRect, context: &context)
                    }
                }
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    Text(state.cameraName)
                        .font(.headline)
                    Text("Waiting for camera frames")
                        .foregroundStyle(.secondary)
                }
                .padding()
            }

            if let frame = state.frame {
                HStack(spacing: 8) {
                    Text(frame.cameraName)
                    Text("\(frame.points.count) pts")
                        .monospacedDigit()
                    if let debugText = frame.debugText {
                        Text(debugText)
                            .monospacedDigit()
                    }
                }
                .font(.caption)
                .foregroundStyle(.white)
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(.black.opacity(0.55), in: Capsule())
                .padding(10)
            }
        }
    }

    private func aspectFitRect(imageSize: CGSize, in bounds: CGRect) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0, bounds.width > 0, bounds.height > 0 else {
            return bounds
        }
        let scale = min(bounds.width / imageSize.width, bounds.height / imageSize.height)
        let width = imageSize.width * scale
        let height = imageSize.height * scale
        return CGRect(
            x: bounds.midX - width * 0.5,
            y: bounds.midY - height * 0.5,
            width: width,
            height: height
        )
    }

    private func drawPreviewOverlay(
        frame: WebcamPreviewFrame,
        drawRect: CGRect,
        context: inout GraphicsContext
    ) {
        let border = Path(drawRect)
        context.stroke(border, with: .color(.white.opacity(0.25)), lineWidth: 1)

        for point in frame.points {
            let position = point.pixelPosition(in: drawRect)
            let radius = point.kind == .head ? CGFloat(7) : CGFloat(4)
            let color = color(for: point.kind).opacity(Double(max(point.confidence, 0.35)))
            let rect = CGRect(
                x: position.x - radius,
                y: position.y - radius,
                width: radius * 2,
                height: radius * 2
            )
            context.fill(Path(ellipseIn: rect), with: .color(color))
            context.stroke(Path(ellipseIn: rect), with: .color(.black.opacity(0.55)), lineWidth: 1)
        }
    }

    private func color(for kind: WebcamPreviewPointKind) -> Color {
        switch kind {
        case .head:
            return .cyan
        case .leftHand:
            return .green
        case .rightHand:
            return .orange
        }
    }
}
#endif
