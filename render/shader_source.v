module render

import sokol.gfx

// Sokol compiles GLSL on OpenGL and MSL on Metal. Metal compiles each stage as its
// own library, so both stages can use the same entry name.

// Metal clip space has z in [0, 1]; the projection matrices produce OpenGL-style
// z in [-1, 1], so Metal vertex shaders remap it with this helper.
const msl_clip_z = '
static inline float4 gl_to_metal_z(float4 p) {
    p.z = (p.z + p.w) * 0.5;
    return p;
}
'

fn set_stage_source(mut stage gfx.ShaderStageDesc, glsl string, msl string) {
	$if macos {
		stage.source = &char(msl.str)
		stage.entry = c'main0'
	} $else {
		stage.source = &char(glsl.str)
	}
}

const ui_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct UiUniforms { float2 u_size; };
struct UiIn {
    float2 a_pos [[attribute(0)]];
    float2 a_uv [[attribute(1)]];
    float a_layer [[attribute(2)]];
    float4 a_color [[attribute(3)]];
};
struct UiOut { float4 position [[position]]; float2 uv; float layer [[flat]]; float4 color; };
vertex UiOut main0(UiIn in [[stage_in]], constant UiUniforms& u [[buffer(0)]]) {
    UiOut o;
    o.position = float4(in.a_pos.x / u.u_size.x * 2.0 - 1.0, 1.0 - in.a_pos.y / u.u_size.y * 2.0, 0.0, 1.0);
    o.uv = in.a_uv; o.layer = in.a_layer; o.color = in.a_color;
    return o;
}
'

const ui_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct UiOut { float4 position [[position]]; float2 uv; float layer [[flat]]; float4 color; };
fragment float4 main0(UiOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv) * in.color;
}
'

const ui_array_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct UiOut { float4 position [[position]]; float2 uv; float layer [[flat]]; float4 color; };
fragment float4 main0(UiOut in [[stage_in]], texture2d_array<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv, uint(round(in.layer))) * in.color;
}
'

const dome_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct DomeOut { float4 position [[position]]; float2 ndc; };
vertex DomeOut main0(uint vid [[vertex_id]]) {
    int2 p = int2((int(vid) << 1) & 2, int(vid) & 2);
    DomeOut o;
    o.ndc = float2(p) * 2.0 - 1.0;
    o.position = float4(o.ndc, 0.9995, 1.0);
    return o;
}
'

const dome_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct DomeUniforms { float4 forward_tan; float4 right_aspect; float4 up_glow; float4 zenith; float4 horizon; float4 sun; };
struct DomeOut { float4 position [[position]]; float2 ndc; };
fragment float4 main0(DomeOut in [[stage_in]], constant DomeUniforms& u [[buffer(0)]]) {
    float3 d = normalize(u.forward_tan.xyz + u.right_aspect.xyz * (in.ndc.x * u.forward_tan.w * u.right_aspect.w) + u.up_glow.xyz * (in.ndc.y * u.forward_tan.w));
    float h = clamp(d.y, -1.0, 1.0);
    float3 c = mix(u.horizon.xyz, u.zenith.xyz, pow(clamp(h, 0.0, 1.0), 0.55));
    if (h < 0.0) c = mix(u.horizon.xyz, u.horizon.xyz * 0.85, clamp(-h * 3.0, 0.0, 1.0));
    float g = pow(max(dot(d, u.sun.xyz), 0.0), 6.0) * u.up_glow.w * (1.0 - clamp(abs(h) * 1.2, 0.0, 1.0));
    return float4(mix(c, float3(1.0, 0.6, 0.3), clamp(g, 0.0, 0.85)), 1.0);
}
'

