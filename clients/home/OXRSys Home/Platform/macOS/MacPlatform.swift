// SPDX-License-Identifier: MPL-2.0

import AppKit
import SwiftUI

enum MacPlatform {
    static func applicationIcon(for path: String) -> Image {
        Image(nsImage: NSWorkspace.shared.icon(forFile: path))
    }

    @discardableResult
    static func open(_ url: URL) -> Bool {
        NSWorkspace.shared.open(url)
    }
}
