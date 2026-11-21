import AVFoundation
import AudioToolbox
import os

enum CaptureError: LocalizedError {
    case audioUnit(String, OSStatus)

    var errorDescription: String? {
        switch self {
        case let .audioUnit(step, status):
            "Audio unit failed to \(step) (error \(status))."
        }
    }
}

/// Owns the iPhone's audio hardware, in one of three states:
///
/// * `capturing`: records the microphone raw (RemoteIO, measurement mode, no app-side
///   processing) and emits 10 ms frames of 48 kHz mono Float32 through `onFrame`. Only the
///   iPhone's own Mic Mode can still process it, when it isn't Standard (the live screen warns then).
/// * `keepingAlive`: releases the microphone (no orange indicator) but keeps a silent,
///   mixable output running so iOS doesn't suspend the app while it's muted in the background.
/// * `stopped`.
///
/// Switching between them (the audio session and the I/O unit) takes a moment, up to a few
/// hundred milliseconds, so it runs on a queue of its own: on the main thread it would stall
/// the UI, the mute button's animation first. Requests run in order, and one that a newer
/// request replaces before it starts is skipped, so only the latest counts.
@MainActor
@Observable
final class AudioCapture {
    enum State: Equatable, Sendable {
        case stopped
        case capturing
        case keepingAlive
    }

    enum Outcome: Sendable {
        case done
        case failed(Error)
        /// A newer request came in meanwhile; it has the say.
        case replaced
    }

    @ObservationIgnored var onFrame: (@Sendable ([Float]) -> Void)?
    /// Peak level (0...1) about 20 times per second.
    @ObservationIgnored var onLevel: (@Sendable (Float) -> Void)?

    /// Where the hardware is, as of the last request that finished.
    private(set) var state: State = .stopped
    /// What the latest request asked for (the state it's in, or on its way to).
    @ObservationIgnored private(set) var wanted: State = .stopped
    @ObservationIgnored private(set) var startedAt = Date.distantPast
    @ObservationIgnored private(set) var hardwareSampleRate: Double = 0

    var isCapturing: Bool {
        if case .capturing = state { return true }
        return false
    }

    private let hardware = AudioHardware()
    private let queue = DispatchQueue(label: "Phomic audio", qos: .userInitiated)
    @ObservationIgnored private var requests = 0
    private let newestRequest = OSAllocatedUnfairLock(initialState: 0)
    @ObservationIgnored private var shownSerial = 0
    @ObservationIgnored private var unfinished = 0
    @ObservationIgnored private var waitingForIdle: [CheckedContinuation<Void, Never>] = []

    /// Moves the hardware to `target`; `restart` starts it again even if it's there already.
    /// `fromBackground`: see AudioHardware.startCapture. Returns at once; `then` runs on the main
    /// actor once the switch is done.
    func set(_ target: State, restart: Bool = false, fromBackground: Bool = false,
             then: (@MainActor @Sendable (Outcome) -> Void)? = nil) {
        requests += 1
        unfinished += 1
        let request = requests
        wanted = target
        newestRequest.withLock { $0 = request }
        let hardware = self.hardware
        let newest = self.newestRequest
        let onFrame = self.onFrame ?? { _ in }
        let onLevel = self.onLevel ?? { _ in }
        queue.async { [weak self] in
            var result: Result<Void, Error>?
            if newest.withLock({ $0 }) == request {
                result = Result {
                    try hardware.move(to: target, restart: restart, fromBackground: fromBackground,
                                      onFrame: onFrame, onLevel: onLevel)
                }
            }
            let finished = result
            let snapshot = hardware.snapshot()
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    self?.finish(request, finished, snapshot, then: then)
                }
            }
        }
    }

    /// Returns once every request made so far is done.
    func idle() async {
        guard unfinished > 0 else { return }
        await withCheckedContinuation { waitingForIdle.append($0) }
    }

    /// `result` is nil for a request that was skipped.
    private func finish(_ request: Int, _ result: Result<Void, Error>?, _ snapshot: AudioHardware.Snapshot,
                        then: (@MainActor @Sendable (Outcome) -> Void)?) {
        if snapshot.serial > shownSerial {
            shownSerial = snapshot.serial
            state = snapshot.state
            startedAt = snapshot.startedAt
            hardwareSampleRate = snapshot.sampleRate
        }
        let outcome: Outcome
        switch result {
        case .some(.success) where request == requests:
            outcome = .done
        case .some(.failure(let error)) where request == requests:
            wanted = state
            outcome = .failed(error)
        default:
            outcome = .replaced
        }
        then?(outcome)
        unfinished -= 1
        if unfinished == 0 {
            let waiting = waitingForIdle
            waitingForIdle = []
            waiting.forEach { $0.resume() }
        }
    }
}

