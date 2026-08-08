import AVFoundation
import RetroEmulator

/// Streams audio from the ares ring buffer into AVAudioEngine.
///
/// emu_read_audio() returns interleaved stereo floats (L,R,L,R,...) at 48 kHz.
/// AVAudioEngine expects non-interleaved (planar) PCM, so we deinterleave on pull.
///
/// The engine, player, and scheduler live for the process — opening the output
/// route pops audibly on real hardware, so games attach and detach from one
/// hot route and the scheduler feeds silence between them.
final class EmulatorAudio {

    /// The context this instance registered. Detach compares against it: the
    /// pump outlives every renderer, and a torn-down renderer's deinit can run
    /// after the next game has already registered — clearing unconditionally
    /// silenced every later game until the app restarted.
    private var owned: OpaquePointer?

    var ctx: OpaquePointer? {
        get { Pump.shared.ctx }
        set {
            owned = newValue
            Pump.shared.ctx = newValue
        }
    }

    func start() throws { Pump.shared.attach() }

    func stop() { Pump.shared.detach(owner: owned) }

    /// Menu pause/resume: ramp out instead of cutting, fade back in.
    func setPaused(_ value: Bool) { Pump.shared.setPaused(value) }

    private final class Pump {

        static let shared = Pump()

        private let engine     = AVAudioEngine()
        private let player     = AVAudioPlayerNode()
        private let sampleRate = 48_000.0
        private let bufSize: AVAudioFrameCount = 1024

        // A single in-flight buffer gaps the output at every completion →
        // reschedule hop — audible as constant crackle.
        private let queueDepth = 3

        private lazy var format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sampleRate,
            channels: 2,
            interleaved: false)!

        private var scratch = [Float](repeating: 0, count: 1024 * 2)

        // A faster ramp swings the GB idle DC audibly; boot silence must not consume it.
        private var fadeRemaining = 0
        private var firstAudible = false
        private let fadeFloats = 19200

        // Between games and while paused: ramp the held tail to zero across
        // quiet buffers, then feed silence so the queue never drains mid-wave.
        private var paused = false
        private var lastL: Float = 0
        private var lastR: Float = 0
        private var tailDone = 0
        private let tailTotal = 9600

        // Completions must hop queues: inline rescheduling on AVFAudio's
        // realtime thread deadlocks against player stops.
        private let queue = DispatchQueue(label: "com.retroemulator.audio")
        private var pumping = false

        private var _ctx: OpaquePointer?
        var ctx: OpaquePointer? {
            get { queue.sync { _ctx } }
            set { queue.sync { _ctx = newValue } }
        }

        func attach() {
            ensurePumping()
            queue.async { [self] in
                fadeRemaining = fadeFloats
                firstAudible = false
                paused = false
            }
        }

        func detach(owner: OpaquePointer?) {
            queue.async { [self] in
                guard owner == nil || _ctx == owner else { return }
                _ctx = nil
                tailDone = 0
            }
        }

        func setPaused(_ value: Bool) {
            queue.async { [self] in
                if !value && paused {
                    fadeRemaining = fadeFloats
                    firstAudible = true
                }
                if value && !paused {
                    tailDone = 0
                }
                paused = value
            }
        }

        // MARK: - Private

        private func ensurePumping() {
            // Interruptions stop the engine; restart on attach rather than hold a dead route.
            if engine.isRunning, queue.sync(execute: { pumping }) { return }
            if engine.attachedNodes.contains(player) == false {
                engine.attach(player)
                engine.connect(player, to: engine.mainMixerNode, format: format)
            }
            try? engine.start()
            // Fill the queue before play(): opening the route mid-starvation
            // lands as a pop.
            queue.sync {
                guard !pumping else { return }
                pumping = true
                for _ in 0..<queueDepth { scheduleNext() }
            }
            player.play()
        }

        /// Runs on `queue` only. Detached or paused: ramp the held tail to
        /// zero, then silence buffers keep the queue fed.
        private func scheduleQuietBuffer() {
            guard let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: bufSize) else { return }
            buf.frameLength = bufSize
            if let L = buf.floatChannelData?[0], let R = buf.floatChannelData?[1] {
                if lastL != 0 || lastR != 0 {
                    let frames = Int(bufSize)
                    for f in 0..<frames {
                        let g = max(0, 1 - Float(tailDone + f) / Float(tailTotal))
                        L[f] = lastL * g
                        R[f] = lastR * g
                    }
                    tailDone += frames
                    if tailDone >= tailTotal {
                        lastL = 0
                        lastR = 0
                        tailDone = 0
                    }
                } else {
                    for f in 0..<Int(bufSize) {
                        L[f] = 0
                        R[f] = 0
                    }
                }
            }
            player.scheduleBuffer(buf) { [weak self] in
                guard let self else { return }
                self.queue.async { self.scheduleNext() }
            }
        }

        /// Runs on `queue` only. One call chain per queued-buffer slot: a
        /// schedule hands the slot to the completion, an empty ring parks it
        /// on a short retry — either way exactly one continuation keeps the
        /// slot alive.
        private func scheduleNext() {
            if paused || _ctx == nil {
                scheduleQuietBuffer()
                return
            }

            var count = 0
            if let ctx = _ctx {
                count = scratch.withUnsafeMutableBufferPointer {
                    emu_read_audio(ctx, $0.baseAddress, $0.count)
                }
            }

            // Empty ring: retry shortly instead of scheduling a zero-length
            // buffer — those complete immediately, spin this loop, and starve
            // the player into gap-pops. The other queued buffers cover the wait.
            guard count > 0 else {
                // A momentarily empty ring is normal between producer ticks;
                // the queued buffers bridge it. Synthesizing audio here turns
                // each one into an audible event.
                queue.asyncAfter(deadline: .now() + .milliseconds(2)) { [weak self] in
                    self?.scheduleNext()
                }
                return
            }

            if fadeRemaining > 0 {
                var i = 0
                if !firstAudible {
                    while i < count && abs(scratch[i]) < 1e-5 { i += 1 }
                    if i < count { firstAudible = true }
                }
                while i < count && fadeRemaining > 0 {
                    scratch[i] *= 1 - Float(fadeRemaining) / Float(fadeFloats)
                    fadeRemaining -= 1
                    i += 1
                }
            }
            if count >= 2 {
                lastL = scratch[count - 2]
                lastR = scratch[count - 1]
            }

            let frames = AVAudioFrameCount(count / 2)
            guard let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: bufSize) else { return }
            buf.frameLength = frames

            // A partial read ships as a short buffer, NOT zero-padded: the next
            // buffer's samples are contiguous with these, while silence padding
            // would insert an audible discontinuity.
            if let L = buf.floatChannelData?[0], let R = buf.floatChannelData?[1] {
                for i in 0..<Int(frames) {
                    L[i] = scratch[i * 2]
                    R[i] = scratch[i * 2 + 1]
                }
            }

            player.scheduleBuffer(buf) { [weak self] in
                guard let self else { return }
                self.queue.async { self.scheduleNext() }
            }
        }
    }
}
