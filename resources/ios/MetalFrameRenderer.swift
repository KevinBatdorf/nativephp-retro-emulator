import Metal
import MetalKit

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

    /// Port of ares desktop-ui/program/platform.cpp:95-115 with desktop's
    /// default settings (output "Scale" = best-fit, aspect correction
    /// "Standard"): video size is width·scaleX·aspectX/aspectY × height·scaleY
    /// (swapped at 90°/270°), best-fit scaled into the viewport. The reference
    /// truncates to u32 at each step — mirrored here. Returns the normalized
    /// output scale for the centered quad, or nil when nothing can be sized.
    func outputScale(viewportWidth: Double, viewportHeight: Double) -> SIMD2<Float>? {
        var videoWidth  = (width * scaleX).rounded(.towardZero)
        var videoHeight = (height * scaleY).rounded(.towardZero)
        if aspectY > 0 { videoWidth = (videoWidth * aspectX / aspectY).rounded(.towardZero) }
        if rotation == 90 || rotation == 270 { swap(&videoWidth, &videoHeight) }
        guard videoWidth > 0, videoHeight > 0, viewportWidth > 0, viewportHeight > 0 else {
            return nil
        }
        let frac = min(viewportWidth / videoWidth, viewportHeight / videoHeight)
        let outputWidth  = (videoWidth * frac).rounded(.towardZero)
        let outputHeight = (videoHeight * frac).rounded(.towardZero)
        return SIMD2<Float>(
            Float(outputWidth / viewportWidth),
            Float(outputHeight / viewportHeight)
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
        pending = nil
        lock.unlock()

        if let px, pw > 0, ph > 0 {
            uploadTexture(pixels: px, width: pw, height: ph)
        }

        guard let drawable   = view.currentDrawable,
              let descriptor = view.currentRenderPassDescriptor,
              let tex        = texture else { return }

        guard var posScale = geom.outputScale(
            viewportWidth: Double(view.drawableSize.width),
            viewportHeight: Double(view.drawableSize.height)
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