/// The audio session and I/O unit behind AudioCapture. Used on AudioCapture's queue only.
private final class AudioHardware: @unchecked Sendable {
    struct Snapshot: Sendable {
        var serial: Int
        var state: AudioCapture.State
        var startedAt: Date
        var sampleRate: Double
    }

    private var state: AudioCapture.State = .stopped
    private var startedAt = Date.distantPast
    private var hardwareSampleRate: Double = 0
    /// Counts changes, so the main actor can tell an old snapshot from a newer one.
    private var serial = 0

    private let ring = SampleRing(capacity: 1 << 16)
    private let wake = DispatchSemaphore(value: 0)
    private var io: AudioUnitIO?
    private var pump: FramePump?

    func snapshot() -> Snapshot {
        Snapshot(serial: serial, state: state, startedAt: startedAt, sampleRate: hardwareSampleRate)
    }

    func move(to target: AudioCapture.State, restart: Bool, fromBackground: Bool,
              onFrame: @escaping @Sendable ([Float]) -> Void,
              onLevel: @escaping @Sendable (Float) -> Void) throws {
        guard target != state || restart else { return }
        defer { serial += 1 }
        switch target {
        case .capturing:
            try startCapture(fromBackground: fromBackground, onFrame: onFrame, onLevel: onLevel)
        case .keepingAlive:
            try startKeepAlive()
        case .stopped:
            stop()
        }
    }

    /// `fromBackground`: iOS only lets a background app activate an audio session that mixes
    /// with other audio (error '!int' otherwise), so then use play-and-record with mixing
    /// (still raw, measurement mode).
    private func startCapture(fromBackground: Bool,
                              onFrame: @escaping @Sendable ([Float]) -> Void,
                              onLevel: @escaping @Sendable (Float) -> Void) throws {
        // Close the current session first: switching an active session's category in the
        // background makes iOS treat the new one as interrupting others.
        stop()

        let session = AVAudioSession.sharedInstance()
        if fromBackground {
            try session.setCategory(.playAndRecord, mode: .measurement, options: [.mixWithOthers])
        } else {
            try session.setCategory(.record, mode: .measurement, options: [])
        }
        try? session.setPreferredSampleRate(WireProtocol.sampleRate)
        try? session.setPreferredIOBufferDuration(0.005)
        try session.setActive(true)

        let rate = session.sampleRate
        ring.reset()
        let io = AudioUnitIO(ring: ring, wake: wake)
        do {
            try io.startCapture(sampleRate: rate)
        } catch {
            try? session.setActive(false, options: .notifyOthersOnDeactivation)
            throw error
        }

        let pump = FramePump(
            ring: ring,
            wake: wake,
            inputRate: rate,
            onFrame: onFrame,
            onLevel: onLevel
        )
        let thread = Thread { pump.run() }
        thread.name = "Phomic audio pump"
        thread.qualityOfService = .userInteractive
        thread.start()

        self.io = io
        self.pump = pump
        hardwareSampleRate = rate
        state = .capturing
        startedAt = Date()
    }

    private func startKeepAlive() throws {
        stop() // close the (possibly non-mixable) recording session first, see startCapture
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try session.setActive(true)
        let io = AudioUnitIO(ring: ring, wake: wake)
        try io.startSilentOutput(sampleRate: session.sampleRate)
        self.io = io
        state = .keepingAlive
        startedAt = Date()
    }

