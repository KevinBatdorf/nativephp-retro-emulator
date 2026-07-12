package com.kevinbatdorf.plugins.retroemulator

import android.content.Intent
import android.graphics.BitmapFactory
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import kotlin.math.abs

/**
 * Presentation-geometry coverage: overscan must be trimmed by default, the
 * screen node's scale/aspect must reach the renderer, and the letterboxed
 * output must match ares' best-fit reference math — so a stretch-to-fill or
 * overscan regression fails a green run instead of only being visible to a
 * human watching the device.
 */
@RunWith(AndroidJUnit4::class)
class VideoGeometryTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun overscanIsTrimmedByDefault() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)
            repeat(10) { core.tick() }
            assert(core.getFrameWidth() > 0) { "no frame produced" }

            val g = core.getVideoGeometry()
            // 564 is the full SFC overscan canvas; trimmed NTSC is 512×224.
            assert(g[0] == 512.0) { "node width must be trimmed to 512, was ${g[0]}" }
            assert(g[1] == 224.0) { "node height must be trimmed to 224, was ${g[1]}" }
            assert(g[2] == 0.5) { "SFC scaleX must be 0.5, was ${g[2]}" }
            assert(g[4] == 8.0 && g[5] == 7.0) {
                "SFC NTSC aspect must be 8:7, was ${g[4]}:${g[5]}"
            }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun overscanToggleShowsFullCanvas() {
        val core = AresCore()
        try {
            assert(core.init())
            assert(core.loadSystem("sfc"))
            assert(core.loadRom(makeLoRom(), null) == AresCore.LOAD_OK)
            repeat(10) { core.tick() }

            core.setVideo(
                luminance = 1f, saturation = 1f, gamma = 1f,
                colorBleed = false, interframeBlending = false,
                overscan = true,
            )
            repeat(5) { core.tick() }

            val g = core.getVideoGeometry()
            assert(g[0] == 564.0) { "overscan: true must show the 564-wide canvas, was ${g[0]}" }
        } finally {
            core.destroy()
        }
    }

    @Test
    fun letterboxMatchesAresBestFitReference() {
        // SFC NTSC trimmed: 512×224 at scale (0.5, 1.0), aspect 8:7 → 292×224
        // video, best-fit into 1920×1080 → 1407×1080 centered at x=256.
        val rect = computeOutputRect(
            nodeWidth = 512.0, nodeHeight = 224.0,
            scaleX = 0.5, scaleY = 1.0,
            aspectX = 8.0, aspectY = 7.0,
            rotation = 0,
            viewportWidth = 1920, viewportHeight = 1080,
        )
        assert(rect == OutputRect(256, 0, 1407, 1080)) { "got $rect" }
    }

    @Test
    fun rotationSwapsOutputDimensions() {
        val rect = computeOutputRect(
            nodeWidth = 512.0, nodeHeight = 224.0,
            scaleX = 0.5, scaleY = 1.0,
            aspectX = 8.0, aspectY = 7.0,
            rotation = 90,
            viewportWidth = 1080, viewportHeight = 1920,
        )
        assert(rect == OutputRect(0, 256, 1080, 1407)) { "got $rect" }
    }

    // 292×224 video (SFC trimmed, standard aspect) into 1920×1200:
    // multipliers are 6 and 5 → integer picks 5×.
    @Test
    fun integerModePicksLargestWholeMultiple() {
        val rect = sfcRect(1920, 1200, output = "integer")
        assert(rect == OutputRect(230, 40, 1460, 1120)) { "got $rect" }
    }

    @Test
    fun integerModeFallsBackToBestFitWhenOneTimesOverflows() {
        // Viewport narrower than the video → multiplier 0 → best-fit like desktop.
        val rect = sfcRect(280, 112, output = "integer")
        assert(rect == OutputRect(67, 0, 146, 112)) { "got $rect" }
    }

    @Test
    fun integerFixedHonorsAndClampsFixedScale() {
        val exact = sfcRect(1920, 1200, output = "integerFixed", fixedScale = 3)
        assert(exact == OutputRect(522, 264, 876, 672)) { "got $exact" }

        // 9× doesn't fit → clamps to the largest fitting multiple (5×).
        val clamped = sfcRect(1920, 1200, output = "integerFixed", fixedScale = 9)
        assert(clamped == OutputRect(230, 40, 1460, 1120)) { "got $clamped" }
    }

    @Test
    fun stretchFillsTheViewport() {
        val rect = sfcRect(1920, 1200, output = "stretch")
        assert(rect == OutputRect(0, 0, 1920, 1200)) { "got $rect" }
    }

    @Test
    fun videoOptionsMergeInsteadOfResetting() {
        val romPath = "/data/local/tmp/test.sfc"
        if (!File(romPath).exists()) {
            android.util.Log.w("VideoGeometryTest",
                "Skipping: no ROM at $romPath — push a .sfc ROM first")
            return
        }

        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EmulatorActivity.EXTRA_ROM_PATH, romPath)
        }
        ActivityScenario.launch<EmulatorActivity>(intent).use { scenario ->
            Thread.sleep(2_000)
            scenario.onActivity { activity ->
                val renderer = activity.renderer
                renderer.queueVideoOptions(output = "integer")
                renderer.queueVideoOptions(luminance = 0.5f)
                assert(renderer.videoOutput == "integer") {
                    "setting luminance must not reset output — got ${renderer.videoOutput}"
                }
                renderer.stopEmulation()
            }
            Thread.sleep(1_000)
        }
    }

    @Test
    fun aspectCorrectionNoneAndAnamorphic() {
        // none: square pixels — 256×224 best-fit into 1920×1080 → ×4.821.
        val none = sfcRect(1920, 1080, aspectCorrection = "none")
        assert(none == OutputRect(343, 0, 1234, 1080)) { "got $none" }

        // anamorphic: standard 292 then ·4/3 = 389 wide → ×1080/224.
        val ana = sfcRect(1920, 1080, aspectCorrection = "anamorphic")
        assert(ana == OutputRect(22, 0, 1875, 1080)) { "got $ana" }
    }

    /** SFC NTSC trimmed geometry (512×224, scale 0.5×1.0, aspect 8:7). */
    private fun sfcRect(
        viewportWidth: Int,
        viewportHeight: Int,
        output: String = "scale",
        fixedScale: Int = 2,
        aspectCorrection: String = "standard",
    ) = computeOutputRect(
        nodeWidth = 512.0, nodeHeight = 224.0,
        scaleX = 0.5, scaleY = 1.0,
        aspectX = 8.0, aspectY = 7.0,
        rotation = 0,
        viewportWidth = viewportWidth, viewportHeight = viewportHeight,
        output = output, fixedScale = fixedScale, aspectCorrection = aspectCorrection,
    )

    @Test
    fun screenshotIsLetterboxedToVideoAspect() {
        val romPath = "/data/local/tmp/test.sfc"

        // Skip rather than fail when no ROM is present (CI without ROM assets).
        if (!File(romPath).exists()) {
            android.util.Log.w("VideoGeometryTest",
                "Skipping: no ROM at $romPath — push a .sfc ROM first")
            return
        }

        val intent = Intent(context, EmulatorActivity::class.java).apply {
            putExtra(EmulatorActivity.EXTRA_ROM_PATH, romPath)
        }

        ActivityScenario.launch<EmulatorActivity>(intent).use { scenario ->
            Thread.sleep(3_000)
            scenario.onActivity { activity ->
                val renderer = activity.renderer
                val png = renderer.syncScreenshot()
                assert(png != null) { "screenshot must capture after 3 s of emulation" }

                val bmp = BitmapFactory.decodeByteArray(png!!, 0, png.size)
                val g = renderer.videoGeometry()
                var videoW = (g[0] * g[2]).toInt()
                val videoH = (g[1] * g[3]).toInt()
                videoW = (videoW * g[4] / g[5]).toInt()
                val expected = videoW.toDouble() / videoH

                val actual = bmp.width.toDouble() / bmp.height
                assert(abs(actual - expected) / expected < 0.02) {
                    "presented aspect $actual must match video aspect $expected " +
                        "(${bmp.width}×${bmp.height} in ${renderer.width}×${renderer.height})"
                }
                // Best-fit: the output fills the surface in exactly one dimension.
                assert(bmp.width <= renderer.width && bmp.height <= renderer.height)
                assert(
                    abs(bmp.width - renderer.width) <= 2 ||
                        abs(bmp.height - renderer.height) <= 2
                ) { "output ${bmp.width}×${bmp.height} must best-fit ${renderer.width}×${renderer.height}" }

                renderer.stopEmulation()
            }
            Thread.sleep(1_000)
        }
    }

    /** Same synthetic LoROM as Phase13SaveTest, without the battery RAM. */
    private fun makeLoRom(): ByteArray {
        val rom = ByteArray(0x8000)
        rom[0x0000] = 0x78
        rom[0x0001] = 0x80.toByte()
        rom[0x0002] = 0xFD.toByte()

        val hb = 0x7FB0
        val title = "ARES GEOMETRY TEST   ".toByteArray()
        for (i in 0 until 21) rom[hb + 0x10 + i] = title[i]

        rom[hb + 0x25] = 0x20
        rom[hb + 0x26] = 0x00 // ROM only
        rom[hb + 0x27] = 0x05
        rom[hb + 0x29] = 0x01

        rom[0x7FFC] = 0x00
        rom[0x7FFD] = 0x80.toByte()

        var sum = 0
        for (b in rom) sum = (sum + (b.toInt() and 0xFF)) and 0xFFFF
        rom[hb + 0x2E] = (sum and 0xFF).toByte()
        rom[hb + 0x2F] = (sum shr 8).toByte()
        val comp = sum.inv() and 0xFFFF
        rom[hb + 0x2C] = (comp and 0xFF).toByte()
        rom[hb + 0x2D] = (comp shr 8).toByte()
        return rom
    }
}
