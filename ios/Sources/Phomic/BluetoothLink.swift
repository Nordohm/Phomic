import CoreBluetooth
import Foundation
import os

/// The iPhone side of the Bluetooth LE link. The PC is the GATT server (peripheral) and
/// the iPhone connects to it, the way it connects to headphones or a watch:
///
/// 1. Find the PC: its service UUID carries the PC id, so we look for exactly the PC the
///    user picked (and try the peripheral the list found it on right away).
/// 2. Subscribe to the control characteristic and write "hello"; the PC answers "welcome"
///    (or a pairing error) as a notification.
/// 3. Write 10 ms Opus packets to the audio characteristic (write without response).
///
/// After an unexpected drop iOS keeps a pending connection, which completes as soon as
/// the PC is reachable again, even with Phomic in the background.
///
/// All state lives on `queue` (also the CoreBluetooth delegate queue).
final class BluetoothLink: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, @unchecked Sendable {
    /// Each step of finding and talking to the PC, for the device log (Console.app, or
    /// `idevicesyslog -m Phomic`): a Bluetooth link that won't come up is otherwise a black box.
    private static let log = Logger(subsystem: "io.github.nordohm.Phomic", category: "Bluetooth")

    private static func short(_ id: UUID) -> String { String(id.uuidString.prefix(8)) }

    enum Event: Sendable {
        /// Looking for the PC or connecting to it.
        case connecting
        case connected(pcName: String, sampleRate: Int)
        /// `canRetry`: the link dropped unexpectedly and Phomic is reconnecting by itself.
        case disconnected(reason: String?, canRetry: Bool)
        case problem(String)
        /// The PC asks the iPhone to move this session to another transport ("wifi").
        case switchRequested(transport: String)
        /// The PC wants a (different) pairing code before it lets this iPhone in.
        case pairingRejected
    }

    static var audioUUID: CBUUID { CBUUID(string: "8E5A1C51-3B7D-4F62-9A1E-6C2D4B8F0A11") }
    static var controlUUID: CBUUID { CBUUID(string: "8E5A1C52-3B7D-4F62-9A1E-6C2D4B8F0A11") }

    /// Each PC advertises its Phomic service as 8E5A1C6<c>-3B7D-4F62-9A1E-6C2D<PC id>,
    /// where c = 1 if it wants a pairing code. Both variants of the PC with this id:
    static func serviceUUIDs(forPC id: String) -> [CBUUID] {
        guard id.count == 8, id.allSatisfy(\.isHexDigit) else { return [] }
        let suffix = "-3B7D-4F62-9A1E-6C2D" + id.uppercased()
        return [CBUUID(string: "8E5A1C60" + suffix), CBUUID(string: "8E5A1C61" + suffix)]
    }

    /// The PC id and pairing flag in an advertised service UUID, if it's a Phomic PC.
    static func parse(_ uuid: CBUUID) -> (id: String, requiresCode: Bool)? {
        let text = uuid.uuidString.uppercased()
        guard text.count == 36, text.hasPrefix("8E5A1C6"), text.dropFirst(8).hasPrefix("-3B7D-4F62-9A1E-6C2D") else { return nil }
        let flag = text[text.index(text.startIndex, offsetBy: 7)]
        guard flag == "0" || flag == "1" else { return nil }
        let id = String(text.suffix(8))
        guard id.allSatisfy(\.isHexDigit) else { return nil }
        return (id, flag == "1")
    }

    var onEvent: (@Sendable (Event) -> Void)?

    private let queue = DispatchQueue(label: "phomic.bluetooth", qos: .userInteractive)
    private let deviceName: String
    private let active = OSAllocatedUnfairLock(initialState: false)

    private var central: CBCentralManager?
    private var wanted = false
    /// Bumped by start/stop, so delayed work from an earlier session does nothing.
    private var session = 0
    private var targetId = ""
    private var serviceUUIDs: [CBUUID] = []
    private var hint: UUID?
    private var code: String?
    private var pcName = "PC"
    private var muted = false

    /// The PC's peripheral once it has shown our service (connected or reconnecting).
    private var peripheral: CBPeripheral?
    /// Pending connections that may turn out to be the PC.
    private var candidates: [UUID: CBPeripheral] = [:]
    private var audio: CBCharacteristic?
    private var control: CBCharacteristic?
    private var welcomed = false
    /// Bumped for each connection handshake, so its watchdog leaves later ones alone.
    private var handshake = 0
    /// PC id → its peripheral, for a direct reconnect when switching back to Bluetooth.
    private var knownPeripherals: [String: UUID] = [:]

    private var encoder = BluetoothAudioEncoder()
    private var currentRate = 0
    private var pendingAudio: [Data] = []

    init(deviceName: String) {
        self.deviceName = deviceName
        super.init()
    }

