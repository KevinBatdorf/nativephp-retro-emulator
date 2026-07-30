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

    private lazy var format = AVAudioFormat(
        commonFormat: .pcmFormatFloat32,
        sampleRate: sampleRate,
        channels: 2,
        interleaved: false)!

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
        queue.async { [weak self] in self?.scheduleNext() }
    }

    func stop() {
        queue.sync { running = false }
        player.stop()
        engine.stop()
    }

    // MARK: - Private

    /// Runs on `queue` only.
    private func scheduleNext() {
        guard running else { return }
        guard let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: bufSize) else { return }

        var interleaved = [Float](repeating: 0, count: Int(bufSize) * 2)
        var count = 0
        if let ctx {
            count = interleaved.withUnsafeMutableBufferPointer {
                emu_read_audio(ctx, $0.baseAddress, $0.count)
            }
        }

        let frames = AVAudioFrameCount(count / 2)
        buf.frameLength = frames

        if frames > 0, let L = buf.floatChannelData?[0], let R = buf.floatChannelData?[1] {
            for i in 0..<Int(frames) {
                L[i] = interleaved[i * 2]
                R[i] = interleaved[i * 2 + 1]
            }
        }

        player.scheduleBuffer(buf) { [weak self] in
            guard let self else { return }
            self.queue.async { self.scheduleNext() }
        }
    }
}