    private func stop() {
        let wasActive = state != .stopped
        stopIO()
        if wasActive {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }

    /// Stops the audio I/O but leaves the audio session as it is.
    private func stopIO() {
        pump?.stop()
        pump = nil
        io?.stop()
        io = nil
        state = .stopped
    }
}

// MARK: - Core Audio I/O unit

/// Wraps one RemoteIO / VoiceProcessingIO audio unit. Created on AudioCapture's queue,
/// but its render callbacks run on Core Audio's real-time thread.
final class AudioUnitIO: @unchecked Sendable {
    private let ring: SampleRing
    private let wake: DispatchSemaphore
    private var unit: AudioUnit?
    private let bufferCapacity = 8192 // iOS raises the slice size to 4096 frames when the screen locks
    private let buffer: UnsafeMutablePointer<Float>

    init(ring: SampleRing, wake: DispatchSemaphore) {
        self.ring = ring
        self.wake = wake
        buffer = .allocate(capacity: bufferCapacity)
        buffer.initialize(repeating: 0, count: bufferCapacity)
    }

    deinit {
        stop()
        buffer.deallocate()
    }

    func startCapture(sampleRate: Double) throws {
        let unit = try Self.makeUnit(subtype: kAudioUnitSubType_RemoteIO)
        self.unit = unit
        do {
            // Bus 1 = microphone, bus 0 = speaker.
            try Self.set(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, UInt32(1))
            try Self.set(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, UInt32(0))
            try Self.set(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, Self.monoFloat(sampleRate))

            let input = AURenderCallbackStruct(
                inputProc: { refCon, flags, timestamp, _, frameCount, _ in
                    Unmanaged<AudioUnitIO>.fromOpaque(refCon)
                        .takeUnretainedValue()
                        .renderInput(flags: flags, timestamp: timestamp, frameCount: frameCount)
                },
                inputProcRefCon: Unmanaged.passUnretained(self).toOpaque()
            )
            try Self.set(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, input)
            // We render into our own buffer.
            try? Self.set(unit, kAudioUnitProperty_ShouldAllocateBuffer, kAudioUnitScope_Output, 1, UInt32(0))

            try Self.check(AudioUnitInitialize(unit), "initialize")
            try Self.check(AudioOutputUnitStart(unit), "start")
        } catch {
            stop()
            throw error
        }
    }

    func startSilentOutput(sampleRate: Double) throws {
        let unit = try Self.makeUnit(subtype: kAudioUnitSubType_RemoteIO)
        self.unit = unit
        do {
            try Self.set(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, UInt32(1))
            try Self.set(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, Self.monoFloat(sampleRate))
            try Self.set(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, Self.silenceCallback())
            try Self.check(AudioUnitInitialize(unit), "initialize")
            try Self.check(AudioOutputUnitStart(unit), "start")
        } catch {
            stop()
            throw error
        }
    }

    func stop() {
        guard let unit else { return }
        AudioOutputUnitStop(unit) // synchronous: no callbacks run after this returns
        AudioUnitUninitialize(unit)
        AudioComponentInstanceDispose(unit)
        self.unit = nil
    }

    /// Real-time thread: pull the microphone samples and copy them into the ring.
    private func renderInput(flags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
                             timestamp: UnsafePointer<AudioTimeStamp>,
                             frameCount: UInt32) -> OSStatus {
        guard let unit, Int(frameCount) <= bufferCapacity else { return noErr }
        var list = AudioBufferList(
            mNumberBuffers: 1,
            mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: frameCount * 4, mData: UnsafeMutableRawPointer(buffer))
        )
        let status = AudioUnitRender(unit, flags, timestamp, 1, frameCount, &list)
        if status == noErr {
            ring.write(buffer, count: Int(frameCount), stride: 1)
            wake.signal()
        }
        return status
    }

