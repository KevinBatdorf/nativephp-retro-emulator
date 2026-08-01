import AVFoundation
import RetroEmulator

/// Streams audio from the ares ring buffer into AVAudioEngine.
///
/// emu_read_audio() returns interleaved stereo floats (L,R,L,R,...) at 48 kHz.
/// AVAudioEngine expects non-interleaved (planar) PCM, so we deinterleave on pull.
final class EmulatorAudio {

    var ctx: OpaquePointer?

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

    // 8 ms linear fade-in per stream start masks the start transient the
    // hardware route produces regardless of sample content. Boot silence
    // must not consume the ramp.
    private var fadeRemaining = 0
    private let fadeFloats = 768

    // Rescheduling MUST NOT run inline in the scheduleBuffer completion
    // handler: that fires on AVFAudio's realtime messenger thread holding
    // internal locks, and a concurrent player.stop() takes the same locks in
    // the opposite order — observed as a hard deadlock (bridge thread stuck in
    // Stop, messenger stuck in scheduleBuffer). Completions hop to this queue
    // instead, and `running` (queue-confined) gates out anything in flight
    // once stop() has run.
    private let queue = DispatchQueue(label: "com.retroemulator.audio")
    private var running = false

    func start() throws {
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)
        try engine.start()
        player.play()
        queue.sync { running = true }
        queue.async { [weak self] in
            guard let self else { return }
            self.fadeRemaining = self.fadeFloats
            for _ in 0..<self.queueDepth { self.scheduleNext() }
        }
    }

    func stop() {
        queue.sync { running = false }
        player.stop()
        engine.stop()
    }

    // MARK: - Private

    /// Runs on `queue` only. One call chain per queued-buffer slot: a schedule
    /// hands the slot to the completion, an empty ring parks it on a short
    /// retry — either way exactly one continuation keeps the slot alive.
    private func scheduleNext() {
        guard running else { return }

        var count = 0
        if let ctx {
            count = scratch.withUnsafeMutableBufferPointer {
                emu_read_audio(ctx, $0.baseAddress, $0.count)
            }
        }

        // Empty ring: retry shortly instead of scheduling a zero-length
        // buffer — those complete immediately, spin this loop, and starve the
        // player into gap-pops. The other queued buffers cover the wait.
        guard count > 0 else {
            queue.asyncAfter(deadline: .now() + .milliseconds(2)) { [weak self] in
                self?.scheduleNext()
            }
            return
        }

        if fadeRemaining > 0 {
            var i = 0
            if fadeRemaining == fadeFloats {
                while i < count && abs(scratch[i]) < 1e-5 { i += 1 }
            }
            while i < count && fadeRemaining > 0 {
                scratch[i] *= 1 - Float(fadeRemaining) / Float(fadeFloats)
                fadeRemaining -= 1
                i += 1
            }
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
