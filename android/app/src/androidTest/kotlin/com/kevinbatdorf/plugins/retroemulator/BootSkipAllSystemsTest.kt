package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Boots every system x engine with a real ROM. The host logs
 * "boot animation skipped (N+M frames)" per boot; systems without a console
 * animation log nothing, which is the correct result for them.
 */
@RunWith(AndroidJUnit4::class)
class BootSkipAllSystemsTest {

    private val combos = listOf(
        Triple("gb", "ares", "tetrisdx.gb"),
        Triple("gb", "sameboy", "tetrisdx.gb"),
        Triple("gbc", "ares", "shantae.gbc"),
        Triple("gbc", "sameboy", "shantae.gbc"),
        Triple("gba", "mgba", "blind-jump.gba"),
        Triple("gba", "ares", "blind-jump.gba"),
        Triple("md", "ares", "miniplanets.bin"),
        Triple("fc", "ares", "super-tilt-bro.nes"),
        Triple("sfc", "ares", "helloworld.sfc"),
    )

    @Test
    fun bootsEverySystemAndEngine() {
        val results = StringBuilder()
        for ((sys, backend, name) in combos) {
            val file = File("/data/local/tmp/roms-test/$name")
            if (!file.exists()) {
                android.util.Log.w("BootSkipAllSystems", "SKIP $sys/$backend — no ROM at $file")
                continue
            }
            val core = EmulatorCore()
            try {
                assert(core.init()) { "$sys/$backend init failed" }
                assert(core.loadSystem(sys, backend = backend)) { "$sys/$backend loadSystem failed" }
                android.util.Log.i("BootSkipAllSystems", "BOOTING $sys/$backend")
                assert(core.loadRom(file.readBytes()) == EmulatorCore.LOAD_OK) {
                    "$sys/$backend loadRom failed"
                }
                var audio = 0L
                val buf = FloatArray(8192)
                repeat(150) {
                    core.tick()
                    while (true) {
                        val n = core.readAudio(buf)
                        if (n <= 0) break
                        audio += n
                    }
                }
                results.append("$sys/$backend ok audio=$audio; ")
                android.util.Log.i("BootSkipAllSystems", "BOOTED $sys/$backend audioFloats=$audio")
            } finally {
                core.destroy()
            }
        }
        android.util.Log.i("BootSkipAllSystems", "RESULTS: $results")
    }
}
