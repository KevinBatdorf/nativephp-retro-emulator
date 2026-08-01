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
    private val drainBuffer = FloatArray(minBufBytes / 4)

    @Volatile private var running = false
    private var drainThread: Thread? = null

    // 8 ms linear fade-in per stream start masks the start transient the
    // hardware route produces regardless of sample content.
    private var fadeRemaining = 0
    private companion object { const val FADE_FLOATS = 768 }

    fun start() {
        val track = buildTrack()
        audioTrack = track
        // Prime with silence, then play: play() on an empty stream starts the
        // output route mid-starvation, and the route opening lands as an
        // audible pop at every game boot.
        track.write(FloatArray(minBufBytes / 4), 0, minBufBytes / 4,
            AudioTrack.WRITE_BLOCKING)
        track.play()
        fadeRemaining = FADE_FLOATS
        running = true
        drainThread = Thread {
            var lastStatNanos = System.nanoTime()
            while (running) {
                val now = System.nanoTime()
                if (now - lastStatNanos > 5_000_000_000L) {
                    lastStatNanos = now
                    Log.i(TAG, "underrunCount=${track.underrunCount}")
                }
                val n = core.readAudio(drainBuffer)
                if (n > 0) {
                    if (fadeRemaining > 0) {
                        var i = 0
                        // Boot-time silence must not consume the ramp — the
                        // transient worth masking is the first audible attack.
                        if (fadeRemaining == FADE_FLOATS) {
                            while (i < n && drainBuffer[i] > -1e-5f && drainBuffer[i] < 1e-5f) i++
                        }
                        while (i < n && fadeRemaining > 0) {
                            drainBuffer[i] *= 1f - fadeRemaining.toFloat() / FADE_FLOATS
                            fadeRemaining--
                            i++
                        }
                    }
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
