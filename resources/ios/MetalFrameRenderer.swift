import Metal
import MetalKit

/// Presentation settings mirroring ares desktop's Video settings.
struct PresentationSettings {
    var output = "scale"            // scale | integer | integerFixed | stretch
    var fixedScale = 2
    var aspectCorrection = "standard"  // none | standard | anamorphic
}

/// Screen-node presentation geometry, captured alongside each frame
/// (ares_get_video_geometry order).
struct VideoGeometry {
    var width: Double = 0
    var height: Double = 0
    var scaleX: Double = 1
    var scaleY: Double = 1
    var aspectX: Double = 1
    var aspectY: Double = 1
    var rotation: Int = 0

    init() {}

    init(values: [Double]) {
        width = values[0]; height = values[1]
        scaleX = values[2]; scaleY = values[3]
        aspectX = values[4]; aspectY = values[5]
        rotation = Int(values[6])
    }

    /// Port of ares desktop-ui/program/platform.cpp:95-166: video size is
    /// width·scaleX (·aspectX/aspectY unless aspectCorrection "none", ·4/3
    /// more for "anamorphic") × height·scaleY, swapped at 90°/270°, then
    /// sized per output mode — "scale" best-fit, "integer" largest whole
    /// multiple, "integerFixed" exactly fixedScale×, "stretch" fill — with
    /// desktop's fallbacks when a mode doesn't fit. u32 truncation at each
    /// step mirrors the reference. Returns the normalized output scale for
    /// the centered quad, or nil when nothing can be sized.
    func outputScale(
        viewportWidth: Int, viewportHeight: Int, settings: PresentationSettings
    ) -> SIMD2<Float>? {
        var videoWidth  = Int(width * scaleX)
        var videoHeight = Int(height * scaleY)
        if settings.aspectCorrection != "none", aspectY > 0 {
            videoWidth = Int(Double(videoWidth) * aspectX / aspectY)
        }
        if settings.aspectCorrection == "anamorphic" { videoWidth = videoWidth * 4 / 3 }
        if rotation == 90 || rotation == 270 { swap(&videoWidth, &videoHeight) }
        guard videoWidth > 0, videoHeight > 0, viewportWidth > 0, viewportHeight > 0 else {
            return nil
        }

        let multiplierX = viewportWidth / videoWidth
        let multiplierY = viewportHeight / videoHeight
        let multiplier  = min(multiplierX, multiplierY)
        let bestFitScale = min(
            Float(viewportWidth) / Float(videoWidth),
            Float(viewportHeight) / Float(videoHeight)
        )

        var outputWidth  = videoWidth * multiplier
        var outputHeight = videoHeight * multiplier

        if multiplier == 0 || settings.output == "scale" {
            outputWidth  = Int(Float(videoWidth) * bestFitScale)
            outputHeight = Int(Float(videoHeight) * bestFitScale)
        }
        // "integer" keeps video·multiplier from above; the reference's inner
        // fallback is unreachable there (multiplier == 0 is caught first).

        if settings.output == "integerFixed" {
            var fixedMult = max(1, settings.fixedScale)
            if fixedMult > multiplierX || fixedMult > multiplierY {
                fixedMult = max(1, min(multiplierX, multiplierY))
                if multiplierX == 0 || multiplierY == 0 {
                    outputWidth  = Int(Float(videoWidth) * bestFitScale)
                    outputHeight = Int(Float(videoHeight) * bestFitScale)
                } else {
                    outputWidth  = videoWidth * fixedMult
                    outputHeight = videoHeight * fixedMult
                }
            } else {
                outputWidth  = videoWidth * fixedMult
                outputHeight = videoHeight * fixedMult
            }
        }

        if settings.output == "stretch" {
            outputWidth  = viewportWidth
            outputHeight = viewportHeight
        }

        return SIMD2<Float>(
            Float(outputWidth) / Float(viewportWidth),
            Float(outputHeight) / Float(viewportHeight)
        )
    }
}

/// MTKViewDelegate that blits the latest ares ARGB8888 frame to screen via Metal.
///
/// ares pixel format is 0xAARRGGBB; on little-endian ARM that is [BB,GG,RR,AA] in
/// memory, which is exactly MTLPixelFormatBGRA8Unorm — no channel swizzle needed.
final class MetalFrameRenderer: NSObject, MTKViewDelegate {

    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState
    private let sampler: MTLSamplerState

    private var texture: MTLTexture?
    private var texWidth  = 0
    private var texHeight = 0

    private let lock = NSLock()
    private var pending: [UInt32]?
    private var pendingW = 0
    private var pendingH = 0
    private var geometry = VideoGeometry()
    private var settings = PresentationSettings()

    /// Thread-safe. Called from the bridge thread when SetVideo changes
    /// presentation settings; applies on the next draw.
    func setPresentation(_ newSettings: PresentationSettings) {
        lock.lock()
        settings = newSettings
        lock.unlock()
    }

    init?(device: MTLDevice, pixelFormat: MTLPixelFormat) {
        self.device = device

        guard let queue = device.makeCommandQueue() else { return nil }
        commandQueue = queue

        guard let lib = device.makeDefaultLibrary(),
              let vert = lib.makeFunction(name: "blit_vert"),
              let frag = lib.makeFunction(name: "blit_frag") else { return nil }

        let pDesc = MTLRenderPipelineDescriptor()
        pDesc.vertexFunction   = vert
        pDesc.fragmentFunction = frag
        pDesc.colorAttachments[0].pixelFormat = pixelFormat
        guard let ps = try? device.makeRenderPipelineState(descriptor: pDesc) else { return nil }
        pipelineState = ps

        let sDesc = MTLSamplerDescriptor()
        sDesc.minFilter = .nearest
        sDesc.magFilter = .nearest
        guard let s = device.makeSamplerState(descriptor: sDesc) else { return nil }
        sampler = s

        super.init()
    }

    /// Thread-safe. Called from the emulation loop thread after each ares_tick().
    func submitFrame(_ pixels: [UInt32], width: Int, height: Int, geometry: VideoGeometry) {
        lock.lock()
        pending  = pixels
        pendingW = width
        pendingH = height
        self.geometry = geometry
        lock.unlock()
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        lock.lock()
        let px = pending
        let pw = pendingW
        let ph = pendingH
        let geom = geometry
        let pres = settings
        pending = nil
        lock.unlock()

        if let px, pw > 0, ph > 0 {
            uploadTexture(pixels: px, width: pw, height: ph)
        }

        guard let drawable   = view.currentDrawable,
              let descriptor = view.currentRenderPassDescriptor,
              let tex        = texture else { return }

        guard var posScale = geom.outputScale(
            viewportWidth: Int(view.drawableSize.width),
            viewportHeight: Int(view.drawableSize.height),
            settings: pres
        ) else { return }

        guard let cmd = commandQueue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        enc.setRenderPipelineState(pipelineState)
        enc.setVertexBytes(&posScale, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()

        cmd.present(drawable)
        cmd.commit()
    }

    // MARK: - Private

    private func uploadTexture(pixels: [UInt32], width: Int, height: Int) {
        if texture == nil || texWidth != width || texHeight != height {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm,
                width: width, height: height, mipmapped: false)
            desc.usage       = [.shaderRead]
            desc.storageMode = .shared
            texture   = device.makeTexture(descriptor: desc)
            texWidth  = width
            texHeight = height
        }
        pixels.withUnsafeBytes { raw in
            texture?.replace(
                region: MTLRegionMake2D(0, 0, width, height),
                mipmapLevel: 0,
                withBytes: raw.baseAddress!,
                bytesPerRow: width * 4)
        }
    }
}
