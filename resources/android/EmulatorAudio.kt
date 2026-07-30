package com.kevinbatdorf.plugins.retroemulator

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log

private const val TAG = "EmulatorAudio"
private const val SAMPLE_RATE = 48000

/**
 * Streams mixed stereo audio from [EmulatorCore]'s native ring buffer into an
 * [AudioTrack] running in streaming mode.
 *
 * Lifecycle: call [start] after a ROM loads and [stop] on teardown — in that
 * order, any number of times. The AudioTrack is created per [start] because a
 * released track cannot be replayed (system switching stops and restarts
 * audio). [start] and [stop] are not thread-safe with respect to each other —
 * call them sequentially.
 */
class EmulatorAudio(private val core: EmulatorCore) {

    private val minBufBytes = AudioTrack.getMinBufferSize(
        SAMPLE_RATE,
        AudioFormat.CHANNEL_OUT_STEREO,
        AudioFormat.ENCODING_PCM_FLOAT,
    ).coerceAtLeast(4096)

    private var audioTrack: AudioTrack? = null

    private fun buildTrack(): AudioTrack = AudioTrack.Builder()
        .setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
        )
        .setAudioFormat(
            AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .build()
        )
        // 2× min — the AudioTrack buffer is kept full by the blocking drain
        // thread, so its size is direct output latency. LOW_LATENCY requests
        // the fast mixer path where the device supports it.
        .setBufferSizeInBytes(minBufBytes * 2)
        .setTransferMode(AudioTrack.MODE_STREAM)
        .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
        .build()

    // Drain buffer holds enough frames for one AudioTrack write at a time.
    // minBufBytes / (4 bytes/float) gives the frame capacity in floats.
    private val drainBuffer = FloatArray(minBufBytes / 4)

    @Volatile private var running = false
    private var drainThread: Thread? = null

    fun start() {
        val track = buildTrack()
        audioTrack = track
        track.play()
        running = true
        drainThread = Thread {
            while (running) {
                val n = core.readAudio(drainBuffer)
                if (n > 0) {
                    var written = 0
                    while (written < n) {
                        val result = track.write(
                            drainBuffer, written, n - written,
                            AudioTrack.WRITE_BLOCKING,
                        )
                        if (result < 0) {
                            Log.e(TAG, "AudioTrack write error: $result")
                            break
                        }
                        written += result
                    }
                } else {
                    Thread.sleep(1)
                }
            }
        }.apply {
            name = "emu-audio"
            start()
        }
        Log.i(TAG, "started — sampleRate=$SAMPLE_RATE bufferBytes=${track.bufferSizeInFrames * 8}")
    }

    fun stop() {
        running = false
        drainThread?.join(500)
        drainThread = null
        // Idempotent: stop() may be called again from a teardown path after the
        // track has already been stopped/released (e.g. stopEmulation followed
        // by Activity.onDestroy → release).
        audioTrack?.let { track ->
            if (track.state == AudioTrack.STATE_INITIALIZED) {
                track.stop()
                track.release()
                Log.i(TAG, "stopped")
            }
        }
        audioTrack = null
    }
}