    private static func silenceCallback() -> AURenderCallbackStruct {
        AURenderCallbackStruct(
            inputProc: { _, flags, _, _, _, ioData in
                if let ioData {
                    for buffer in UnsafeMutableAudioBufferListPointer(ioData) {
                        if let data = buffer.mData {
                            memset(data, 0, Int(buffer.mDataByteSize))
                        }
                    }
                }
                flags.pointee.insert(.unitRenderAction_OutputIsSilence)
                return noErr
            },
            inputProcRefCon: nil
        )
    }

    private static func makeUnit(subtype: OSType) throws -> AudioUnit {
        var description = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: subtype,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )
        guard let component = AudioComponentFindNext(nil, &description) else {
            throw CaptureError.audioUnit("find the audio I/O component", -1)
        }
        var unit: AudioUnit?
        try check(AudioComponentInstanceNew(component, &unit), "open")
        guard let unit else { throw CaptureError.audioUnit("open", -1) }
        return unit
    }

    private static func monoFloat(_ sampleRate: Double) -> AudioStreamBasicDescription {
        AudioStreamBasicDescription(
            mSampleRate: sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 32,
            mReserved: 0
        )
    }

    private static func set<T>(_ unit: AudioUnit, _ property: AudioUnitPropertyID, _ scope: AudioUnitScope,
                               _ element: AudioUnitElement, _ value: T) throws {
        let status = withUnsafePointer(to: value) { pointer in
            AudioUnitSetProperty(unit, property, scope, element, pointer, UInt32(MemoryLayout<T>.size))
        }
        try check(status, "configure property \(property)")
    }

    private static func check(_ status: OSStatus, _ step: String) throws {
        if status != noErr {
            throw CaptureError.audioUnit(step, status)
        }
    }
}

// MARK: - Frame pump

/// Drains the ring buffer, resamples to 48 kHz if needed, and emits 10 ms frames.
final class FramePump: @unchecked Sendable {
    private let ring: SampleRing
    private let wake: DispatchSemaphore
    private let converter: RateConverter?
    private let onFrame: @Sendable ([Float]) -> Void
    private let onLevel: @Sendable (Float) -> Void
    private let stopped = OSAllocatedUnfairLock(initialState: false)

    init(ring: SampleRing, wake: DispatchSemaphore, inputRate: Double,
         onFrame: @escaping @Sendable ([Float]) -> Void, onLevel: @escaping @Sendable (Float) -> Void) {
        self.ring = ring
        self.wake = wake
        self.converter = inputRate == WireProtocol.sampleRate ? nil : RateConverter(inputRate: inputRate)
        self.onFrame = onFrame
        self.onLevel = onLevel
    }

    func stop() {
        stopped.withLock { $0 = true }
        wake.signal()
    }

    func run() {
        let chunk = 4096
        let scratch = UnsafeMutablePointer<Float>.allocate(capacity: chunk)
        defer { scratch.deallocate() }

        let frameSamples = WireProtocol.frameSamples
        var pending: [Float] = []
        pending.reserveCapacity(8192)
        var levelPeak: Float = 0
        var framesSinceLevel = 0

        while !stopped.withLock({ $0 }) {
            _ = wake.wait(timeout: .now() + .milliseconds(100))

            while true {
                let count = ring.read(into: scratch, max: chunk)
                if count == 0 { break }
                let samples = UnsafeBufferPointer(start: scratch, count: count)
                if let converter {
                    converter.convert(samples, appendingTo: &pending)
                } else {
                    pending.append(contentsOf: samples)
                }
            }

            var offset = 0
            while pending.count - offset >= frameSamples {
                let frame = Array(pending[offset..<(offset + frameSamples)])
                offset += frameSamples
                var peak: Float = 0
                for sample in frame {
                    peak = max(peak, abs(sample))
                }
                onFrame(frame)

                levelPeak = max(levelPeak, peak)
                framesSinceLevel += 1
                if framesSinceLevel >= 5 {
                    onLevel(levelPeak)
                    levelPeak = 0
                    framesSinceLevel = 0
                }
            }
            if offset > 0 {
                pending.removeFirst(offset)
            }
        }
    }
}

