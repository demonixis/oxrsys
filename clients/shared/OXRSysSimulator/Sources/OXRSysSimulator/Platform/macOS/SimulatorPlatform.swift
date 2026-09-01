// SPDX-License-Identifier: MPL-2.0

#if os(macOS)
import AppKit

enum SimulatorPlatform {
    static func setPointerCaptured(_ captured: Bool) {
        CGAssociateMouseAndMouseCursorPosition(captured ? 0 : 1)
        if captured {
            NSCursor.hide()
        } else {
            NSCursor.unhide()
        }
    }
}
#endif
