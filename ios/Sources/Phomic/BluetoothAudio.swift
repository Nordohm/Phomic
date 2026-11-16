import Accelerate
import COpus
import Foundation

/// Audio over Bluetooth LE. Each 10 ms frame becomes one notification:
///
///   [0]     format: 0xB1 = Opus (48 kHz mono, 10 ms, low-delay mode)
///   [1..2]  sequence number (UInt16, wraps)
///   [3...]  one Opus packet, sized to fit the link (64 kbit/s when it can)
///
/// If the Opus encoder can't be created, it falls back to the older IMA-ADPCM format
/// (0xA1 = 24 kHz, 0xA2 = 16 kHz), which the PC still understands.
final class BluetoothAudioEncoder {
    static let opusFormat: UInt8 = 0xB1
    static let targetBitrate = 64_000
    private static let complexity: Int32 = 5

    private var opus: OpaquePointer?
    private var opusBitrate = 0
    private var adpcm = AdpcmPacketEncoder()
    private var sequence: UInt16 = 0
    private var scratch = [UInt8](repeating: 0, count: 1275)

    init() {
        opus = Self.makeOpus()
    }

    deinit {
        if let opus {
            opus_encoder_destroy(opus)
        }
    }

    func reset() {
        if let opus {
            opus_encoder_destroy(opus)
        }
        opus = Self.makeOpus()
        opusBitrate = Self.targetBitrate
        adpcm = AdpcmPacketEncoder()
        sequence = 0
    }

    /// Sample rate the PC will get on a link with this notification size.
    func sampleRate(forMaximumLength length: Int) -> Int {
        if opus != nil { return length > 3 + 20 ? Int(WireProtocol.sampleRate) : 0 }
        return BluetoothFormat.best(forMaximumLength: length)?.sampleRate ?? 0
    }

    /// `frame`: 480 samples at 48 kHz. Nil if nothing useful fits in one notification.
    func encode(_ frame: [Float], maximumLength: Int) -> (packet: Data, sampleRate: Int)? {
        guard let opus else {
            guard let encoded = adpcm.encode(frame, maximumLength: maximumLength) else { return nil }
            return (encoded.packet, encoded.format.sampleRate)
        }
        let budget = min(maximumLength - 3, scratch.count)
        guard budget >= 20 else { return nil }

        // 100 packets a second: on small links, lower the bitrate so every packet fits.
        let bitrate = min(Self.targetBitrate, budget * 8 * 100 * 9 / 10)
        if bitrate != opusBitrate {
            phomic_opus_set_bitrate(opus, Int32(bitrate))
            opusBitrate = bitrate
        }

        let bytes = frame.withUnsafeBufferPointer { pcm in
            scratch.withUnsafeMutableBufferPointer { out in
                opus_encode_float(opus, pcm.baseAddress!, Int32(WireProtocol.frameSamples), out.baseAddress!, Int32(budget))
            }
        }
        guard bytes > 0 else { return nil }

        var packet = Data(capacity: 3 + Int(bytes))
        packet.append(Self.opusFormat)
        withUnsafeBytes(of: sequence.littleEndian) { packet.append(contentsOf: $0) }
        packet.append(contentsOf: scratch[0..<Int(bytes)])
        sequence &+= 1
        return (packet, Int(WireProtocol.sampleRate))
    }

    private static func makeOpus() -> OpaquePointer? {
        var error: Int32 = 0
        let encoder = phomic_opus_encoder_create(Int32(targetBitrate), complexity, &error)
        return error == OPUS_OK ? encoder : nil
    }
}

// MARK: - Fallback: IMA-ADPCM

/// [0] 0xA1 = 24 kHz, 0xA2 = 16 kHz · [1..2] sequence · [3..4] ADPCM predictor (Int16) ·
/// [5] step index · [6...] 4-bit codes, two per byte, low nibble first.
enum BluetoothFormat: UInt8 {
    case wideband24k = 0xA1
    case wideband16k = 0xA2

    var decimation: Int {
        switch self {
        case .wideband24k: 2
        case .wideband16k: 3
        }
    }

    var samplesPerPacket: Int { WireProtocol.frameSamples / decimation }
    var packetSize: Int { 6 + samplesPerPacket / 2 }
    var sampleRate: Int { Int(WireProtocol.sampleRate) / decimation }

    /// Highest quality that fits in one notification.
    static func best(forMaximumLength length: Int) -> BluetoothFormat? {
        if length >= BluetoothFormat.wideband24k.packetSize { return .wideband24k }
        if length >= BluetoothFormat.wideband16k.packetSize { return .wideband16k }
        return nil
    }
}