// MARK: - Sample-rate conversion

/// Mono Float32 at the hardware rate -> mono Float32 at 48 kHz. Only used when the
/// hardware doesn't already run at 48 kHz.
final class RateConverter {
    private let converter: AVAudioConverter
    private let inputFormat: AVAudioFormat
    private let outputFormat: AVAudioFormat
    private let ratio: Double

    init?(inputRate: Double) {
        guard let inputFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: inputRate, channels: 1, interleaved: false),
              let outputFormat = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: WireProtocol.sampleRate, channels: 1, interleaved: false),
              let converter = AVAudioConverter(from: inputFormat, to: outputFormat) else {
            return nil
        }
        converter.sampleRateConverterQuality = AVAudioQuality.high.rawValue
        self.converter = converter
        self.inputFormat = inputFormat
        self.outputFormat = outputFormat
        self.ratio = WireProtocol.sampleRate / inputRate
    }

    func convert(_ samples: UnsafeBufferPointer<Float>, appendingTo output: inout [Float]) {
        guard let base = samples.baseAddress, !samples.isEmpty,
              let inBuffer = AVAudioPCMBuffer(pcmFormat: inputFormat, frameCapacity: AVAudioFrameCount(samples.count)),
              let channel = inBuffer.floatChannelData?[0] else {
            return
        }
        channel.update(from: base, count: samples.count)
        inBuffer.frameLength = AVAudioFrameCount(samples.count)

        let capacity = AVAudioFrameCount(Double(samples.count) * ratio) + 64
        guard let outBuffer = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: capacity) else {
            return
        }

        nonisolated(unsafe) var supplied = false
        nonisolated(unsafe) let source = inBuffer
        var error: NSError?
        let status = converter.convert(to: outBuffer, error: &error) { _, inputStatus in
            if supplied {
                // Keep the converter's internal state for the next chunk.
                inputStatus.pointee = .noDataNow
                return nil
            }
            supplied = true
            inputStatus.pointee = .haveData
            return source
        }
        guard status != .error, let data = outBuffer.floatChannelData?[0] else {
            return
        }
        output.append(contentsOf: UnsafeBufferPointer(start: data, count: Int(outBuffer.frameLength)))
    }
}

// MARK: - Ring buffer

/// Single-producer / single-consumer float FIFO. The lock is held only for the copy,
/// so the real-time producer never waits long. Overflow drops the oldest samples.
final class SampleRing: @unchecked Sendable {
    private let capacity: Int
    private let storage: UnsafeMutablePointer<Float>
    private let lock: UnsafeMutablePointer<os_unfair_lock>
    private var writeTotal = 0
    private var readTotal = 0

    init(capacity: Int) {
        self.capacity = capacity
        storage = .allocate(capacity: capacity)
        storage.initialize(repeating: 0, count: capacity)
        lock = .allocate(capacity: 1)
        lock.initialize(to: os_unfair_lock())
    }

    deinit {
        storage.deallocate()
        lock.deinitialize(count: 1)
        lock.deallocate()
    }

    func write(_ source: UnsafePointer<Float>, count: Int, stride: Int) {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        var index = writeTotal % capacity
        for i in 0..<count {
            storage[index] = source[i * stride]
            index += 1
            if index == capacity { index = 0 }
        }
        writeTotal += count
        if writeTotal - readTotal > capacity {
            readTotal = writeTotal - capacity
        }
    }

    func read(into destination: UnsafeMutablePointer<Float>, max: Int) -> Int {
        os_unfair_lock_lock(lock)
        defer { os_unfair_lock_unlock(lock) }
        let count = min(max, writeTotal - readTotal)
        var index = readTotal % capacity
        for i in 0..<count {
            destination[i] = storage[index]
            index += 1
            if index == capacity { index = 0 }
        }
        readTotal += count
        return count
    }

    func reset() {
        os_unfair_lock_lock(lock)
        writeTotal = 0
        readTotal = 0
        os_unfair_lock_unlock(lock)
    }
}
