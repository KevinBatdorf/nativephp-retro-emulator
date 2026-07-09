#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full-screen triangle-strip quad covering NDC [-1,1]×[-1,1].
// UV origin is top-left; ares frame origin is also top-left.
vertex VertexOut blit_vert(uint vid [[vertex_id]]) {
    constexpr float2 pos[4] = { {-1,-1},{1,-1},{-1,1},{1,1} };
    constexpr float2 uvs[4] = { {0,1},{1,1},{0,0},{1,0} };
    VertexOut o;
    o.position = float4(pos[vid], 0, 1);
    o.uv = uvs[vid];
    return o;
}

fragment float4 blit_frag(VertexOut in            [[stage_in]],
                           texture2d<half> tex     [[texture(0)]],
                           sampler        samp     [[sampler(0)]]) {
    return float4(tex.sample(samp, in.uv));
}
