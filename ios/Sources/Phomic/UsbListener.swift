import Foundation
import Network

/// Waits for Phomic on a PC to reach this iPhone through a USB cable.
///
/// Windows reaches an iPhone over USB through Apple's USB service (usbmux, part of the
/// Apple Devices app), which hands the PC's connection to a port on the iPhone's loopback
/// interface. So the listener only accepts connections from inside the iPhone: nothing on
/// Wi-Fi can reach it, and it needs no Local Network permission.
///
/// The PC keeps trying while the cable is in, so the connection arrives by itself once
/// both are ready. A new connection replaces the old one (a cable plugged in again).
final class UsbListener: @unchecked Sendable {
    enum Event: Sendable {
        case connection(NWConnection)
        case failed(String)
    }

    var onEvent: (@Sendable (Event) -> Void)?

    private let queue = DispatchQueue(label: "phomic.usb", qos: .userInteractive)
    private var listener: NWListener?
    private var generation = 0

    func start() {
        queue.async {
            guard self.listener == nil else { return }
            self.open()
        }
    }

    func stop() {
        queue.async {
            self.generation += 1
            self.listener?.cancel()
            self.listener = nil
        }
    }

    private func open() {
        generation += 1
        let gen = generation

        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 2
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback),
                                                     port: NWEndpoint.Port(rawValue: WireProtocol.usbPort)!)
        parameters.allowLocalEndpointReuse = true
        parameters.serviceClass = .responsiveData

        let listener: NWListener
        do {
            listener = try NWListener(using: parameters)
        } catch {
            onEvent?(.failed("Couldn't wait for USB: \(error.localizedDescription)"))
            return
        }
        self.listener = listener
        listener.newConnectionHandler = { [weak self] connection in
            guard let self, gen == self.generation else {
                connection.cancel()
                return
            }
            self.onEvent?(.connection(connection))
        }
        listener.stateUpdateHandler = { [weak self] state in
            guard let self, gen == self.generation else { return }
            switch state {
            case .failed(let error):
                self.listener?.cancel()
                self.listener = nil
                if case .posix(.EADDRINUSE) = error {
                    // Just stopped and started again: the old socket is still closing.
                    self.queue.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                        guard let self, gen == self.generation, self.listener == nil else { return }
                        self.open()
                    }
                } else {
                    self.onEvent?(.failed("Couldn't wait for USB: \(error.localizedDescription)"))
                }
            default:
                break
            }
        }
        listener.start(queue: queue)
    }
}
