package com.kevinbatdorf.plugins.retroemulator

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Instrumented test for the ares JNI library.
 *
 * Asserts:
 *   - Library loads without UnsatisfiedLinkError
 *   - nativeInit() returns true without SIGABRT
 *   - nativeTick() completes without crashing
 *   - nativeDestroy() completes without crashing
 */
@RunWith(AndroidJUnit4::class)
class EmulatorCoreTest {

    @Test
    fun loadsLibraryWithoutCrash() {
        // System.loadLibrary is called in EmulatorCore's init block.
        // If the .so is missing or corrupt this throws UnsatisfiedLinkError.
        val core = EmulatorCore()
        assertNotNull(core)
    }

    @Test
    fun initReturnsTrueWithoutCrash() {
        val core = EmulatorCore()
        assertTrue("nativeInit() must return true on first call", core.init())
        core.destroy()
    }

    @Test
    fun tickDoesNotCrash() {
        val core = EmulatorCore()
        core.init()
        core.tick()   // must not SIGABRT with no ROM loaded
        core.destroy()
    }

    @Test
    fun versionStringIsNonEmpty() {
        val core = EmulatorCore()
        core.init()
        val version = core.version()
        assertTrue("version must be non-empty", version.isNotEmpty())
        core.destroy()
    }
}
