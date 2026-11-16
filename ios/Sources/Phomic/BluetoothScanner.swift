import CoreBluetooth
import Foundation

/// A PC running Phomic within Bluetooth range.
struct NearbyPC: Identifiable, Hashable, Sendable {
    /// The PC's id (8 hex digits); the same id the PC uses over Wi-Fi.
    let id: String
    let name: String
    let requiresCode: Bool
    /// The PC's peripheral, so the iPhone can start connecting to it straight away.
    var peripheralID: UUID? = nil
}

/// Lists PCs running Phomic within Bluetooth range, from the Phomic service each PC advertises:
/// its UUID carries the PC id and whether it wants a pairing code (see BluetoothLink), and
/// the advertisement (or scan response) carries the PC's Bluetooth name.
final class BluetoothScanner: NSObject, CBCentralManagerDelegate, @unchecked Sendable {
    var onChange: (@Sendable ([NearbyPC]) -> Void)?
    /// A user-facing problem (Bluetooth off, not allowed), or nil.
    var onProblem: (@Sendable (String?) -> Void)?

    private let queue = DispatchQueue(label: "phomic.bluetooth-scan")
    private var central: CBCentralManager?
    private var wanted = false
    private var seen: [String: (pc: NearbyPC, lastSeen: Date)] = [:]
    private var pruneTimer: DispatchSourceTimer?

    func start() {
        queue.async {
            self.wanted = true
            if self.central == nil {
                // Creating the manager triggers the Bluetooth permission prompt.
                self.central = CBCentralManager(delegate: self, queue: self.queue)
            } else {
                self.scanIfPossible()
            }
        }
    }

    func stop() {
        queue.async {
            guard self.wanted else { return }
            self.wanted = false
            self.central?.stopScan()
            self.pruneTimer?.cancel()
            self.pruneTimer = nil
            self.seen.removeAll()
            self.publish()
        }
    }

    private func scanIfPossible() {
        guard wanted, let central, central.state == .poweredOn else { return }
        central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        if pruneTimer == nil {
            // PCs that stop announcing (closed, out of range) disappear after a few seconds.
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + 1, repeating: 1)
            timer.setEventHandler { [weak self] in
                guard let self else { return }
                let before = self.seen.count
                self.seen = self.seen.filter { Date().timeIntervalSince($0.value.lastSeen) < 6 }
                if self.seen.count != before {
                    self.publish()
                }
            }
            timer.resume()
            pruneTimer = timer
        }
    }

    private func publish() {
        let pcs = seen.values.map(\.pc).sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        onChange?(pcs)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            onProblem?(nil)
            scanIfPossible()
        case .poweredOff:
            onProblem?("Bluetooth is turned off. Turn it on in Control Center or Settings.")
        case .unauthorized:
            onProblem?("Phomic isn't allowed to use Bluetooth. Turn it on in Settings › Privacy & Security › Bluetooth.")
        case .unsupported:
            onProblem?("This device doesn't support Bluetooth LE.")
        default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let uuids = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? [])
            + (advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey] as? [CBUUID] ?? [])
        guard let (id, requiresCode) = uuids.lazy.compactMap(BluetoothLink.parse).first else { return }
        // The name usually arrives in the scan response: keep the last one we saw.
        let advertised = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        let name = advertised.flatMap { $0.isEmpty ? nil : $0 } ?? seen[id]?.pc.name ?? "PC"
        let pc = NearbyPC(id: id, name: name, requiresCode: requiresCode, peripheralID: peripheral.identifier)
        let changed = seen[id]?.pc != pc
        seen[id] = (pc, Date())
        if changed {
            publish()
        }
    }
}
