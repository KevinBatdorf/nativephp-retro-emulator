package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.abs

/**
 * Idle sits at zero and no 5 ms window-mean step exceeds what game audio
 * produces. Both have shipped broken as audible popping before.
 */
@RunWith(AndroidJUnit4::class)
class AudioBootSmoothnessTest {

    private val romPath = "/data/local/tmp/test-gb.rom"

    private fun romOrSkip(): ByteArray? {
        val file = File(romPath)
        if (!file.exists()) {
            android.util.Log.w("AudioBootSmoothnessTest", "Skipping: no ROM at $romPath")
            return null
        }
        return file.readBytes()
    }

    /** Dumps boot audio so a human can hear the real code path, not a filtered WAV. */
    @Test
    fun dumpAresGbBootAudio() = dumpBootAudio("ares")

    @Test
    fun dumpSameboyGbBootAudio() = dumpBootAudio("sameboy")

    private fun dumpBootAudio(backend: String) {
        val rom = romOrSkip() ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = backend)) { "loadSystem failed" }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK) { "loadRom failed" }

            val out = File("/sdcard/Download/boot-dump-$backend.f32")
            out.outputStream().buffered().use { sink ->
                val buf = FloatArray(8192)
                val bytes = java.nio.ByteBuffer.allocate(8192 * 4)
                    .order(java.nio.ByteOrder.LITTLE_ENDIAN)
                repeat(420) {
                    core.tick()
                    while (true) {
                        val n = core.readAudio(buf)
                        if (n <= 0) break
                        bytes.clear()
                        for (i in 0 until n) bytes.putFloat(buf[i])
                        sink.write(bytes.array(), 0, n * 4)
                    }
                }
            }
            android.util.Log.i("AudioBootSmoothnessTest", "wrote ${out.length()} bytes")
        } finally {
            core.destroy()
        }
    }

    @Test
    fun aresGbIdleSitsAtZeroAndBootHasNoDcSteps() = idleSitsAtZero("ares")

    /** SameBoy mixes zero-mean per channel (patched apu.c); pin the same contract. */
    @Test
    fun sameboyGbIdleSitsAtZeroAndBootHasNoDcSteps() = idleSitsAtZero("sameboy")

    private fun idleSitsAtZero(backend: String) {
        val rom = romOrSkip() ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = backend)) { "loadSystem failed" }
            assert(core.loadRom(rom) == EmulatorCore.LOAD_OK) { "loadRom failed" }

            val left = ArrayList<Float>(48_000 * 3)
            val buf = FloatArray(8192)
            repeat(180) {
                core.tick()
                while (true) {
                    val n = core.readAudio(buf)
                    if (n <= 0) break
                    var i = 0
                    while (i < n) {
                        left.add(buf[i])
                        i += 2
                    }
                }
            }
            assert(left.size > 48_000) { "drained ${left.size} samples — expected >1s of audio" }

            val w = 240
            val means = ArrayList<Double>(left.size / w)
            var idx = 0
            while (idx + w <= left.size) {
                var s = 0.0
                for (j in idx until idx + w) s += left[j]
                means.add(s / w)
                idx += w
            }

            // Skip the filter's own settling time, then boot silence must be silent.
            val first = means.drop(10).take(20).average()
            assert(abs(first) < 0.01) {
                "idle level $first is off zero — the GB pedestal is reaching the ring"
            }

            // Quietest late 250 ms: silence must still be silent, not drifted onto a rail.
            val lateStart = means.size / 2
            var quietMean = 0.0
            var quietSpread = Double.MAX_VALUE
            for (i in lateStart until means.size - 50) {
                val slice = means.subList(i, i + 50)
                val mn = slice.min()
                val mx = slice.max()
                if (mx - mn < quietSpread) {
                    quietSpread = mx - mn
                    quietMean = slice.average()
                }
            }
            assert(abs(quietMean) < 0.02) {
                "late idle level $quietMean is off zero — a pedestal is building up"
            }

            // No adjacent-window step beyond what game onsets produce.
            var maxStep = 0.0
            for (i in 1 until means.size) {
                val d = abs(means[i] - means[i - 1])
                if (d > maxStep) maxStep = d
            }
            assert(maxStep < 0.08) {
                "window-mean step $maxStep exceeds the audible-pop threshold"
            }
        } finally {
            core.destroy()
        }
    }
}