    // MARK: - Public API

    /// Connect to the PC with this id. `hint`: the peripheral its announcement came from.
    func start(target: String, name: String, hint: UUID?, code: String?) {
        queue.async {
            Self.log.notice("start: PC \(target, privacy: .public), hint \(hint.map(Self.short) ?? "none", privacy: .public)")
            self.hangUp()
            self.session += 1
            self.wanted = true
            self.targetId = target
            self.serviceUUIDs = Self.serviceUUIDs(forPC: target)
            self.pcName = name
            self.hint = hint
            self.code = code
            if let central = self.central {
                self.connectIfPossible(central)
            } else {
                // Creating the manager triggers the Bluetooth permission prompt.
                self.central = CBCentralManager(delegate: self, queue: self.queue)
            }
        }
    }

    func stop() {
        queue.async {
            guard self.wanted else { return }
            Self.log.notice("stop")
            self.wanted = false
            self.session += 1
            let session = self.session
            self.active.withLock { $0 = false }
            self.central?.stopScan()
            if self.welcomed {
                self.writeControl(["type": "bye"])
            }
            self.welcomed = false
            self.pendingAudio.removeAll()
            // Give "bye" a moment to go out, then hang up.
            self.queue.asyncAfter(deadline: .now() + 0.3) {
                guard self.session == session else { return }
                self.hangUp()
            }
        }
    }

    func setMuted(_ muted: Bool) {
        queue.async {
            self.muted = muted
            self.pendingAudio.removeAll()
            if self.welcomed {
                self.writeControl(["type": "state", "name": self.deviceName, "muted": muted])
            }
        }
    }

    /// `frame`: 480 samples at 48 kHz.
    func send(frame: [Float]) {
        guard active.withLock({ $0 }) else { return }
        queue.async {
            guard self.welcomed, !self.muted, let peripheral = self.peripheral else { return }
            let maximumLength = peripheral.maximumWriteValueLength(for: .withoutResponse)
            guard let encoded = self.encoder.encode(frame, maximumLength: maximumLength) else { return }
            if encoded.sampleRate != self.currentRate {
                self.currentRate = encoded.sampleRate
                self.onEvent?(.connected(pcName: self.pcName, sampleRate: encoded.sampleRate))
            }
            self.pendingAudio.append(encoded.packet)
            if self.pendingAudio.count > 12 {
                // More than ~120 ms behind: drop the oldest audio rather than build latency.
                self.pendingAudio.removeFirst(self.pendingAudio.count - 12)
            }
            self.flushAudio()
        }
    }

    // MARK: - Connecting

    private func connectIfPossible(_ central: CBCentralManager) {
        guard wanted, central.state == .poweredOn else { return }
        guard !serviceUUIDs.isEmpty else {
            onEvent?(.problem("This PC can't be reached over Bluetooth. Update Phomic on the PC."))
            return
        }
        onEvent?(.connecting)
        // Fast path: a PC we've talked to before, or the one whose announcement we tapped.
        var ids: [UUID] = []
        if let known = knownPeripherals[targetId] { ids.append(known) }
        if let hint, !ids.contains(hint) { ids.append(hint) }
        Self.log.notice("connecting: \(ids.count) known peripheral(s), scanning for the PC's service")
        for candidate in central.retrievePeripherals(withIdentifiers: ids) {
            connect(candidate)
        }
        // And look for the PC's advertisement, which carries its service UUID.
        central.scanForPeripherals(withServices: serviceUUIDs, options: nil)
    }

    private func connect(_ candidate: CBPeripheral) {
        guard let central, wanted,
              candidates[candidate.identifier] == nil,
              peripheral?.identifier != candidate.identifier else { return }
        candidates[candidate.identifier] = candidate
        candidate.delegate = self
        Self.log.notice("connect to \(Self.short(candidate.identifier), privacy: .public)")
        central.connect(candidate, options: nil)
    }

    /// This peripheral is the PC: keep it, drop the other candidates.
    private func adopt(_ found: CBPeripheral) {
        if let old = peripheral, old.identifier != found.identifier {
            central?.cancelPeripheralConnection(old)
        }
        peripheral = found
        candidates[found.identifier] = nil
        for other in candidates.values {
            central?.cancelPeripheralConnection(other)
        }
        candidates.removeAll()
        central?.stopScan()
    }

    /// Closes every connection of this session (no events).
    private func hangUp() {
        active.withLock { $0 = false }
        central?.stopScan()
        for candidate in candidates.values {
            central?.cancelPeripheralConnection(candidate)
        }
        candidates.removeAll()
        if let peripheral {
            central?.cancelPeripheralConnection(peripheral)
        }
        peripheral = nil
        audio = nil
        control = nil
        welcomed = false
        pendingAudio.removeAll()
    }

