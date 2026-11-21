import Foundation

/// Wire protocol shared with the PC app (see docs/protocols.md).
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

    /// One audio packet (layout above), the samples clamped to -1...1.
    static func audioPacket(session: UInt32, sequence: UInt32, frame: [Float]) -> Data {
        var packet = Data(count: 12 + 2 * frame.count)
        packet.withUnsafeMutableBytes { bytes in
            bytes.copyBytes(from: magic)
            bytes.storeBytes(of: session.littleEndian, toByteOffset: 4, as: UInt32.self)
            bytes.storeBytes(of: sequence.littleEndian, toByteOffset: 8, as: UInt32.self)
            for (index, sample) in frame.enumerated() {
                let value = Int16(max(-1, min(1, sample)) * 32767)
                bytes.storeBytes(of: value.littleEndian, toByteOffset: 12 + 2 * index, as: Int16.self)
            }
        }
        return packet
    }
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
