import Metal
import MetalKit

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
    func submitFrame(_ pixels: [UInt32], width: Int, height: Int) {
        lock.lock()
        pending  = pixels
        pendingW = width
        pendingH = height
        lock.unlock()
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        lock.lock()
        let px = pending
        let pw = pendingW
        let ph = pendingH
        pending = nil
        lock.unlock()

        if let px, pw > 0, ph > 0 {
            uploadTexture(pixels: px, width: pw, height: ph)
        }

        guard let drawable   = view.currentDrawable,
              let descriptor = view.currentRenderPassDescriptor,
              let tex        = texture else { return }

        guard let cmd = commandQueue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        enc.setRenderPipelineState(pipelineState)
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