    /// The PC ended the session (bye, pairing refused): hang up and don't come back.
    private func finish() {
        wanted = false
        session += 1
        hangUp()
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Self.log.notice("Bluetooth state \(central.state.rawValue)")
        switch central.state {
        case .poweredOn:
            connectIfPossible(central)
        case .poweredOff:
            let wasConnected = peripheral != nil
            candidates.removeAll()
            peripheral = nil
            audio = nil
            control = nil
            welcomed = false
            active.withLock { $0 = false }
            if wanted {
                if wasConnected {
                    onEvent?(.disconnected(reason: nil, canRetry: true))
                }
                onEvent?(.problem("Bluetooth is turned off. Turn it on in Control Center or Settings."))
            }
        case .unauthorized:
            if wanted { onEvent?(.problem("Phomic isn't allowed to use Bluetooth. Turn it on in Settings › Privacy & Security › Bluetooth.")) }
        case .unsupported:
            if wanted { onEvent?(.problem("This device doesn't support Bluetooth LE.")) }
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover found: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        Self.log.notice("found \(Self.short(found.identifier), privacy: .public), RSSI \(RSSI.intValue)")
        connect(found)
    }

    func centralManager(_ central: CBCentralManager, didConnect connected: CBPeripheral) {
        Self.log.notice("connected to \(Self.short(connected.identifier), privacy: .public)")
        guard wanted, !serviceUUIDs.isEmpty else {
            central.cancelPeripheralConnection(connected)
            return
        }
        connected.delegate = self
        connected.discoverServices(serviceUUIDs)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect failed: CBPeripheral, error: Error?) {
        Self.log.notice("failed to connect to \(Self.short(failed.identifier), privacy: .public): \(error?.localizedDescription ?? "-", privacy: .public)")
        candidates[failed.identifier] = nil
        guard wanted, failed.identifier == peripheral?.identifier else { return }
        // Try the PC again in a moment.
        let session = self.session
        queue.asyncAfter(deadline: .now() + 1) {
            guard self.session == session, self.wanted, let peripheral = self.peripheral else { return }
            self.central?.connect(peripheral, options: nil)
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral gone: CBPeripheral, error: Error?) {
        Self.log.notice("disconnected from \(Self.short(gone.identifier), privacy: .public) (the PC: \(gone.identifier == self.peripheral?.identifier), live: \(self.welcomed)): \(error?.localizedDescription ?? "-", privacy: .public)")
        if candidates.removeValue(forKey: gone.identifier) != nil {
            return
        }
        guard gone.identifier == peripheral?.identifier else { return }
        let wasLive = welcomed
        audio = nil
        control = nil
        welcomed = false
        active.withLock { $0 = false }
        pendingAudio.removeAll()
        guard wanted else {
            peripheral = nil
            return
        }
        // Unexpected: keep a pending connection (iOS completes it whenever the PC is back,
        // even in the background), and also look for the PC in case its address changed.
        central.connect(gone, options: nil)
        if !serviceUUIDs.isEmpty {
            central.scanForPeripherals(withServices: serviceUUIDs, options: nil)
        }
        onEvent?(wasLive ? .disconnected(reason: "The Bluetooth connection was lost.", canRetry: true) : .connecting)
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ device: CBPeripheral, didDiscoverServices error: Error?) {
        guard wanted, !serviceUUIDs.isEmpty else { return }
        guard let service = device.services?.first(where: { serviceUUIDs.contains($0.uuid) }) else {
            Self.log.notice("no Phomic service on \(Self.short(device.identifier), privacy: .public): \(error?.localizedDescription ?? "-", privacy: .public)")
            if candidates[device.identifier] != nil {
                // Not the PC we want (or Phomic isn't running there): let it go.
                candidates[device.identifier] = nil
                central?.cancelPeripheralConnection(device)
            } else if device.identifier == peripheral?.identifier {
                // Phomic stopped on the PC: wait until it advertises its service again.
                let wasLive = welcomed
                peripheral = nil
                audio = nil
                control = nil
                welcomed = false
                active.withLock { $0 = false }
                pendingAudio.removeAll()
                central?.cancelPeripheralConnection(device)
                central?.scanForPeripherals(withServices: serviceUUIDs, options: nil)
                onEvent?(wasLive ? .disconnected(reason: "Phomic stopped on the PC.", canRetry: true) : .connecting)
            }
            return
        }
        Self.log.notice("the PC is \(Self.short(device.identifier), privacy: .public)")
        adopt(device)
        startHandshakeWatchdog(for: device)
        device.discoverCharacteristics([Self.audioUUID, Self.controlUUID], for: service)
    }

    func peripheral(_ device: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        // The PC restarted Phomic (its service was rebuilt): find it again.
        guard wanted, device.identifier == peripheral?.identifier, !serviceUUIDs.isEmpty,
              invalidatedServices.contains(where: { serviceUUIDs.contains($0.uuid) }) else { return }
        audio = nil
        control = nil
        welcomed = false
        active.withLock { $0 = false }
        pendingAudio.removeAll()
        device.discoverServices(serviceUUIDs)
    }

    func peripheral(_ device: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard wanted, device.identifier == peripheral?.identifier else { return }
        audio = service.characteristics?.first { $0.uuid == Self.audioUUID }
        control = service.characteristics?.first { $0.uuid == Self.controlUUID }
        guard let control, audio != nil else {
            finish()
            onEvent?(.disconnected(reason: "Phomic on the PC is a different version. Update both apps.", canRetry: false))
            return
        }
        device.setNotifyValue(true, for: control)
    }

    func peripheral(_ device: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard wanted, device.identifier == peripheral?.identifier, characteristic.uuid == Self.controlUUID else { return }
        Self.log.notice("subscribed: \(characteristic.isNotifying), \(error?.localizedDescription ?? "-", privacy: .public)")
        if error != nil || !characteristic.isNotifying {
            central?.cancelPeripheralConnection(device) // reconnects by itself
            return
        }
        sendHello()
    }

    private func sendHello() {
        Self.log.notice("hello")
        var hello: [String: Any] = ["type": "hello", "v": 2, "name": deviceName, "muted": muted]
        if let code {
            hello["code"] = code
        }
        writeControl(hello)
    }

    /// A handshake that stalls (no "welcome" from the PC) shouldn't leave us on
    /// "Connecting" forever: say hello again after 2.5 s, and start over after 7 s.
    private func startHandshakeWatchdog(for device: CBPeripheral) {
        handshake += 1
        let handshake = self.handshake
        let id = device.identifier
        queue.asyncAfter(deadline: .now() + 2.5) {
            guard self.isStuck(handshake, id), self.control?.isNotifying == true else { return }
            Self.log.notice("no welcome after 2.5 s: hello again")
            self.sendHello()
        }
        queue.asyncAfter(deadline: .now() + 7) {
            guard self.isStuck(handshake, id), let peripheral = self.peripheral else { return }
            Self.log.notice("no welcome after 7 s: starting over")
            // didDisconnect keeps a pending connection and looks for the PC again.
            self.central?.cancelPeripheralConnection(peripheral)
        }
    }

    private func isStuck(_ handshake: Int, _ id: UUID) -> Bool {
        self.handshake == handshake && wanted && !welcomed && peripheral?.identifier == id
    }

    func peripheral(_ device: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard wanted, device.identifier == peripheral?.identifier, characteristic.uuid == Self.controlUUID,
              let value = characteristic.value,
              let message = try? JSONSerialization.jsonObject(with: value) as? [String: Any],
              let type = message["type"] as? String else { return }
        Self.log.notice("from the PC: \(type, privacy: .public)")
        switch type {
        case "welcome":
            pcName = (message["name"] as? String) ?? pcName
            welcomed = true
            knownPeripherals[targetId] = device.identifier
            encoder.reset()
            pendingAudio.removeAll()
            currentRate = encoder.sampleRate(forMaximumLength: device.maximumWriteValueLength(for: .withoutResponse))
            active.withLock { $0 = true }
            if currentRate == 0 {
                onEvent?(.problem("The Bluetooth link to the PC is too small for audio. Try Wi-Fi instead."))
            }
            onEvent?(.connected(pcName: pcName, sampleRate: currentRate))
        case "error":
            if (message["reason"] as? String) == "pairing" {
                finish()
                onEvent?(.pairingRejected)
            }
        case "bye":
            finish()
            onEvent?(.disconnected(reason: (message["reason"] as? String) ?? "The PC ended the connection.", canRetry: false))
        case "switch":
            if let transport = message["transport"] as? String {
                onEvent?(.switchRequested(transport: transport))
            }
        default:
            break
        }
    }

    func peripheralIsReady(toSendWriteWithoutResponse device: CBPeripheral) {
        flushAudio()
    }

    // MARK: - Sending

    private func flushAudio() {
        guard let peripheral, let audio else {
            pendingAudio.removeAll()
            return
        }
        while let packet = pendingAudio.first, peripheral.canSendWriteWithoutResponse {
            peripheral.writeValue(packet, for: audio, type: .withoutResponse)
            pendingAudio.removeFirst()
        }
    }

    private func writeControl(_ message: [String: Any]) {
        guard let peripheral, let control,
              let data = try? JSONSerialization.data(withJSONObject: message) else { return }
        peripheral.writeValue(data, for: control, type: .withResponse)
    }
}
