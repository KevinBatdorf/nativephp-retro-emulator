package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Instrumented test for the ares JNI library.
 *
 * Phase 3 success criteria:
 *   - Library loads without UnsatisfiedLinkError
 *   - nativeInit() returns true without SIGABRT
 *   - nativeTick() completes without crashing
 *   - nativeDestroy() completes without crashing
 */
@RunWith(AndroidJUnit4::class)
class AresCoreTest {

    @Test
    fun loadsLibraryWithoutCrash() {
        // System.loadLibrary is called in AresCore's init block.
        // If the .so is missing or corrupt this throws UnsatisfiedLinkError.
        val core = AresCore()
        assertNotNull(core)
    }

    @Test
    fun initReturnsTrueWithoutCrash() {
        val core = AresCore()
        assertTrue("nativeInit() must return true on first call", core.init())
        core.destroy()
    }

    @Test
    fun tickDoesNotCrash() {
        val core = AresCore()
        core.init()
        core.tick()   // no-op stub in Phase 3 — must not SIGABRT
        core.destroy()
    }

    @Test
    fun versionStringIsNonEmpty() {
        val core = AresCore()
        core.init()
        val version = core.version()
        assertTrue("version must be non-empty", version.isNotEmpty())
        core.destroy()
    }
}
