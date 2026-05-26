// SPDX-License-Identifier: MPL-2.0

import Combine
import Foundation

final class HomePreferences: ObservableObject {
    static let developerModeEnabledKey = "developerModeEnabled"

    @Published var developerModeEnabled: Bool {
        didSet {
            defaults.set(developerModeEnabled, forKey: Self.developerModeEnabledKey)
        }
    }

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        developerModeEnabled = defaults.bool(forKey: Self.developerModeEnabledKey)
    }
}
