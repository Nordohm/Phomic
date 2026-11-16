import AVFoundation
import Network
import Observation
import UIKit
import PhomicActivity

/// App state: finding PCs (Bonjour over Wi-Fi, announcements over Bluetooth), the
/// connection lifecycle on either transport (with automatic reconnect and live
/// hand-over between them), pairing codes, and the microphone.
///
/// While a session is active the app always runs *some* audio I/O, which is what lets
/// iOS keep it alive in the background (UIBackgroundModes: audio): the microphone when
/// live, or silent mixable playback while muted, so the mic is fully released then.
@MainActor
@Observable
final class MicController {
    enum Transport: String, CaseIterable, Identifiable {
        case wifi
        case bluetooth

        var id: String { rawValue }
        var title: String {
            switch self {
            case .wifi: "Wi-Fi"
            case .bluetooth: "Bluetooth"
            }
        }
    }

    enum Phase: Equatable {
        case idle
        case connecting(String)
        case streaming(String)
        case reconnecting(String)
    }

    /// A PC that wants its pairing code before it lets this iPhone in.
    struct PairingPrompt: Identifiable {
        enum Retry {
            case wifi(endpoint: NWEndpoint, name: String, pcId: String?)
            case bluetooth(NearbyPC)
        }

        let id = UUID()
        let pcId: String?
        let pcName: String
        /// The PC turned down the code we had saved or just typed.
        let wrongCode: Bool
        let retry: Retry
    }

    /// A PC whose pairing code this iPhone remembers (Settings lists them).
    struct SavedPairing: Identifiable, Hashable {
        let id: String      // storage key: the PC's id, or "name:<name>" for manual connections
        let name: String
    }

    private(set) var phase: Phase = .idle {
        didSet {
            if (oldValue == .idle) != (phase == .idle) {
                updateScanning()
            }
            refreshLiveActivity()
        }
    }
    private(set) var transport: Transport?
    private(set) var servers: [DiscoveredServer] = []
    private(set) var nearbyPCs: [NearbyPC] = []
    private(set) var browseProblem: String?
    private(set) var bluetoothProblem: String?
    private(set) var roundTripMs: Double?
    private(set) var bluetoothSampleRate: Int?
    /// Meter position 0...1 (-60 dBFS ... 0 dBFS).
    private(set) var level: Float = 0
    /// The iPhone's Mic Mode (Standard / Voice Isolation / Wide Spectrum) while recording.
    private(set) var systemMicMode: String?
    var pairingPrompt: PairingPrompt?
    private(set) var savedPairings: [SavedPairing] = []
    var lastError: String?

    var isMuted = false {
        didSet {
            guard isMuted != oldValue else { return }
            client.setMuted(isMuted)
            bluetooth.setMuted(isMuted)
            if isActive {
                applyAudioState()
            }
            refreshLiveActivity()
        }
    }

    /// Wi-Fi or Bluetooth, chosen with the switch at the top. Changing it while live
    /// hands the session over to the other transport (the microphone keeps running).
    var preferredTransport: Transport {
        didSet {
            UserDefaults.standard.set(preferredTransport.rawValue, forKey: Keys.transport)
            if isActive, transport != preferredTransport {
                switchTransport(to: preferredTransport)
            }
            updateScanning()
        }
    }

    /// The Lock Screen / Dynamic Island activity while connected (Settings).
    var liveActivityEnabled: Bool {
        didSet {
            UserDefaults.standard.set(liveActivityEnabled, forKey: Keys.liveActivity)
            refreshLiveActivity()
        }
    }

    var manualHost: String {
        didSet { UserDefaults.standard.set(manualHost, forKey: Keys.manualHost) }
    }

    var manualPort: String {
        didSet { UserDefaults.standard.set(manualPort, forKey: Keys.manualPort) }
    }

    var isActive: Bool { phase != .idle }

    var canShowMicModes: Bool {
        capture.isCapturing
    }

    private let client: StreamClient
    private let bluetooth: BluetoothLink
    private let capture = AudioCapture()
    private let browser = ServerBrowser()
    private let scanner = BluetoothScanner()
    @ObservationIgnored private var target: (endpoint: NWEndpoint, name: String)?
    /// Last PC reached over Wi-Fi, and the PC we're talking to on either transport: used
    /// to find the same PC again when switching transports.
    @ObservationIgnored private var lastWiFi: (endpoint: NWEndpoint, name: String)?
    @ObservationIgnored private var currentPC: (id: String?, name: String)?
    @ObservationIgnored private var retryTask: Task<Void, Never>?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private let liveActivity = LiveActivityController()

