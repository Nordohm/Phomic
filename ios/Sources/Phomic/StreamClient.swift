import Foundation
import Network
import os

/// Talks to the Windows app: a TCP control connection (hello / pings / mute / bye)
/// plus a UDP flow that carries the audio packets.
///
/// All state lives on `queue`; every public method hops onto it.
final class StreamClient: @unchecked Sendable {
    enum Event: Sendable {
        case connecting
        case connected(serverName: String, pcId: String?)
        /// The PC wants a (different) pairing code before it lets this iPhone in.
        case pairingRequired(pcId: String?, pcName: String?, message: String)
        case roundTrip(milliseconds: Double)
        /// The PC asks the iPhone to move this session to another transport ("bluetooth").
        case switchRequested(transport: String)
        /// `reason` is nil when we hung up ourselves. `canRetry` is false when the PC
        /// deliberately ended or refused the session.
        case disconnected(reason: String?, canRetry: Bool)
    }

    var onEvent: (@Sendable (Event) -> Void)?

    private let queue = DispatchQueue(label: "phomic.stream", qos: .userInteractive)
    private let deviceName: String

    private var control: NWConnection?
    private var audio: NWConnection?
    private var generation = 0
    private var rxBuffer = Data()
    private var session: UInt32 = 0
    private var sequence: UInt32 = 0
    private var streaming = false
    private var muted = false
    private var serverName = ""
    private var serverId: String?
    private var pairingCode: String?
    private var pingTimer: DispatchSourceTimer?
    private var nextPingID = 1
    private var pingSentAt: [Int: UInt64] = [:]
    private var lastRoundTrip: Double = 0
    private var lastReceive: UInt64 = 0

    init(deviceName: String) {
        self.deviceName = deviceName
    }

    func connect(to endpoint: NWEndpoint, code: String?) {
        queue.async {
            self.pairingCode = code
            self.openControl(to: endpoint)
        }
    }

    func disconnect() {
        queue.async {
            guard let control = self.control else { return }
            // Let "bye" reach the PC before closing, so it shows a clean disconnect.
            if let bye = Self.encode(ControlMessage(type: "bye")) {
                control.send(content: bye, completion: .contentProcessed { _ in control.cancel() })
                self.control = nil
            }
            self.teardown()
            self.onEvent?(.disconnected(reason: nil, canRetry: false))
        }
    }

    func setMuted(_ muted: Bool) {
        queue.async {
            self.muted = muted
            if self.streaming {
                self.sendControl(.mute(muted))
            }
        }
    }

    /// Whether a session is set up, so idle frames can be dropped without hopping queues.
    private let active = OSAllocatedUnfairLock(initialState: false)

    /// `frame` is 480 samples of 48 kHz mono audio in -1...1.
    func send(frame: [Float]) {
        guard active.withLock({ $0 }) else { return }
        queue.async {
            guard self.streaming, !self.muted, let audio = self.audio else { return }
            var packet = Data(capacity: 12 + frame.count * 2)
            packet.append(contentsOf: WireProtocol.magic)
            withUnsafeBytes(of: self.session.littleEndian) { packet.append(contentsOf: $0) }
            withUnsafeBytes(of: self.sequence.littleEndian) { packet.append(contentsOf: $0) }
            for sample in frame {
                let value = Int16(max(-1, min(1, sample)) * 32767)
                withUnsafeBytes(of: value.littleEndian) { packet.append(contentsOf: $0) }
            }
            self.sequence &+= 1
            audio.send(content: packet, completion: .idempotent)
        }
    }

    // MARK: - Control connection

