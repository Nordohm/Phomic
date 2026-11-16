import Foundation
import Network

struct DiscoveredServer: Identifiable, Hashable, @unchecked Sendable {
    var id: String { name }
    let name: String
    let endpoint: NWEndpoint
    /// From the TXT record: the PC's id and whether it requires a pairing code.
    let pcId: String?
    let requiresCode: Bool
}

/// Finds PCs advertising `_phomic._tcp` with Bonjour.
final class ServerBrowser: @unchecked Sendable {
    var onChange: (@Sendable ([DiscoveredServer]) -> Void)?
    /// A user-facing problem (e.g. local network access denied), or nil when browsing works.
    var onProblem: (@Sendable (String?) -> Void)?

    private let queue = DispatchQueue(label: "phomic.browser")
    private var browser: NWBrowser?

    func start() {
        queue.async { self.startBrowsing() }
    }

    private func startBrowsing() {
        guard browser == nil else { return }
        let parameters = NWParameters()
        parameters.includePeerToPeer = false
        let browser = NWBrowser(for: .bonjourWithTXTRecord(type: WireProtocol.serviceType, domain: nil), using: parameters)
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            self?.publish(results)
        }
        browser.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.onProblem?(nil)
            case .waiting(let error):
                self.onProblem?(Self.describe(error))
            case .failed(let error):
                self.onProblem?(Self.describe(error))
                self.browser?.cancel()
                self.browser = nil
                self.queue.asyncAfter(deadline: .now() + 3) { self.startBrowsing() }
            default:
                break
            }
        }
        browser.start(queue: queue)
        self.browser = browser
    }

    private func publish(_ results: Set<NWBrowser.Result>) {
        var byName: [String: DiscoveredServer] = [:]
        for result in results {
            if case let .service(name, _, _, _) = result.endpoint {
                var pcId: String?
                var requiresCode = false
                if case let .bonjour(txt) = result.metadata {
                    pcId = txt["id"]
                    requiresCode = txt["pair"] == "1"
                }
                byName[name] = DiscoveredServer(name: name, endpoint: result.endpoint, pcId: pcId, requiresCode: requiresCode)
            }
        }
        // The same PC can show up twice, as "PC" and "PC (1)", while Windows re-advertises it:
        // keep one per PC id, preferring the name without Bonjour's conflict number.
        var byPC: [String: DiscoveredServer] = [:]
        var withoutId: [DiscoveredServer] = []
        for server in byName.values {
            guard let id = server.pcId?.uppercased() else {
                withoutId.append(server)
                continue
            }
            if let kept = byPC[id], Self.preference(kept) <= Self.preference(server) {
                continue
            }
            byPC[id] = server
        }
        let servers = (Array(byPC.values) + withoutId)
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        onChange?(servers)
    }

    /// Lower is better: no " (n)" suffix first, then the shorter name.
    private static func preference(_ server: DiscoveredServer) -> Int {
        let numbered = server.name.range(of: #"\s?\(\d+\)$"#, options: .regularExpression) != nil
        return (numbered ? 1000 : 0) + server.name.count
    }

    private static func describe(_ error: NWError) -> String {
        if case .dns(let code) = error, code == -65570 { // kDNSServiceErr_PolicyDenied
            return "Phomic can't see your network. Allow Local Network access in Settings › Privacy & Security › Local Network."
        }
        return "Searching for PCs failed: \(error.localizedDescription)"
    }
}