    private enum Keys {
        static let manualHost = "manualHost"
        static let manualPort = "manualPort"
        static let transport = "transport"
        static let pairingCodes = "pairingCodes"
        static let pairingNames = "pairingNames"
        static let liveActivity = "liveActivity"
    }

    init() {
        let defaults = UserDefaults.standard
        preferredTransport = Transport(rawValue: defaults.string(forKey: Keys.transport) ?? "") ?? .wifi
        liveActivityEnabled = defaults.object(forKey: Keys.liveActivity) as? Bool ?? true
        manualHost = defaults.string(forKey: Keys.manualHost) ?? ""
        manualPort = defaults.string(forKey: Keys.manualPort) ?? String(WireProtocol.defaultPort)
        let name = UIDevice.current.name
        client = StreamClient(deviceName: name)
        bluetooth = BluetoothLink(deviceName: name)

        client.onEvent = { [weak self] event in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.handle(event) }
            }
        }
        bluetooth.onEvent = { [weak self] event in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.handle(event) }
            }
        }
        let client = self.client
        let bluetooth = self.bluetooth
        capture.onFrame = { frame in
            client.send(frame: frame)
            bluetooth.send(frame: frame)
        }
        capture.onLevel = { [weak self] peak in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.updateLevel(peak) }
            }
        }
        browser.onChange = { [weak self] servers in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.servers = servers }
            }
        }
        browser.onProblem = { [weak self] problem in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.browseProblem = problem }
            }
        }
        scanner.onChange = { [weak self] pcs in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.nearbyPCs = pcs }
            }
        }
        scanner.onProblem = { [weak self] problem in
            DispatchQueue.main.async { [weak self] in
                MainActor.assumeIsolated { self?.bluetoothProblem = problem }
            }
        }

        reloadSavedPairings()
        // The Live Activity's mute button (ToggleMuteIntent) runs here, in the app.
        ToggleMuteIntent.handler = { [weak self] in
            guard let self, self.isActive else { return }
            self.isMuted.toggle()
        }
        observeAudioSession()
        browser.start()
        updateScanning()
    }

    // MARK: - User actions

    func connect(to server: DiscoveredServer) {
        if server.requiresCode, storedCode(pcId: server.pcId, name: server.name) == nil {
            pairingPrompt = PairingPrompt(pcId: server.pcId, pcName: server.name, wrongCode: false,
                                          retry: .wifi(endpoint: server.endpoint, name: server.name, pcId: server.pcId))
            return
        }
        beginWiFi(endpoint: server.endpoint, name: server.name, pcId: server.pcId)
    }

    func connectManually() {
        let host = manualHost.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !host.isEmpty,
              let portNumber = UInt16(manualPort.trimmingCharacters(in: .whitespaces)),
              let port = NWEndpoint.Port(rawValue: portNumber) else {
            lastError = "Enter the PC's address and port."
            return
        }
        beginWiFi(endpoint: .hostPort(host: NWEndpoint.Host(host), port: port), name: host, pcId: nil)
    }

    func connect(to pc: NearbyPC) {
        if pc.requiresCode, storedCode(pcId: pc.id, name: pc.name) == nil {
            pairingPrompt = PairingPrompt(pcId: pc.id, pcName: pc.name, wrongCode: false, retry: .bluetooth(pc))
            return
        }
        beginBluetooth(pc)
    }

    func submitPairingCode(_ code: String) {
        guard let prompt = pairingPrompt else { return }
        pairingPrompt = nil
        let trimmed = code.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        storeCode(trimmed, pcId: prompt.pcId, name: prompt.pcName)
        switch prompt.retry {
        case let .wifi(endpoint, name, pcId):
            beginWiFi(endpoint: endpoint, name: name, pcId: pcId)
        case let .bluetooth(pc):
            beginBluetooth(pc)
        }
    }

    func cancelPairing() {
        pairingPrompt = nil
    }

    func disconnect() {
        stopAll()
    }

    func showSystemMicModes() {
        AVCaptureDevice.showSystemUserInterface(.microphoneModes)
    }

    // MARK: - Lifecycle

    private func beginWiFi(endpoint: NWEndpoint, name: String, pcId: String?) {
        retryTask?.cancel()
        lastError = nil
        preferredTransport = .wifi
        transport = .wifi
        target = (endpoint, name)
        currentPC = (pcId, name)
        phase = .connecting(name)
        Task {
            guard await prepareAudio(for: .wifi) else { return }
            guard let target, target.name == name else { return }
            client.setMuted(isMuted)
            client.connect(to: target.endpoint, code: storedCode(pcId: pcId, name: name))
        }
    }

    private func beginBluetooth(_ pc: NearbyPC) {
        retryTask?.cancel()
        lastError = nil
        preferredTransport = .bluetooth
        transport = .bluetooth
        target = nil
        currentPC = (pc.id, pc.name)
        phase = .connecting(pc.name)
        Task {
            guard await prepareAudio(for: .bluetooth) else { return }
            bluetooth.setMuted(isMuted)
            bluetooth.start(target: pc.id, name: pc.name, hint: pc.peripheralID,
                            code: storedCode(pcId: pc.id, name: pc.name))
        }
    }

    /// Asks for microphone access and starts the right audio I/O. False if it failed or
    /// the user switched away meanwhile.
    private func prepareAudio(for transport: Transport) async -> Bool {
        guard await Self.microphoneAccessGranted() else {
            stopAll()
            lastError = "Phomic needs microphone access. Turn it on in Settings › Privacy & Security › Microphone."
            return false
        }
        guard self.transport == transport else { return false }
        applyAudioState()
        return self.transport == transport
    }

    /// Microphone when live, silent keep-alive when muted.
    private func applyAudioState(restart: Bool = false) {
        let unmuting = !isMuted && capture.state == .keepingAlive
        do {
            if isMuted {
                if capture.state != .keepingAlive || restart {
                    try capture.startKeepAlive()
                }
            } else if !capture.isCapturing || restart {
                try capture.startCapture(fromBackground: UIApplication.shared.applicationState != .active)
            }
        } catch {
            let code = (error as NSError).code
            if unmuting {
                // e.g. unmuting from the Live Activity, and iOS said no: stay connected, muted.
                lastError = "Couldn't turn the microphone on (error \(code)). Unmute here instead."
                isMuted = true // didSet restarts the silent keep-alive
                return
            }
            let message = "Couldn't start the microphone: \(error.localizedDescription) (error \(code))"
            stopAll()
            lastError = message
        }
        if !capture.isCapturing {
            level = 0
            systemMicMode = nil
        }
    }

    private func handle(_ event: StreamClient.Event) {
        guard transport == .wifi else { return }
        switch event {
        case .connecting:
            break
        case let .connected(serverName, pcId):
            phase = .streaming(serverName)
            lastError = nil
            lastWiFi = target
            currentPC = (pcId ?? currentPC?.id, serverName)
        case .roundTrip(let milliseconds):
            roundTripMs = milliseconds
        case .switchRequested(let requested):
            // The PC's "Switch here" button.
            if let next = Transport(rawValue: requested) {
                preferredTransport = next
            }
        case let .pairingRequired(pcId, pcName, _):
            let attempted = target
            let id = pcId ?? currentPC?.id
            let name = pcName ?? attempted?.name ?? "the PC"
            let hadCode = storedCode(pcId: id, name: name) != nil
            forgetCode(pcId: id, name: name)
            stopAll()
            if let attempted {
                pairingPrompt = PairingPrompt(pcId: id, pcName: name, wrongCode: hadCode,
                                              retry: .wifi(endpoint: attempted.endpoint, name: attempted.name, pcId: id))
            }
        case .disconnected(let reason, let canRetry):
            roundTripMs = nil
            if let target, canRetry, reason != nil {
                // Unexpected drop (Wi-Fi blip, PC asleep...): keep audio running and retry.
                phase = .reconnecting(target.name)
                lastError = reason
                scheduleRetry()
            } else if target != nil || reason != nil {
                stopAll()
                lastError = reason
            }
        }
    }

    private func handle(_ event: BluetoothLink.Event) {
        guard transport == .bluetooth else { return }
        switch event {
        case .connecting:
            if !isStreaming { phase = .connecting(currentPC?.name ?? "your PC") }
        case .switchRequested(let requested):
            if let next = Transport(rawValue: requested) {
                preferredTransport = next
            }
        case let .connected(pcName, sampleRate):
            currentPC = (currentPC?.id, pcName)
            phase = .streaming(pcName)
            bluetoothSampleRate = sampleRate > 0 ? sampleRate : nil
            lastError = nil
        case let .disconnected(reason, canRetry):
            bluetoothSampleRate = nil
            if canRetry {
                // Phomic reconnects by itself (also in the background).
                phase = .reconnecting(currentPC?.name ?? "your PC")
            } else {
                stopAll()
                lastError = reason
            }
        case .problem(let message):
            lastError = message
        case .pairingRejected:
            let pc = NearbyPC(id: currentPC?.id ?? "", name: currentPC?.name ?? "the PC", requiresCode: true)
            let hadCode = storedCode(pcId: pc.id, name: pc.name) != nil
            forgetCode(pcId: pc.id, name: pc.name)
            stopAll()
            pairingPrompt = PairingPrompt(pcId: pc.id, pcName: pc.name, wrongCode: hadCode, retry: .bluetooth(pc))
        }
    }

    private var isStreaming: Bool {
        if case .streaming = phase { return true }
        return false
    }

    /// Hands a live session over to the other transport without stopping the microphone.
    private func switchTransport(to next: Transport) {
        retryTask?.cancel()
        lastError = nil
        roundTripMs = nil
        bluetoothSampleRate = nil
        switch next {
        case .bluetooth:
            guard let pc = currentPC, let id = pc.id else {
                stopAll()
                lastError = "Choose your PC below to connect over Bluetooth."
                return
            }
            client.disconnect() // says "bye", so the PC frees up straight away
            target = nil
            transport = .bluetooth
            phase = .connecting(pc.name)
            bluetooth.setMuted(isMuted)
            bluetooth.start(target: id, name: pc.name, hint: nearbyPCs.first { $0.id == id }?.peripheralID,
                            code: storedCode(pcId: id, name: pc.name))
        case .wifi:
            let destination = wifiDestination()
            bluetooth.stop()
            guard let destination else {
                stopAll()
                lastError = "Choose your PC below to connect over Wi-Fi."
                return
            }
            transport = .wifi
            target = (destination.endpoint, destination.name)
            phase = .connecting(destination.name)
            client.setMuted(isMuted)
            client.connect(to: destination.endpoint, code: storedCode(pcId: destination.pcId, name: destination.name))
        }
    }

    /// The same PC over Wi-Fi: by id or name with Bonjour, else the last Wi-Fi PC, else
    /// the only PC on the network.
    private func wifiDestination() -> (endpoint: NWEndpoint, name: String, pcId: String?)? {
        if let pc = currentPC {
            let match = servers.first { server in
                if let id = pc.id, let serverId = server.pcId { return id.caseInsensitiveCompare(serverId) == .orderedSame }
                return server.name.caseInsensitiveCompare(pc.name) == .orderedSame
            }
            if let match {
                return (match.endpoint, match.name, match.pcId)
            }
        }
        if let lastWiFi {
            return (lastWiFi.endpoint, lastWiFi.name, currentPC?.id)
        }
        if servers.count == 1, let only = servers.first {
            return (only.endpoint, only.name, only.pcId)
        }
        return nil
    }

    private func scheduleRetry() {
        retryTask?.cancel()
        retryTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2))
            guard !Task.isCancelled, let self, self.transport == .wifi, let target = self.target else { return }
            self.client.connect(to: target.endpoint, code: self.storedCode(pcId: self.currentPC?.id, name: target.name))
        }
    }

    private func stopAll() {
        retryTask?.cancel()
        retryTask = nil
        target = nil
        transport = nil
        client.disconnect()  // both are no-ops when already idle
        bluetooth.stop()
        capture.stop()
        phase = .idle
        level = 0
        roundTripMs = nil
        bluetoothSampleRate = nil
        systemMicMode = nil
    }

    /// Scan for PCs over Bluetooth only while that list is on screen.
    private func updateScanning() {
        if preferredTransport == .bluetooth && !isActive {
            scanner.start()
        } else {
            scanner.stop()
            nearbyPCs = []
        }
    }

    // MARK: - Pairing codes (remembered per PC)

    private func codeKey(pcId: String?, name: String) -> String {
        if let pcId, !pcId.isEmpty { return pcId.uppercased() }
        return "name:" + name.lowercased()
    }

    private func storedCode(pcId: String?, name: String) -> String? {
        let codes = UserDefaults.standard.dictionary(forKey: Keys.pairingCodes) as? [String: String] ?? [:]
        return codes[codeKey(pcId: pcId, name: name)]
    }

    private func storeCode(_ code: String, pcId: String?, name: String) {
        let key = codeKey(pcId: pcId, name: name)
        let defaults = UserDefaults.standard
        var codes = defaults.dictionary(forKey: Keys.pairingCodes) as? [String: String] ?? [:]
        var names = defaults.dictionary(forKey: Keys.pairingNames) as? [String: String] ?? [:]
        codes[key] = code
        names[key] = name
        defaults.set(codes, forKey: Keys.pairingCodes)
        defaults.set(names, forKey: Keys.pairingNames)
        reloadSavedPairings()
    }

    private func forgetCode(pcId: String?, name: String) {
        forgetPairing(codeKey(pcId: pcId, name: name))
    }

    func forgetPairing(_ key: String) {
        let defaults = UserDefaults.standard
        var codes = defaults.dictionary(forKey: Keys.pairingCodes) as? [String: String] ?? [:]
        var names = defaults.dictionary(forKey: Keys.pairingNames) as? [String: String] ?? [:]
        codes[key] = nil
        names[key] = nil
        defaults.set(codes, forKey: Keys.pairingCodes)
        defaults.set(names, forKey: Keys.pairingNames)
        reloadSavedPairings()
    }

    func forgetAllPairings() {
        UserDefaults.standard.removeObject(forKey: Keys.pairingCodes)
        UserDefaults.standard.removeObject(forKey: Keys.pairingNames)
        reloadSavedPairings()
    }

    private func reloadSavedPairings() {
        let defaults = UserDefaults.standard
        let codes = defaults.dictionary(forKey: Keys.pairingCodes) as? [String: String] ?? [:]
        let names = defaults.dictionary(forKey: Keys.pairingNames) as? [String: String] ?? [:]
        savedPairings = codes.keys.map { key in
            // Codes saved before names were kept: fall back to what the key says.
            let fallback = key.hasPrefix("name:") ? String(key.dropFirst(5)) : "PC \(key)"
            return SavedPairing(id: key, name: names[key] ?? fallback)
        }
        .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    // MARK: - Level and Mic Mode

    private func updateLevel(_ peak: Float) {
        guard isActive, capture.isCapturing else { return }
        let db = peak > 0.000_01 ? 20 * log10(min(peak, 1)) : -100
        level = max(0, min(1, (db + 60) / 60))
        let mode = Self.describe(AVCaptureDevice.activeMicrophoneMode)
        if mode != systemMicMode {
            systemMicMode = mode
        }
    }

    private static func describe(_ mode: AVCaptureDevice.MicrophoneMode) -> String {
        switch mode {
        case .standard: "Standard"
        case .voiceIsolation: "Voice Isolation"
        case .wideSpectrum: "Wide Spectrum"
        @unknown default: "Other"
        }
    }

    // MARK: - Live Activity

    /// Lock screen / Dynamic Island while connected: live, muted or reconnecting.
    private func refreshLiveActivity() {
        guard liveActivityEnabled else {
            liveActivity.show(nil)
            return
        }
        let bluetooth = transport == .bluetooth
        switch phase {
        case .idle:
            liveActivity.show(nil)
        case .connecting(let name), .reconnecting(let name):
            liveActivity.show(.init(status: .reconnecting, pcName: name, bluetooth: bluetooth))
        case .streaming(let name):
            liveActivity.show(.init(status: isMuted ? .muted : .live, pcName: name, bluetooth: bluetooth))
        }
    }

    // MARK: - Audio session events

    private func observeAudioSession() {
        let center = NotificationCenter.default
        // A Live Activity can only be started from the foreground: retry when we're back.
        observers.append(center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refreshLiveActivity()
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            let rawType = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            MainActor.assumeIsolated {
                // A phone call or Siri took the audio hardware; take it back afterwards.
                guard let self, self.isActive, let rawType,
                      AVAudioSession.InterruptionType(rawValue: rawType) == .ended else { return }
                self.applyAudioState(restart: true)
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                // A new route (e.g. a headset) can change the hardware sample rate.
                guard let self, self.isActive, self.capture.isCapturing,
                      Date().timeIntervalSince(self.capture.startedAt) > 1,
                      AVAudioSession.sharedInstance().sampleRate != self.capture.hardwareSampleRate else { return }
                self.applyAudioState(restart: true)
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.isActive else { return }
                self.applyAudioState(restart: true)
            }
        })
    }

    private static func microphoneAccessGranted() async -> Bool {
        switch AVAudioApplication.shared.recordPermission {
        case .granted:
            return true
        case .denied:
            return false
        case .undetermined:
            return await AVAudioApplication.requestRecordPermission()
        @unknown default:
            return false
        }
    }
}
