package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.abs

/**
 * Pins the ares GB ring contract the ear cares about: the idle level is
 * nonzero (a DC-blocking filter decays it to zero), constant from first
 * sample onward (re-centering filters drift it; the pre-fix mixer stepped
 * it), and no 5 ms window-mean step exceeds what game audio produces.
 * Each of the three has shipped broken as audible popping before.
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

    @Test
    fun aresGbIdlePedestalIsConstantAndBootHasNoDcSteps() {
        val rom = romOrSkip() ?: return
        val core = EmulatorCore()
        try {
            assert(core.init())
            assert(core.loadSystem("gb", backend = "ares")) { "loadSystem failed" }
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

            val first = means.take(20).average()             // first 100 ms: boot silence
            assert(abs(first) > 0.05) {
                "idle level $first is ~zero — a DC-blocking filter is back in the GB path"
            }

            // Quietest late 250 ms: the idle level must not have drifted.
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
            assert(abs(first - quietMean) < 0.02) {
                "idle level drifted $first -> $quietMean — the pedestal must hold, not re-center"
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
