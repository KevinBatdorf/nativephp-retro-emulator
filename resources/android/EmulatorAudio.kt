package com.kevinbatdorf.plugins.retroemulator

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log

private const val TAG = "EmulatorAudio"
private const val SAMPLE_RATE = 48000

/**
 * Streams mixed stereo audio from [EmulatorCore]'s native ring buffer into a
 * process-lifetime [AudioTrack].
 *
 * Opening the output route pops audibly on real hardware, so the track and
 * its pump thread are created once and never stop: games attach and detach,
 * and the pump writes silence between games to keep the route hot. Lifecycle:
 * call [start] after a ROM loads and [stop] on teardown, any number of times.
 */
class EmulatorAudio(private val core: EmulatorCore) {

    fun start() = Pump.attach(core)

    fun stop() = Pump.detach(core)

    /** Menu pause/resume: ramp out instead of cutting, fade back in. */
    fun setPaused(paused: Boolean) = Pump.setPaused(paused)

    private object Pump {

        private val minBufBytes = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_FLOAT,
        ).coerceAtLeast(4096)

        private var track: AudioTrack? = null
        private var thread: Thread? = null

        @Volatile private var source: EmulatorCore? = null
        @Volatile private var paused = false

        private val drainBuffer = FloatArray(minBufBytes / 4)

        // Silence writes pace the loop in real time while no game plays; the
        // route never idles into standby, so game boots reuse a hot stream.
        private val silence = FloatArray(minBufBytes / 8)

        // A faster ramp swings the GB idle DC audibly; boot silence must not consume it.
        private const val FADE_FLOATS = 19200
        private var fadeRemaining = 0
        private var firstAudible = false

        // Held tail level, ramped to zero once when a game detaches so the
        // switch to silence never cuts mid-wave.
        private var lastL = 0f
        private var lastR = 0f

        @Synchronized
        fun attach(core: EmulatorCore) {
            source = core
            paused = false
            fadeRemaining = FADE_FLOATS
            firstAudible = false
            ensurePump()
        }

        fun setPaused(value: Boolean) {
            if (!value && paused) {
                fadeRemaining = FADE_FLOATS
                firstAudible = true
            }
            paused = value
        }

        @Synchronized
        fun detach(core: EmulatorCore) {
            if (source === core) source = null
        }

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
            // 2× min — the buffer is kept full by the blocking pump, so its
            // size is direct output latency. LOW_LATENCY requests the fast
            // mixer path where the device supports it.
            .setBufferSizeInBytes(minBufBytes * 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()

        private fun ensurePump() {
            if (thread?.isAlive == true &&
                track?.state == AudioTrack.STATE_INITIALIZED
            ) return
            val t = buildTrack()
            track = t
            // Prime with silence, then play: play() on an empty stream starts
            // the route mid-starvation and the opening lands as a pop.
            t.write(FloatArray(minBufBytes / 4), 0, minBufBytes / 4,
                AudioTrack.WRITE_BLOCKING)
            t.play()
            thread = Thread { pumpLoop(t) }.apply {
                name = "emu-audio"
                start()
            }
            Log.i(TAG, "pump started — sampleRate=$SAMPLE_RATE bufferBytes=${t.bufferSizeInFrames * 8}")
        }

        // Speaker amps gate on near-silence and click awake at the next sound;
        // an inaudible 19.5 kHz pilot at -46 dBFS keeps them open.
        private var pilotPhase = 0.0
        private val pilotStep = 2.0 * Math.PI * 19_500.0 / SAMPLE_RATE
        private fun dither(buf: FloatArray, count: Int) {
            var phase = pilotPhase
            var i = 0
            while (i + 1 < count) {
                val v = (Math.sin(phase) * 0.005).toFloat()
                buf[i] += v
                buf[i + 1] += v
                phase += pilotStep
                i += 2
            }
            if (phase > 2.0 * Math.PI) phase -= 2.0 * Math.PI * Math.floor(phase / (2.0 * Math.PI))
            pilotPhase = phase
        }

        private fun pumpLoop(track: AudioTrack) {
            var lastStatNanos = System.nanoTime()
            while (true) {
                val now = System.nanoTime()
                if (now - lastStatNanos > 5_000_000_000L) {
                    lastStatNanos = now
                    Log.i(TAG, "underrunCount=${track.underrunCount}")
                }
                val src = source
                if (src == null || paused) {
                    if (lastL != 0f || lastR != 0f) writeTailRamp(track)
                    java.util.Arrays.fill(silence, 0f)
                    dither(silence, silence.size)
                    track.write(silence, 0, silence.size, AudioTrack.WRITE_BLOCKING)
                    continue
                }
                val n = src.readAudio(drainBuffer)
                if (n > 0) {
                    if (fadeRemaining > 0) {
                        var i = 0
                        if (!firstAudible) {
                            while (i < n && drainBuffer[i] > -1e-5f && drainBuffer[i] < 1e-5f) i++
                            if (i < n) firstAudible = true
                        }
                        while (i < n && fadeRemaining > 0) {
                            drainBuffer[i] *= 1f - fadeRemaining.toFloat() / FADE_FLOATS
                            fadeRemaining--
                            i++
                        }
                    }
                    if (n >= 2) {
                        lastL = drainBuffer[n - 2]
                        lastR = drainBuffer[n - 1]
                    }
                    dither(drainBuffer, n)
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
                    // A momentarily empty ring is normal between producer
                    // ticks; the track buffer bridges it. Synthesizing audio
                    // here (ramps, fades) turns each one into an audible event.
                    Thread.sleep(1)
                }
            }
        }

        /** Ramp the held tail level to zero over ~200 ms so the cut is inaudible. */
        private fun writeTailRamp(track: AudioTrack) {
            val frames = 9600
            val ramp = FloatArray(frames * 2)
            for (f in 0 until frames) {
                val g = 1f - (f + 1).toFloat() / frames
                ramp[f * 2] = lastL * g
                ramp[f * 2 + 1] = lastR * g
            }
            lastL = 0f
            lastR = 0f
            track.write(ramp, 0, ramp.size, AudioTrack.WRITE_BLOCKING)
        }
    }
}