    private func openControl(to endpoint: NWEndpoint) {
        teardown()
        let gen = generation

        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.connectionTimeout = 6
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 2
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.includePeerToPeer = false
        parameters.serviceClass = .responsiveData

        let connection = NWConnection(to: endpoint, using: parameters)
        control = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, gen == self.generation else { return }
            self.controlStateChanged(state)
        }
        connection.start(queue: queue)
        receive(on: connection, gen: gen)
        onEvent?(.connecting)
    }

    private func controlStateChanged(_ state: NWConnection.State) {
        switch state {
        case .ready:
            lastReceive = Self.now()
            sendControl(.hello(name: deviceName, code: pairingCode))
        case .waiting(let error), .failed(let error):
            fail(Self.describe(error), canRetry: true)
        default:
            break
        }
    }

    private func receive(on connection: NWConnection, gen: Int) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] data, _, isComplete, error in
            guard let self, gen == self.generation else { return }
            if let data, !data.isEmpty {
                self.lastReceive = Self.now()
                self.rxBuffer.append(data)
                self.processLines()
                guard gen == self.generation else { return } // a message ended the session
            }
            if isComplete {
                self.fail("The PC closed the connection.", canRetry: true)
            } else if let error {
                self.fail(Self.describe(error), canRetry: true)
            } else {
                self.receive(on: connection, gen: gen)
            }
        }
    }

    private func processLines() {
        while let newline = rxBuffer.firstIndex(of: 0x0A) {
            let line = Data(rxBuffer[rxBuffer.startIndex..<newline])
            rxBuffer.removeSubrange(rxBuffer.startIndex...newline)
            guard !line.isEmpty, let message = try? JSONDecoder().decode(ControlMessage.self, from: line) else {
                continue
            }
            handle(message)
            if control == nil { return }
        }
    }

    private func handle(_ message: ControlMessage) {
        switch message.type {
        case "welcome":
            guard let session = message.session, let port = message.udpPort,
                  let udpPort = NWEndpoint.Port(rawValue: UInt16(clamping: port)) else {
                fail("The PC sent an unexpected reply.", canRetry: false)
                return
            }
            self.session = session
            serverName = message.name ?? "PC"
            serverId = message.pcId
            openAudio(port: udpPort)
        case "pong":
            if let id = message.id, let sentAt = pingSentAt.removeValue(forKey: id) {
                lastRoundTrip = Double(Self.now() - sentAt) / 1_000_000
                onEvent?(.roundTrip(milliseconds: lastRoundTrip))
            }
        case "switch":
            if let transport = message.transport {
                onEvent?(.switchRequested(transport: transport))
            }
        case "bye":
            teardown()
            onEvent?(.disconnected(reason: "The PC ended the connection.", canRetry: false))
        case "error" where message.reason == "pairing":
            teardown()
            onEvent?(.pairingRequired(pcId: message.pcId, pcName: message.name,
                                      message: message.message ?? "This PC needs a pairing code."))
        case "error":
            teardown()
            onEvent?(.disconnected(reason: message.message ?? "The PC refused the connection.", canRetry: false))
        default:
            break
        }
    }

    // MARK: - Audio flow

    private func openAudio(port: NWEndpoint.Port) {
        guard let control else { return }

        // Send audio to the exact address the TCP connection resolved to.
        let host: NWEndpoint.Host
        if case let .hostPort(resolved, _)? = control.currentPath?.remoteEndpoint {
            host = resolved
        } else if case let .hostPort(requested, _) = control.endpoint {
            host = requested
        } else {
            fail("Couldn't work out the PC's address.", canRetry: true)
            return
        }

        let parameters = NWParameters.udp
        parameters.serviceClass = .interactiveVoice
        let connection = NWConnection(host: host, port: port, using: parameters)
        audio = connection
        let gen = generation
        connection.stateUpdateHandler = { [weak self] state in
            guard let self, gen == self.generation else { return }
            switch state {
            case .ready:
                self.sequence = 0
                self.streaming = true
                self.active.withLock { $0 = true }
                if self.muted {
                    self.sendControl(.mute(true))
                }
                self.startPinging()
                self.onEvent?(.connected(serverName: self.serverName, pcId: self.serverId))
            case .failed(let error):
                self.fail(Self.describe(error), canRetry: true)
            default:
                break
            }
        }
        connection.start(queue: queue)
    }

    // MARK: - Keepalive

    private func startPinging() {
        pingTimer?.cancel()
        let gen = generation
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 0.2, repeating: 1.0)
        timer.setEventHandler { [weak self] in
            guard let self, gen == self.generation else { return }
            if Self.now() - self.lastReceive > 5_000_000_000 {
                self.fail("Lost contact with the PC.", canRetry: true)
                return
            }
            let id = self.nextPingID
            self.nextPingID += 1
            self.pingSentAt[id] = Self.now()
            if self.pingSentAt.count > 16 {
                self.pingSentAt = self.pingSentAt.filter { $0.key > id - 8 }
            }
            self.sendControl(.ping(id: id, rtt: self.lastRoundTrip))
        }
        timer.resume()
        pingTimer = timer
    }

    // MARK: - Helpers

    private func sendControl(_ message: ControlMessage) {
        guard let control, let data = Self.encode(message) else { return }
        control.send(content: data, completion: .contentProcessed { _ in })
    }

    private func fail(_ reason: String, canRetry: Bool) {
        guard control != nil else { return }
        teardown()
        onEvent?(.disconnected(reason: reason, canRetry: canRetry))
    }

    private func teardown() {
        generation += 1 // makes callbacks from the old connections no-ops
        pingTimer?.cancel()
        pingTimer = nil
        control?.cancel()
        control = nil
        audio?.cancel()
        audio = nil
        rxBuffer.removeAll()
        streaming = false
        active.withLock { $0 = false }
        session = 0
        pingSentAt.removeAll()
        lastRoundTrip = 0
    }

    private static func encode(_ message: ControlMessage) -> Data? {
        guard var data = try? JSONEncoder().encode(message) else { return nil }
        data.append(0x0A)
        return data
    }

    private static func now() -> UInt64 {
        DispatchTime.now().uptimeNanoseconds
    }

    private static func describe(_ error: NWError) -> String {
        switch error {
        case .posix(.ECONNREFUSED):
            return "The PC refused the connection. Is Phomic open on it?"
        case .posix(.ETIMEDOUT):
            return "Timed out. Check that both devices are on the same Wi-Fi and that Windows Firewall allows Phomic."
        case .posix(.ENETUNREACH), .posix(.EHOSTUNREACH):
            return "The PC isn't reachable from this network."
        case .dns(let code) where code == -65570: // kDNSServiceErr_PolicyDenied
            return "Local network access is off. Turn it on in Settings › Privacy & Security › Local Network."
        case .dns:
            return "Couldn't find the PC on the network."
        default:
            return error.localizedDescription
        }
    }
}
