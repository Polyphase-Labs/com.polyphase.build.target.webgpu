/**
 * @file WebGPUShaders.h
 * @brief Embedded WGSL shader sources for the WebGPU backend.
 *
 * Console-style ubershaders, semantics 1:1 with WebGL2Shaders.h: a small
 * fixed set with uniform-driven feature toggles (unlit/dynamic/baked light
 * mode, fog mode, alpha cutoff) instead of a permutation cache. Shader-graph
 * materials degrade to this standard shading exactly like they do on
 * GameCube / 3DS — the baked material SPIR-V in .oct files is ignored.
 *
 * Uniform layout rule: every member is a vec4/mat4 (or an array of vec4), so
 * the WGSL struct layout is byte-identical to the packed CPU mirror structs in
 * Graphics_WebGPU.cpp (which static_assert their offsets). Scalars ride in
 * vec4 lanes; integer toggles use vec4<i32>.
 *
 * Unlike the GL backend there is no uHasTexture toggle: draws always bind a
 * texture (a 1x1 white fallback when the material has none) and the shader
 * samples unconditionally — which is also what WGSL's uniform-control-flow
 * rule for textureSample wants.
 *
 * Colour convention: the engine packs vertex colours as R in the low byte;
 * unorm8x4 vertex input yields (r,g,b,a) in that order, matching GL's
 * GL_UNSIGNED_BYTE x4 normalized attribs. Fog matches
 * Engine/Shaders/GLSL/src/Fog.glsl: world-space euclidean distance, linear or
 * exponential density, plus the sky horizon fade.
 */

#pragma once

// ---------------------------------------------------------------------------
// 3D mesh ubershader (static, CPU-skinned skeletal, text mesh, terrain,
// tilemap, debug mesh)
// ---------------------------------------------------------------------------

static const char* kMeshWGSL = R"WGSL(
struct MeshUniforms {
    mvp        : mat4x4<f32>,
    model      : mat4x4<f32>,
    normal0    : vec4<f32>,          // inverse-transpose model, columns
    normal1    : vec4<f32>,
    normal2    : vec4<f32>,
    baseColor  : vec4<f32>,
    modes      : vec4<i32>,          // x lightMode(0/1/2)  y fogMode(0/1/2)  z fogExponential  w numPointLights
    params     : vec4<f32>,          // x vertexColorScale  y alphaCutoff(<0 = off)  z fogNear  w fogFar
    ambient    : vec4<f32>,          // xyz ambient
    dirDir     : vec4<f32>,          // xyz surface->light  w hasDirLight(0/1)
    dirColor   : vec4<f32>,
    fogColor   : vec4<f32>,          // rgb colour, a intensity
    camPos     : vec4<f32>,
    pointPosRadius : array<vec4<f32>, 8>,   // xyz pos, w radius
    pointColor     : array<vec4<f32>, 8>,
}

@group(0) @binding(0) var<uniform> u : MeshUniforms;
@group(1) @binding(0) var uSampler : sampler;
@group(1) @binding(1) var uTexture : texture_2d<f32>;

struct VsIn {
    @location(0) position : vec3<f32>,
    @location(1) uv0      : vec2<f32>,
    @location(2) uv1      : vec2<f32>,
    @location(3) normal   : vec3<f32>,
    @location(4) color    : vec4<f32>,   // unorm8x4; constant white via a stride-0 buffer when absent
}

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv0      : vec2<f32>,
    @location(1) normal   : vec3<f32>,
    @location(2) worldPos : vec3<f32>,
    @location(3) color    : vec4<f32>,
}

@vertex
fn vs_main(in : VsIn) -> VsOut {
    var out : VsOut;
    out.position = u.mvp * vec4<f32>(in.position, 1.0);
    out.uv0      = in.uv0;
    let nrm = mat3x3<f32>(u.normal0.xyz, u.normal1.xyz, u.normal2.xyz);
    out.normal   = nrm * in.normal;
    out.worldPos = (u.model * vec4<f32>(in.position, 1.0)).xyz;
    out.color    = in.color;
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    // Cooked vertex/baked colors are stored pre-divided by the engine's
    // ColorScale; restore the range at the input (floats have the headroom).
    let vc = in.color * u.params.x;

    let diffuse = u.baseColor * textureSample(uTexture, uSampler, in.uv0);

    // Dynamic light sum (light colors are authored full-range; no ColorScale).
    var dyn = vec3<f32>(0.0);
    if (u.modes.x != 0) {
        let n = normalize(in.normal);
        if (u.dirDir.w != 0.0) {
            dyn += max(dot(n, u.dirDir.xyz), 0.0) * u.dirColor.rgb;
        }
        for (var i = 0; i < 8; i++) {
            if (i >= u.modes.w) { break; }
            let toL   = u.pointPosRadius[i].xyz - in.worldPos;
            let dist  = length(toL);
            let t     = clamp(dist / u.pointPosRadius[i].w, 0.0, 1.0);
            let atten = 1.0 - t;
            if (atten > 0.0) {
                let l = toL / max(dist, 0.0001);
                dyn += max(dot(n, l), 0.0) * atten * u.pointColor[i].rgb;
            }
        }
    }

    var color : vec4<f32>;
    if (u.modes.x == 2) {
        // Baked vertex lighting (Vulkan Forward.frag parity):
        // final = diffuse * baked + dynamicLights * diffuse; ambient skipped.
        color = vec4<f32>(diffuse.rgb * (vc.rgb + dyn), diffuse.a * vc.a);
    } else if (u.modes.x == 1) {
        color = vec4<f32>(diffuse.rgb * vc.rgb * (u.ambient.rgb + dyn), diffuse.a * vc.a);
    } else {
        color = diffuse * vc;
    }

    if (u.params.y >= 0.0 && color.a < u.params.y) {
        discard;
    }

    if (u.modes.y != 0) {
        var fogFactor = 0.0;
        let toFrag = in.worldPos - u.camPos.xyz;
        if (u.modes.y == 2) {
            // Sky: fade toward the horizon (matches the console backends).
            let dir = normalize(toFrag);
            let t = clamp(dir.y / 0.25, 0.0, 1.0);
            fogFactor = (1.0 - smoothstep(0.0, 1.0, t)) * u.fogColor.a;
        } else {
            let fragDepth = length(toFrag);
            let fogLength = max(u.params.w - u.params.z, 0.0001);
            let fogAlpha  = clamp((fragDepth - u.params.z) / fogLength, 0.0, 1.0) * u.fogColor.a;
            if (u.modes.z != 0) {
                fogFactor = 1.0 - clamp(pow(20.0, -fogAlpha), 0.0, 1.0);
            } else {
                fogFactor = fogAlpha;
            }
        }
        color = vec4<f32>(mix(color.rgb, u.fogColor.rgb, fogFactor), color.a);
    }

    return color;
}
)WGSL";

