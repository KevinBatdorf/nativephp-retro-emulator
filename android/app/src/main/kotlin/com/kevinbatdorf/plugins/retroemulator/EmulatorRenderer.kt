package com.kevinbatdorf.plugins.retroemulator

import android.content.Context
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

private const val TAG = "EmulatorRenderer"

// Texture dimensions — large enough for any SFC canvas (max ~564×448).
private const val TEX_W = 1024
private const val TEX_H = 512

/**
 * A [GLSurfaceView] that drives the ares emulator and blits each frame via a
 * full-screen textured quad. All ares calls happen on the GL thread so libco
 * coroutine contexts are consistent throughout the session.
 *
 * Usage:
 * 1. Construct and set as the Activity's content view.
 * 2. Call [pendingSystemLoad] / [pendingRomLoad] with your data; the actual
 *    native calls execute on the first [onDrawFrame] after each flag is set.
 */
class EmulatorRenderer(context: Context) : GLSurfaceView(context) {

    private val core = AresCore()

    // Pending commands posted from the main thread and consumed on the GL thread.
    @Volatile var pendingIplRom: ByteArray?    = null
    @Volatile var pendingBoardsBml: ByteArray? = null
    @Volatile var pendingRomBytes: ByteArray?  = null

    @Volatile private var systemLoaded = false
    @Volatile private var romLoaded    = false

    // Current UV extents for the content region within the 1024×512 texture.
    private var uvW = 1f
    private var uvH = 1f

    private val renderer = object : GLSurfaceView.Renderer {

        private var textureId  = 0
        private var program    = 0
        private var posLoc     = 0
        private var texLoc     = 0
        private var uvScaleLoc = 0
        private var vbo        = 0

        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            GLES20.glClearColor(0f, 0f, 0f, 1f)

            // Compile shaders.
            program    = buildProgram(VERTEX_SRC, FRAGMENT_SRC)
            posLoc     = GLES20.glGetAttribLocation(program,  "aPos")
            texLoc     = GLES20.glGetUniformLocation(program, "uTex")
            uvScaleLoc = GLES20.glGetUniformLocation(program, "uUvScale")

            // Full-screen quad (two triangles as a triangle strip, NDC coords).
            val verts = floatArrayOf(
                -1f, -1f,
                 1f, -1f,
                -1f,  1f,
                 1f,  1f
            )
            val buf: FloatBuffer = ByteBuffer
                .allocateDirect(verts.size * 4)
                .order(ByteOrder.nativeOrder())
                .asFloatBuffer()
                .put(verts)
                .also { it.position(0) }

            val vbos = IntArray(1)
            GLES20.glGenBuffers(1, vbos, 0)
            vbo = vbos[0]
            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
            GLES20.glBufferData(
                GLES20.GL_ARRAY_BUFFER,
                verts.size * 4,
                buf,
                GLES20.GL_STATIC_DRAW
            )

            // Allocate the frame texture.
            val texIds = IntArray(1)
            GLES20.glGenTextures(1, texIds, 0)
            textureId = texIds[0]
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S,     GLES20.GL_CLAMP_TO_EDGE)
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T,     GLES20.GL_CLAMP_TO_EDGE)
            // Allocate storage (zeroed = black).
            GLES20.glTexImage2D(
                GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA,
                TEX_W, TEX_H, 0,
                GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null
            )

            // Initialise ares and hand it the texture ID.
            core.init()
            core.setupRenderer(textureId)
            Log.i(TAG, "Surface created — ares ${core.version()}")
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            GLES20.glViewport(0, 0, width, height)
        }

        override fun onDrawFrame(gl: GL10?) {
            // --- Consume pending system load ---
            val bml = pendingBoardsBml
            if (!systemLoaded && bml != null) {
                val ipl = pendingIplRom
                systemLoaded = core.loadSystem(ipl, bml)
                if (!systemLoaded) Log.e(TAG, "loadSystem failed")
            }

            // --- Consume pending ROM load ---
            val rom = pendingRomBytes
            if (systemLoaded && !romLoaded && rom != null) {
                romLoaded = core.loadRom(rom)
                pendingRomBytes = null   // consumed
                if (!romLoaded) Log.e(TAG, "loadRom failed")
            }

            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

            if (!romLoaded) return

            // Tick the emulator — video() callback uploads the frame to textureId.
            core.tick()

            // Update UV scale when the frame dimensions change.
            val fw = core.getFrameWidth()
            val fh = core.getFrameHeight()
            if (fw > 0 && fh > 0) {
                uvW = fw.toFloat() / TEX_W.toFloat()
                uvH = fh.toFloat() / TEX_H.toFloat()
            }

            // Draw the full-screen quad.
            GLES20.glUseProgram(program)

            GLES20.glBindBuffer(GLES20.GL_ARRAY_BUFFER, vbo)
            GLES20.glEnableVertexAttribArray(posLoc)
            GLES20.glVertexAttribPointer(posLoc, 2, GLES20.GL_FLOAT, false, 8, 0)

            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, textureId)
            GLES20.glUniform1i(texLoc, 0)
            GLES20.glUniform2f(uvScaleLoc, uvW, uvH)

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)

            GLES20.glDisableVertexAttribArray(posLoc)
        }
    }

    init {
        setEGLContextClientVersion(2)
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    // ---------------------------------------------------------------------------
    // Public API — called from the main thread; data is consumed on the GL thread.
    // ---------------------------------------------------------------------------

    /** Queue a system load; it executes on the next [onDrawFrame]. */
    fun queueSystemLoad(iplRomBytes: ByteArray?, boardsBmlBytes: ByteArray) {
        pendingIplRom    = iplRomBytes
        pendingBoardsBml = boardsBmlBytes
        requestRender()
    }

    /** Queue a ROM load; it executes on the next [onDrawFrame] after the system is ready. */
    fun queueRomLoad(romBytes: ByteArray) {
        pendingRomBytes = romBytes
        requestRender()
    }
}