const sky_quad_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct SkyUniforms { float4x4 vp; };
struct SkyIn {
    float3 pos [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct SkyOut { float4 position [[position]]; float2 vuv; float4 vcolor; };
' + msl_clip_z + '
vertex SkyOut main0(SkyIn in [[stage_in]], constant SkyUniforms& u [[buffer(0)]]) {
    SkyOut o;
    o.position = gl_to_metal_z(u.vp * float4(in.pos, 1.0));
    o.vuv = in.uv; o.vcolor = in.color;
    return o;
}
'

const sky_quad_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct SkyOut { float4 position [[position]]; float2 vuv; float4 vcolor; };
fragment float4 main0(SkyOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.vuv) * in.vcolor;
}
'

const world_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct WorldUniforms { float4x4 u_vp; float4 u_offset; };
struct WorldIn {
    float3 a_pos [[attribute(0)]];
    float2 a_uv [[attribute(1)]];
    float a_layer [[attribute(2)]];
    float4 a_color [[attribute(3)]];
    float4 a_light [[attribute(4)]];
};
struct WorldOut {
    float4 position [[position]];
    float2 uv;
    float layer [[flat]];
    float4 color;
    float2 light;
    float dist;
};
' + msl_clip_z + '
vertex WorldOut main0(WorldIn in [[stage_in]], constant WorldUniforms& u [[buffer(0)]]) {
    float3 p = in.a_pos + u.u_offset.xyz;
    WorldOut o;
    o.position = gl_to_metal_z(u.u_vp * float4(p, 1.0));
    o.uv = in.a_uv; o.layer = in.a_layer; o.color = in.a_color;
    o.light = in.a_light.xy;
    o.dist = length(p);
    return o;
}
'

const world_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct WorldUniforms { float4 u_fog; float4 u_params; };
struct WorldOut {
    float4 position [[position]];
    float2 uv;
    float layer [[flat]];
    float4 color;
    float2 light;
    float dist;
};
fragment float4 main0(WorldOut in [[stage_in]],
                      texture2d_array<float> atlas [[texture(0)]],
                      texture2d<float> anim [[texture(1)]],
                      sampler smp [[sampler(0)]],
                      constant WorldUniforms& u [[buffer(0)]]) {
    float2 an = anim.read(uint2(uint(int(in.layer + 0.5)), 0)).rg;
    float a = u.u_params.z / max(an.y, 1.0);
    float b = max(an.x, 1.0);
    float frame = floor(a - b * floor(a / b));
    float4 texel = atlas.sample(smp, in.uv, uint(round(in.layer + frame)));
    if (texel.a < u.u_params.w) discard_fragment();
    float3 l = max(float3(in.light.x * u.u_fog.w), float3(1.0, 0.87, 0.70) * in.light.y);
    float3 bright = mix(float3(0.035), float3(1.0), l * l * (0.35 + 0.65 * l));
    float f = clamp((in.dist - u.u_params.x) / max(u.u_params.y - u.u_params.x, 0.001), 0.0, 1.0);
    return float4(mix(texel.rgb * in.color.rgb * bright, u.u_fog.rgb, f), texel.a * in.color.a);
}
'

const cloud_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct CloudUniforms { float4x4 vp; float4 offset; };
struct CloudIn {
    float3 pos [[attribute(0)]];
    float shade [[attribute(1)]];
};
struct CloudOut { float4 position [[position]]; float vshade; float dist; };
' + msl_clip_z + '
vertex CloudOut main0(CloudIn in [[stage_in]], constant CloudUniforms& u [[buffer(0)]]) {
    float3 p = in.pos + u.offset.xyz;
    CloudOut o;
    o.position = gl_to_metal_z(u.vp * float4(p, 1.0));
    o.vshade = in.shade;
    o.dist = length(p);
    return o;
}
'

const cloud_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct CloudUniforms { float4 cloud_color; float4 fog; float4 params; };
struct CloudOut { float4 position [[position]]; float vshade; float dist; };
fragment float4 main0(CloudOut in [[stage_in]], constant CloudUniforms& u [[buffer(0)]]) {
    float f = clamp((in.dist - u.params.x) / max(u.params.y - u.params.x, 0.001), 0.0, 1.0);
    return float4(mix(u.cloud_color.rgb * in.vshade, u.fog.rgb, f), 0.82 * (1.0 - f * 0.9));
}
'

const particle_vs_msl = '#include <metal_stdlib>
using namespace metal;
struct ParticleUniforms { float4x4 vp; };
struct ParticleIn {
    float3 pos [[attribute(0)]];
    float3 uvl [[attribute(1)]];
    float4 col [[attribute(2)]];
};
struct ParticleOut { float4 position [[position]]; float3 texcoord; float4 color; float dist; };
' + msl_clip_z + '
vertex ParticleOut main0(ParticleIn in [[stage_in]], constant ParticleUniforms& u [[buffer(0)]]) {
    ParticleOut o;
    o.position = gl_to_metal_z(u.vp * float4(in.pos, 1.0));
    o.texcoord = in.uvl; o.color = in.col;
    o.dist = length(in.pos);
    return o;
}
'

const particle_fs_msl = '#include <metal_stdlib>
using namespace metal;
struct ParticleUniforms { float4 fog; float4 params; };
struct ParticleOut { float4 position [[position]]; float3 texcoord; float4 color; float dist; };
fragment float4 main0(ParticleOut in [[stage_in]],
                      texture2d_array<float> atlas [[texture(0)]],
                      sampler smp [[sampler(0)]],
                      constant ParticleUniforms& u [[buffer(0)]]) {
    float4 t = atlas.sample(smp, in.texcoord.xy, uint(round(in.texcoord.z)));
    if (t.a < 0.3) discard_fragment();
    float f = clamp((in.dist - u.params.x) / max(u.params.y - u.params.x, 0.001), 0.0, 1.0);
    return float4(mix(t.rgb * in.color.rgb, u.fog.rgb, f), 1.0);
}
'
