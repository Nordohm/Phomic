import Foundation
import Observation
import UIKit

/// Is there a newer Phomic on the App Store? Asks Apple's lookup service (what the App Store
/// itself shows) when Phomic comes on screen, at most every few hours. The request names only
/// the app. Offline, or no answer: nothing shows, and the next check tries again.
@MainActor
@Observable
final class UpdateCheck {
    /// The App Store's version, when it's newer than this one and its notice wasn't closed.
    private(set) var available: String?

    static let appStore = URL(string: "itms-apps://apps.apple.com/app/id6835323092")!
    private static let lookup = URL(string: "https://itunes.apple.com/lookup?id=6835323092")!
    private static let interval: TimeInterval = 6 * 3600
    private static let dismissedKey = "dismissedUpdate"

    @ObservationIgnored private var lastCheck: Date?
    @ObservationIgnored private var observer: NSObjectProtocol?

    init() {
        observer = NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkIfDue() }
        }
    }

    /// Not again for this version, only for the next one.
    func dismiss() {
        guard let available else { return }
        UserDefaults.standard.set(available, forKey: Self.dismissedKey)
        self.available = nil
    }

    private func checkIfDue() {
        if let lastCheck, Date().timeIntervalSince(lastCheck) < Self.interval { return }
        lastCheck = Date()
        Task { await check() }
    }

    private func check() async {
        var request = URLRequest(url: Self.lookup)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        guard let response = try? await URLSession.shared.data(for: request),
              let reply = try? JSONDecoder().decode(Lookup.self, from: response.0),
              let latest = reply.results.first?.version else { return }
        let current = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? ""
        let dismissed = UserDefaults.standard.string(forKey: Self.dismissedKey)
        available = Self.isVersion(latest, newerThan: current) && latest != dismissed ? latest : nil
    }

    /// "1.10.0" is newer than "1.9.2"; missing parts count as 0.
    nonisolated static func isVersion(_ candidate: String, newerThan current: String) -> Bool {
        let a = candidate.split(separator: ".").map { Int($0) ?? 0 }
        let b = current.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(a.count, b.count) {
            let x = i < a.count ? a[i] : 0
            let y = i < b.count ? b[i] : 0
            if x != y { return x > y }
        }
        return false
    }

    private struct Lookup: Decodable {
        struct App: Decodable {
            let version: String
        }
        let results: [App]
    }
}