// ---------------------------------------------------------------------------
// GL shader sources
// ---------------------------------------------------------------------------

// The ares video callback writes ARGB8888 (little-endian: BGRA in memory).
// When uploaded as GL_RGBA the channels are in BGRA order from GL's perspective,
// so the fragment shader swizzles .bgra → RGBA to display correctly.
private const val VERTEX_SRC = """
    attribute vec2 aPos;
    uniform   vec2 uUvScale;
    varying   vec2 vUv;
    void main() {
        // Flip Y so row 0 of the ares output (screen top) maps to the top of
        // the quad (GL NDC Y = +1).  Scale into the content region of the texture.
        vUv = vec2(
            (aPos.x * 0.5 + 0.5) * uUvScale.x,
            (0.5 - aPos.y * 0.5) * uUvScale.y
        );
        gl_Position = vec4(aPos, 0.0, 1.0);
    }
"""

private const val FRAGMENT_SRC = """
    precision mediump float;
    uniform sampler2D uTex;
    varying vec2      vUv;
    void main() {
        vec4 c = texture2D(uTex, vUv);
        // Swap R↔B: ares BGRA memory → correct RGBA display.
        gl_FragColor = c.bgra;
    }
"""

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------

private fun compileShader(type: Int, src: String): Int {
    val id = GLES20.glCreateShader(type)
    GLES20.glShaderSource(id, src)
    GLES20.glCompileShader(id)
    val status = IntArray(1)
    GLES20.glGetShaderiv(id, GLES20.GL_COMPILE_STATUS, status, 0)
    if (status[0] == 0) {
        Log.e(TAG, "Shader compile error: ${GLES20.glGetShaderInfoLog(id)}")
        GLES20.glDeleteShader(id)
        return 0
    }
    return id
}

private fun buildProgram(vertSrc: String, fragSrc: String): Int {
    val vert = compileShader(GLES20.GL_VERTEX_SHADER,   vertSrc)
    val frag = compileShader(GLES20.GL_FRAGMENT_SHADER, fragSrc)
    val prog = GLES20.glCreateProgram()
    GLES20.glAttachShader(prog, vert)
    GLES20.glAttachShader(prog, frag)
    GLES20.glLinkProgram(prog)
    val status = IntArray(1)
    GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, status, 0)
    if (status[0] == 0) {
        Log.e(TAG, "Program link error: ${GLES20.glGetProgramInfoLog(prog)}")
    }
    GLES20.glDeleteShader(vert)
    GLES20.glDeleteShader(frag)
    return prog
}
