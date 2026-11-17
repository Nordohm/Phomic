import Foundation

/// Wire protocol shared with the Windows app (see the repo README, "Protocol").
///
/// Control: TCP, one UTF-8 JSON object per line.
/// Audio:   UDP datagrams, little-endian:
///            [0..3]   magic "PHMC"
///            [4..7]   session id (from the "welcome" message)
///            [8..11]  sequence number, +1 per packet, wraps
///            [12..]   480 x Int16 PCM, mono, 48 kHz (10 ms)
enum WireProtocol {
    static let version = 1
    static let sampleRate: Double = 48_000
    static let frameSamples = 480
    static let serviceType = "_phomic._tcp"
    static let defaultPort: UInt16 = 50505
    /// Where the iPhone waits for the PC over USB (loopback only, see UsbListener).
    static let usbPort: UInt16 = 50505
    static let magic: [UInt8] = Array("PHMC".utf8)
}

/// Every control message, in either direction. Unused fields are left out of the JSON.
struct ControlMessage: Codable, Sendable {
    var type: String
    var version: Int?
    var name: String?
    var sampleRate: Int?
    var channels: Int?
    var frameSamples: Int?
    var udpPort: Int?
    var session: UInt32?
    var id: Int?
    var rtt: Double?
    var muted: Bool?
    var message: String?
    /// "switch" requests: "wifi", "bluetooth" or "usb". In "welcome": "usb" over USB.
    var transport: String?
    /// The PC's id (8 hex digits), the same over Wi-Fi, Bluetooth and USB.
    var pcId: String?
    /// Pairing code, when the PC requires one.
    var code: String?
    /// Why an "error" happened, e.g. "pairing".
    var reason: String?

    init(type: String) {
        self.type = type
    }

    static func hello(name: String, code: String?) -> ControlMessage {
        var message = ControlMessage(type: "hello")
        message.code = code
        message.version = WireProtocol.version
        message.name = name
        message.sampleRate = Int(WireProtocol.sampleRate)
        message.channels = 1
        message.frameSamples = WireProtocol.frameSamples
        return message
    }

    static func ping(id: Int, rtt: Double) -> ControlMessage {
        var message = ControlMessage(type: "ping")
        message.id = id
        message.rtt = rtt
        return message
    }

    static func mute(_ muted: Bool) -> ControlMessage {
        var message = ControlMessage(type: "mute")
        message.muted = muted
        return message
    }
}