struct AdpcmPacketEncoder {
    private var decimator: Decimator?
    private var adpcm = ImaAdpcmEncoder()
    private var sequence: UInt16 = 0

    /// `frame`: 480 samples at 48 kHz. Returns nil if even the smallest packet doesn't fit.
    mutating func encode(_ frame: [Float], maximumLength: Int) -> (packet: Data, format: BluetoothFormat)? {
        guard let format = BluetoothFormat.best(forMaximumLength: maximumLength) else { return nil }
        if decimator?.factor != format.decimation {
            decimator = Decimator(factor: format.decimation)
        }
        let samples = decimator!.process(frame)

        var packet = Data(capacity: format.packetSize)
        packet.append(format.rawValue)
        withUnsafeBytes(of: sequence.littleEndian) { packet.append(contentsOf: $0) }
        withUnsafeBytes(of: Int16(adpcm.predictor).littleEndian) { packet.append(contentsOf: $0) }
        packet.append(UInt8(adpcm.index))

        var i = 0
        while i + 1 < samples.count {
            let low = adpcm.encode(Self.pcm(samples[i]))
            let high = adpcm.encode(Self.pcm(samples[i + 1]))
            packet.append(low | (high << 4))
            i += 2
        }
        sequence &+= 1
        return (packet, format)
    }

    private static func pcm(_ sample: Float) -> Int32 {
        Int32(max(-1, min(1, sample)) * 32767)
    }
}

/// Standard IMA/DVI ADPCM encoder.
struct ImaAdpcmEncoder {
    private(set) var predictor: Int32 = 0
    private(set) var index: Int32 = 0

    static let indexTable: [Int32] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
    static let stepTable: [Int32] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]

    mutating func encode(_ sample: Int32) -> UInt8 {
        var step = Self.stepTable[Int(index)]
        var diff = sample - predictor
        var code: Int32 = 0
        if diff < 0 {
            code = 8
            diff = -diff
        }
        var delta = step >> 3
        if diff >= step { code |= 4; diff -= step; delta += step }
        step >>= 1
        if diff >= step { code |= 2; diff -= step; delta += step }
        step >>= 1
        if diff >= step { code |= 1; delta += step }

        predictor += (code & 8) != 0 ? -delta : delta
        predictor = max(-32768, min(32767, predictor))
        index = max(0, min(88, index + Self.indexTable[Int(code)]))
        return UInt8(code)
    }
}

/// Windowed-sinc low-pass + decimation by an integer factor (Accelerate `vDSP_desamp`).
struct Decimator {
    let factor: Int
    private let taps: [Float]
    private var history: [Float]

    init(factor: Int) {
        self.factor = factor
        // Pass band ~9.6 kHz (24 kHz output) or ~6.5 kHz (16 kHz output), stop band below Nyquist.
        let cutoff = factor == 2 ? 0.2 : 0.135
        taps = Self.lowpass(length: 32 * factor + 1, cutoff: cutoff)
        history = [Float](repeating: 0, count: taps.count - 1)
    }

    mutating func process(_ input: [Float]) -> [Float] {
        let buffer = history + input
        let outputCount = input.count / factor
        var output = [Float](repeating: 0, count: outputCount)
        buffer.withUnsafeBufferPointer { source in
            taps.withUnsafeBufferPointer { filter in
                output.withUnsafeMutableBufferPointer { destination in
                    vDSP_desamp(source.baseAddress!, vDSP_Stride(factor), filter.baseAddress!,
                                destination.baseAddress!, vDSP_Length(outputCount), vDSP_Length(filter.count))
                }
            }
        }
        history = Array(buffer.suffix(taps.count - 1))
        return output
    }

    /// Blackman-windowed sinc; `cutoff` is a fraction of the input sample rate.
    private static func lowpass(length: Int, cutoff: Double) -> [Float] {
        let m = Double(length - 1)
        var taps = (0..<length).map { i -> Double in
            let x = Double(i) - m / 2
            let sinc = x == 0 ? 2 * cutoff : sin(2 * .pi * cutoff * x) / (.pi * x)
            let window = 0.42 - 0.5 * cos(2 * .pi * Double(i) / m) + 0.08 * cos(4 * .pi * Double(i) / m)
            return sinc * window
        }
        let sum = taps.reduce(0, +)
        taps = taps.map { $0 / sum }
        return taps.map(Float.init)
    }
}