// ---------------------------------------------------------------------------
// Particle shader (pre-billboarded VertexParticle quads, fan-expanded on CPU)
// ---------------------------------------------------------------------------

static const char* kParticleWGSL = R"WGSL(
struct ParticleUniforms {
    mvp      : mat4x4<f32>,
    model    : mat4x4<f32>,
    modes    : vec4<i32>,    // x fogMode  y fogExponential
    params   : vec4<f32>,    // z fogNear  w fogFar
    fogColor : vec4<f32>,
    camPos   : vec4<f32>,
}

@group(0) @binding(0) var<uniform> u : ParticleUniforms;
@group(1) @binding(0) var uSampler : sampler;
@group(1) @binding(1) var uTexture : texture_2d<f32>;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv       : vec2<f32>,
    @location(1) color    : vec4<f32>,
    @location(2) worldPos : vec3<f32>,
}

@vertex
fn vs_main(@location(0) position : vec3<f32>,
           @location(1) uv       : vec2<f32>,
           @location(2) color    : vec4<f32>) -> VsOut {
    var out : VsOut;
    out.position = u.mvp * vec4<f32>(position, 1.0);
    out.uv       = uv;
    out.color    = color;
    out.worldPos = (u.model * vec4<f32>(position, 1.0)).xyz;
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    var color = in.color * textureSample(uTexture, uSampler, in.uv);

    if (u.modes.x != 0) {
        let fragDepth = length(in.worldPos - u.camPos.xyz);
        let fogLength = max(u.params.w - u.params.z, 0.0001);
        let fogAlpha  = clamp((fragDepth - u.params.z) / fogLength, 0.0, 1.0) * u.fogColor.a;
        var fogFactor = fogAlpha;
        if (u.modes.y != 0) {
            fogFactor = 1.0 - clamp(pow(20.0, -fogAlpha), 0.0, 1.0);
        }
        color = vec4<f32>(mix(color.rgb, u.fogColor.rgb, fogFactor), color.a);
    }

    return color;
}
)WGSL";

// ---------------------------------------------------------------------------
// UI shader (Quad / QuadBorder / Text / Poly — screen-pixel VertexUI)
// ---------------------------------------------------------------------------

static const char* kUiWGSL = R"WGSL(
struct UiUniforms {
    scaleOffset : vec4<f32>,   // xy posScale (Text: scaledTextSize/fontSize), zw posOffset
    screenSize  : vec4<f32>,   // xy screen dimensions in pixels
    tint        : vec4<f32>,
}

@group(0) @binding(0) var<uniform> u : UiUniforms;
@group(1) @binding(0) var uSampler : sampler;
@group(1) @binding(1) var uTexture : texture_2d<f32>;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) uv    : vec2<f32>,
    @location(1) color : vec4<f32>,
}

@vertex
fn vs_main(@location(0) position : vec2<f32>,
           @location(1) uv       : vec2<f32>,
           @location(2) color    : vec4<f32>) -> VsOut {
    var out : VsOut;
    let px  = position * u.scaleOffset.xy + u.scaleOffset.zw;
    // Engine coords are y-down screen pixels; WebGPU NDC is y-up (same as GL).
    let ndc = vec2<f32>(px.x / u.screenSize.x * 2.0 - 1.0,
                        1.0 - px.y / u.screenSize.y * 2.0);
    out.position = vec4<f32>(ndc, 0.0, 1.0);
    out.uv = uv;
    out.color = color;
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    return in.color * u.tint * textureSample(uTexture, uSampler, in.uv);
}
)WGSL";

// ---------------------------------------------------------------------------
// Line shader (debug lines, VertexLine)
// ---------------------------------------------------------------------------

static const char* kLineWGSL = R"WGSL(
struct LineUniforms {
    viewProj : mat4x4<f32>,
}

@group(0) @binding(0) var<uniform> u : LineUniforms;

struct VsOut {
    @builtin(position) position : vec4<f32>,
    @location(0) color : vec4<f32>,
}

@vertex
fn vs_main(@location(0) position : vec3<f32>,
           @location(1) color    : vec4<f32>) -> VsOut {
    var out : VsOut;
    out.position = u.viewProj * vec4<f32>(position, 1.0);
    out.color = color;
    return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4<f32> {
    return in.color;
}
)WGSL";
