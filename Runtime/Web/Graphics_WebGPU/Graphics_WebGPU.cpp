/**
 * @file Graphics_WebGPU.cpp
 * @brief WebGPU (Dawn emdawnwebgpu port via Emscripten) backend for the
 *        engine's GFX_* surface.
 *
 * Structure mirrors Graphics_WebGL2.cpp — same feature matrix, same
 * console-style material model (uniform-driven WGSL ubershaders in
 * WebGPUShaders.h; shader-graph materials degrade to standard shading; the
 * baked material SPIR-V in .oct files is ignored), same CPU skinning.
 *
 * WebGPU-specific architecture:
 *   - DEVICE: acquired asynchronously in shell.html's Module.preRun behind a
 *     run dependency, imported here via emscripten_webgpu_get_device()
 *     (reads Module.preinitializedWebGPUDevice).
 *   - FRAME: one command encoder + ONE render pass for the whole frame
 *     (clear at load, submit in GFX_EndFrame; the browser presents when the
 *     rAF callback returns). If the surface texture can't be acquired the
 *     frame is skipped and the surface reconfigured.
 *   - PIPELINES: immutable state objects, lazily built into a cache keyed by
 *     {program, mesh layout, blend, depthTest, depthWrite, topology}.
 *     Cull is always off (engine content is two-sided-tolerant, matching the
 *     GL backend's glDisable(GL_CULL_FACE)).
 *   - UNIFORMS: wgpuQueueWriteBuffer executes before the frame's submit, so
 *     per-draw uniform REWRITES would all land before any draw ran. Every
 *     draw therefore gets its own 256-aligned slice of a growing ring of
 *     uniform chunks, bound via one dynamic-offset bind group per chunk
 *     (group 0). Group 1 is texture+sampler, cached per texture resource.
 *   - GL idioms emulated: TRIANGLE_FAN -> CPU fan->list expansion;
 *     constant vertex attribute (white) -> arrayStride-0 vertex buffer.
 *   - DEPTH: [0,1] clip range -> perspectiveRH_ZO/orthoRH_ZO. Framebuffer
 *     origin is top-left -> no viewport/scissor y-flips.
 *
 * Resource storage rides the shared POLYPHASE_PLATFORM_ADDON arms in
 * GraphicsTypes.h, treated opaquely:
 *   - TextureResource:        heap WebGpuTexture* in mPixels.
 *   - StaticMeshResource:     heap WebGpuMesh* in mVertexData.
 *   - SkeletalMeshResource:   heap WebGpuIndexBuffer* in mIndexData.
 *   - StaticMeshCompResource: heap WebGpuBuffer* (baked colors) in
 *     mColorVertexData.
 *   - SkeletalMeshCompResource / ParticleCompResource: heap WebGpuDynBuffer*
 *     in mVertexData (capacity fields track bytes as usual).
 *   - Quad/Text/Poly draw through the per-frame transient vertex ring — no
 *     per-widget GPU objects (their addon-arm fields stay null).
 *   - TextMeshCompResource / TileMap / Terrain use backend-side maps.
 *
 * Not implemented (logged no-ops), matching the WebGL2 backend: light bake,
 * path trace, GPU timestamps, post-process chain, hit-check, voxels, shadow
 * meshes, splats. Mipmaps are also not generated (WebGPU has no
 * glGenerateMipmap; a blit-based generator is future work) — textures sample
 * their top level only.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */
#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsConstants.h"
#include "Engine/Maths.h"
#include "Engine/Engine.h"
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Font.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/ShadowMesh3d.h"
#include "Engine/Nodes/3D/Skybox3D.h"
#include "Engine/Nodes/3D/SkeletalMesh3d.h"
#include "Engine/Nodes/3D/InstancedMesh3d.h"
#include "Engine/Nodes/3D/TextMesh3d.h"
#include "Engine/Nodes/3D/TileMap2d.h"
#include "Engine/Nodes/3D/Terrain3d.h"
#include "Engine/Nodes/3D/Particle3d.h"
#include "Engine/Nodes/3D/Camera3d.h"
#include "Engine/Nodes/Widgets/Widget.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"
#include "Log.h"

#include <emscripten.h>
#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "WebGPUShaders.h"

// The preferred canvas format is stashed on Module by shell.html's preRun
// (navigator.gpu.getPreferredCanvasFormat()). The C API's
// wgpuSurfaceGetCapabilities needs a WGPUAdapter, which the
// preinitialized-device path never surfaces to wasm — so ask JS directly.
EM_JS(int, PolyGpuPreferredFormatIsBGRA, (), {
    return (Module.polyphasePreferredFormat === "rgba8unorm") ? 0 : 1;
});

// ===========================================================================
// State
// ===========================================================================

namespace
{
    // ----- Core objects ----------------------------------------------------
    WGPUInstance sInstance = nullptr;
    WGPUDevice   sDevice   = nullptr;
    WGPUQueue    sQueue    = nullptr;
    WGPUSurface  sSurface  = nullptr;
    WGPUTextureFormat sSurfaceFormat = WGPUTextureFormat_BGRA8Unorm;

    WGPUTexture     sDepthTexture = nullptr;
    WGPUTextureView sDepthView    = nullptr;
    uint32_t sSurfaceW = 0, sSurfaceH = 0;

    bool sInitialised = false;

    // ----- Frame state -----------------------------------------------------
    WGPUCommandEncoder    sEncoder     = nullptr;
    WGPURenderPassEncoder sPass        = nullptr;
    WGPUTexture           sFrameTex    = nullptr;
    WGPUTextureView       sFrameView   = nullptr;
    bool sFrameActive = false;

    bool sInForwardPass = false;
    bool sInUiPass      = false;
    bool sInShadowPass  = false;
    bool sMaterialsEnabled = true;

    // ----- Shader modules / layouts ---------------------------------------
    enum Program : uint32_t
    {
        PROG_MESH = 0,
        PROG_PARTICLE,
        PROG_UI,
        PROG_LINE,
        PROG_COUNT
    };

    WGPUShaderModule sModules[PROG_COUNT] = {};

    WGPUBindGroupLayout sUniformBGL = nullptr;   // group 0: dynamic uniform slice
    WGPUBindGroupLayout sTextureBGL = nullptr;   // group 1: sampler + texture
    WGPUPipelineLayout  sLayoutWithTex = nullptr;   // mesh/particle/ui
    WGPUPipelineLayout  sLayoutNoTex   = nullptr;   // line

    // Mesh vertex-layout variants (see the pipeline cache key). The GL
    // backend mutated attrib 4 per draw; WebGPU bakes the choice into the
    // pipeline instead.
    enum MeshLayout : uint32_t
    {
        ML_INLINE_COLOR_44 = 0,   // VertexColor stream, color inline at offset 40
        ML_NO_COLOR_40     = 1,   // Vertex stream + stride-0 white buffer on slot 1
        ML_BAKED_40        = 2,   // Vertex stream + per-component baked colors on slot 1
        ML_BAKED_44        = 3,   // VertexColor stream (color ignored) + baked colors
        ML_COUNT
    };

    enum BlendKey : uint32_t
    {
        BK_OPAQUE = 0,      // also Masked (cutoff is shader-side)
        BK_TRANSLUCENT,
        BK_ADDITIVE,
    };

    // Packed pipeline key: program(2) | meshLayout(2) | blend(2) |
    // depthTest(1) | depthWrite(1) | lineTopology(1)
    uint32_t MakePipelineKey(Program prog, MeshLayout layout, BlendKey blend,
                             bool depthTest, bool depthWrite, bool lines)
    {
        return (uint32_t)prog | ((uint32_t)layout << 2) | ((uint32_t)blend << 4) |
               ((depthTest ? 1u : 0u) << 6) | ((depthWrite ? 1u : 0u) << 7) |
               ((lines ? 1u : 0u) << 8);
    }

    std::unordered_map<uint32_t, WGPURenderPipeline> sPipelines;

    // ----- Uniform ring ----------------------------------------------------
    // One dynamic-offset bind group per chunk. Slices are 256-aligned; the
    // bind-group binding size is the largest shader struct (rounded up), so
    // allocation always leaves that much headroom.
    constexpr uint32_t kUniformChunkSize   = 256 * 1024;
    constexpr uint32_t kUniformSliceAlign  = 256;
    constexpr uint32_t kUniformBindingSize = 768;   // >= sizeof(MeshUniformsCPU)

    struct UniformChunk
    {
        WGPUBuffer    buffer = nullptr;
        WGPUBindGroup bindGroup = nullptr;
    };
    std::vector<UniformChunk> sUniformChunks;
    uint32_t sUniformChunkIdx = 0;
    uint32_t sUniformOffset   = 0;

    // ----- Transient vertex ring (UI, lines, fan expansion) ---------------
    constexpr uint32_t kTransientChunkSize = 512 * 1024;

    struct TransientChunk
    {
        WGPUBuffer buffer = nullptr;
        uint32_t   size   = 0;
    };
    std::vector<TransientChunk> sTransientChunks;
    uint32_t sTransientChunkIdx = 0;
    uint32_t sTransientOffset   = 0;

    // ----- Samplers / fallback texture ------------------------------------
    WGPUSampler sSamplers[2][3] = {};      // [linear][wrap: repeat/clamp/mirror]
    WGPUTexture     sWhiteTex = nullptr;
    WGPUTextureView sWhiteView = nullptr;
    WGPUBindGroup   sWhiteBindGroup = nullptr;
    WGPUBuffer      sWhiteVertexBuffer = nullptr;   // 4 bytes 0xFFFFFFFF, stride-0 slot

    // ----- Resource wrappers ----------------------------------------------
    struct WebGpuTexture
    {
        WGPUTexture     tex = nullptr;
        WGPUTextureView view = nullptr;
        WGPUBindGroup   bindGroup = nullptr;
    };

    struct WebGpuBuffer
    {
        WGPUBuffer buffer = nullptr;
        uint32_t   capacityBytes = 0;
    };

    struct WebGpuMesh
    {
        WGPUBuffer vertexBuffer = nullptr;
        WGPUBuffer indexBuffer = nullptr;
        uint32_t numIndices = 0;
        bool hasColor = false;
    };

    struct WebGpuIndexBuffer
    {
        WGPUBuffer buffer = nullptr;
        uint32_t numIndices = 0;
    };

    // Dynamic vertex buffer (CPU-skinned skeletal comps, particles, text
    // meshes). Grown on demand; rewritten at most once per frame per
    // resource, so a plain writeBuffer is hazard-free.
    struct WebGpuDynBuffer
    {
        WGPUBuffer buffer = nullptr;
        uint32_t capacityBytes = 0;
        uint32_t numVerts = 0;
    };

    // TextMesh / TileMap / Terrain have no addon-arm storage — side tables.
    std::unordered_map<TextMesh3D*, WebGpuDynBuffer> sTextMeshBuffers;

    struct WebGpuIndexedColorMesh
    {
        WebGpuBuffer vertexBuffer;
        WebGpuBuffer indexBuffer;
        uint32_t numIndices = 0;
    };
    std::unordered_map<TileMap2D*, WebGpuIndexedColorMesh> sTileMapBuffers;
    std::unordered_map<Terrain3D*, WebGpuIndexedColorMesh> sTerrainBuffers;

    // ----- Scene lighting / fog (cached per Forward pass) ------------------
    constexpr int kMaxPointLights = 8;
    glm::vec3 sAmbient(0.35f);
    bool      sHasDirLight = false;
    glm::vec3 sLightDir(0.35f, 0.75f, 0.55f);
    glm::vec3 sLightColor(1.0f);
    int       sNumPointLights = 0;
    glm::vec4 sPointLightPosRadius[kMaxPointLights];
    glm::vec3 sPointLightColor[kMaxPointLights];
    float     sColorScale = 1.0f;

    FogSettings sFog;

    enum WebFogMode { WEB_FOG_OFF = 0, WEB_FOG_DISTANCE = 1, WEB_FOG_SKY = 2 };

    // ===================================================================
    // CPU mirrors of the WGSL uniform structs (WebGPUShaders.h).
    // vec4/mat4 members only, so layouts match byte for byte.
    // ===================================================================

    struct MeshUniformsCPU
    {
        glm::mat4 mvp;
        glm::mat4 model;
        glm::vec4 normal0, normal1, normal2;
        glm::vec4 baseColor;
        int32_t   modes[4];      // lightMode, fogMode, fogExponential, numPointLights
        glm::vec4 params;        // vcScale, alphaCutoff, fogNear, fogFar
        glm::vec4 ambient;
        glm::vec4 dirDir;        // xyz dir, w hasDirLight
        glm::vec4 dirColor;
        glm::vec4 fogColor;
        glm::vec4 camPos;
        glm::vec4 pointPosRadius[8];
        glm::vec4 pointColor[8];
    };
    static_assert(sizeof(MeshUniformsCPU) == 560, "must match WGSL MeshUniforms");
    static_assert(offsetof(MeshUniformsCPU, modes) == 192, "layout drift");
    static_assert(offsetof(MeshUniformsCPU, pointPosRadius) == 304, "layout drift");
    static_assert(sizeof(MeshUniformsCPU) <= kUniformBindingSize, "binding too small");

    struct ParticleUniformsCPU
    {
        glm::mat4 mvp;
        glm::mat4 model;
        int32_t   modes[4];      // fogMode, fogExponential
        glm::vec4 params;        // z fogNear, w fogFar
        glm::vec4 fogColor;
        glm::vec4 camPos;
    };
    static_assert(sizeof(ParticleUniformsCPU) == 192, "must match WGSL ParticleUniforms");

    struct UiUniformsCPU
    {
        glm::vec4 scaleOffset;   // xy posScale, zw posOffset
        glm::vec4 screenSize;
        glm::vec4 tint;
    };
    static_assert(sizeof(UiUniformsCPU) == 48, "must match WGSL UiUniforms");

    struct LineUniformsCPU
    {
        glm::mat4 viewProj;
    };
    static_assert(sizeof(LineUniformsCPU) == 64, "must match WGSL LineUniforms");

    // ===================================================================
    // Small helpers
    // ===================================================================

    WGPUStringView Sv(const char* s)
    {
        WGPUStringView v;
        v.data = s;
        v.length = s ? std::strlen(s) : 0;
        return v;
    }

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    WGPUBuffer CreateBuffer(uint64_t size, WGPUBufferUsage usage, const char* label)
    {
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.label = Sv(label);
        desc.usage = usage;
        desc.size  = size;
        return wgpuDeviceCreateBuffer(sDevice, &desc);
    }

    // wgpuQueueWriteBuffer requires a size that is a multiple of 4; pad via a
    // scratch copy when the payload isn't (odd uint16 index counts).
    void WriteBufferPadded(WGPUBuffer buffer, uint64_t offset, const void* data, size_t bytes)
    {
        if (bytes == 0) return;
        if ((bytes & 3u) == 0)
        {
            wgpuQueueWriteBuffer(sQueue, buffer, offset, data, bytes);
            return;
        }
        static std::vector<uint8_t> sScratch;
        const size_t padded = (bytes + 3u) & ~size_t(3u);
        sScratch.resize(padded, 0);
        std::memcpy(sScratch.data(), data, bytes);
        wgpuQueueWriteBuffer(sQueue, buffer, offset, sScratch.data(), padded);
    }

    void ReleaseBuffer(WGPUBuffer& b)
    {
        if (b) { wgpuBufferDestroy(b); wgpuBufferRelease(b); b = nullptr; }
    }

    // ----- Uniform ring ----------------------------------------------------

    void EnsureUniformChunk(uint32_t idx)
    {
        while (sUniformChunks.size() <= idx)
        {
            UniformChunk c;
            c.buffer = CreateBuffer(kUniformChunkSize,
                                    WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                                    "PolyUniformChunk");

            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = 0;
            entry.buffer  = c.buffer;
            entry.offset  = 0;
            entry.size    = kUniformBindingSize;

            WGPUBindGroupDescriptor bgDesc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bgDesc.label      = Sv("PolyUniformBG");
            bgDesc.layout     = sUniformBGL;
            bgDesc.entryCount = 1;
            bgDesc.entries    = &entry;
            c.bindGroup = wgpuDeviceCreateBindGroup(sDevice, &bgDesc);

            sUniformChunks.push_back(c);
        }
    }

    // Write a per-draw uniform block and bind it at group 0.
    void BindUniforms(const void* data, uint32_t bytes)
    {
        const uint32_t slice = AlignUp(bytes, kUniformSliceAlign);
        if (sUniformOffset + kUniformBindingSize > kUniformChunkSize)
        {
            ++sUniformChunkIdx;
            sUniformOffset = 0;
        }
        EnsureUniformChunk(sUniformChunkIdx);

        UniformChunk& chunk = sUniformChunks[sUniformChunkIdx];
        wgpuQueueWriteBuffer(sQueue, chunk.buffer, sUniformOffset, data, AlignUp(bytes, 4));

        const uint32_t dynOffset = sUniformOffset;
        wgpuRenderPassEncoderSetBindGroup(sPass, 0, chunk.bindGroup, 1, &dynOffset);
        sUniformOffset += slice;
    }

    // ----- Transient vertex ring -------------------------------------------

    void EnsureTransientChunk(uint32_t idx, uint32_t minSize)
    {
        while (sTransientChunks.size() <= idx)
        {
            TransientChunk c;
            c.size = (minSize > kTransientChunkSize) ? AlignUp(minSize, 4) : kTransientChunkSize;
            c.buffer = CreateBuffer(c.size,
                                    WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                    "PolyTransientVB");
            sTransientChunks.push_back(c);
        }
    }

    // Upload transient vertex data; returns the buffer + byte offset to bind.
    bool PushTransientVerts(const void* data, uint32_t bytes,
                            WGPUBuffer& outBuffer, uint32_t& outOffset)
    {
        if (bytes == 0) return false;
        const uint32_t padded = AlignUp(bytes, 4);

        EnsureTransientChunk(sTransientChunkIdx, padded);
        if (sTransientOffset + padded > sTransientChunks[sTransientChunkIdx].size)
        {
            ++sTransientChunkIdx;
            sTransientOffset = 0;
            EnsureTransientChunk(sTransientChunkIdx, padded);
        }

        TransientChunk& chunk = sTransientChunks[sTransientChunkIdx];
        WriteBufferPadded(chunk.buffer, sTransientOffset, data, bytes);
        outBuffer = chunk.buffer;
        outOffset = sTransientOffset;
        sTransientOffset += padded;
        return true;
    }

    // ----- Samplers / textures ---------------------------------------------

    WGPUSampler GetSampler(bool linear, WrapMode wrapMode)
    {
        int wrapIdx = 0;
        WGPUAddressMode mode = WGPUAddressMode_Repeat;
        switch (wrapMode)
        {
            case WrapMode::Clamp:  wrapIdx = 1; mode = WGPUAddressMode_ClampToEdge;  break;
            case WrapMode::Mirror: wrapIdx = 2; mode = WGPUAddressMode_MirrorRepeat; break;
            default:               wrapIdx = 0; mode = WGPUAddressMode_Repeat;       break;
        }

        WGPUSampler& s = sSamplers[linear ? 1 : 0][wrapIdx];
        if (s == nullptr)
        {
            WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
            desc.label = Sv("PolySampler");
            desc.addressModeU = mode;
            desc.addressModeV = mode;
            desc.addressModeW = mode;
            desc.magFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            desc.minFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;   // single mip level
            desc.maxAnisotropy = 1;
            s = wgpuDeviceCreateSampler(sDevice, &desc);
        }
        return s;
    }

    WGPUBindGroup MakeTextureBindGroup(WGPUTextureView view, WGPUSampler sampler)
    {
        WGPUBindGroupEntry entries[2];
        entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[0].binding = 0;
        entries[0].sampler = sampler;
        entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
        entries[1].binding = 1;
        entries[1].textureView = view;

        WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        desc.label      = Sv("PolyTextureBG");
        desc.layout     = sTextureBGL;
        desc.entryCount = 2;
        desc.entries    = entries;
        return wgpuDeviceCreateBindGroup(sDevice, &desc);
    }

    WebGpuTexture* GetWebTexture(Texture* tex)
    {
        if (tex == nullptr) return nullptr;
        TextureResource* r = tex->GetResource();
        return r ? static_cast<WebGpuTexture*>(r->mPixels) : nullptr;
    }

    // Bind group 1: the texture's own group, or the white fallback. The
    // shader samples unconditionally (see WebGPUShaders.h).
    void BindTextureOrWhite(Texture* tex)
    {
        WebGpuTexture* wt = GetWebTexture(tex);
        wgpuRenderPassEncoderSetBindGroup(
            sPass, 1, (wt && wt->bindGroup) ? wt->bindGroup : sWhiteBindGroup, 0, nullptr);
    }

    // ----- Pipeline cache --------------------------------------------------

    BlendKey BlendKeyFromMode(BlendMode mode)
    {
        switch (mode)
        {
            case BlendMode::Translucent: return BK_TRANSLUCENT;
            case BlendMode::Additive:    return BK_ADDITIVE;
            default:                     return BK_OPAQUE;   // Opaque / Masked
        }
    }

    WGPURenderPipeline BuildPipeline(Program prog, MeshLayout layout, BlendKey blend,
                                     bool depthTest, bool depthWrite, bool lines)
    {
        // --- Vertex layouts ------------------------------------------------
        WGPUVertexAttribute meshAttrs[4];
        WGPUVertexAttribute colorAttr = WGPU_VERTEX_ATTRIBUTE_INIT;
        WGPUVertexBufferLayout buffers[2];
        size_t bufferCount = 0;

        auto attr = [](WGPUVertexFormat fmt, uint64_t off, uint32_t loc) {
            WGPUVertexAttribute a = WGPU_VERTEX_ATTRIBUTE_INIT;
            a.format = fmt; a.offset = off; a.shaderLocation = loc;
            return a;
        };

        WGPUVertexAttribute streamAttrs[3];

        if (prog == PROG_MESH)
        {
            meshAttrs[0] = attr(WGPUVertexFormat_Float32x3, offsetof(Vertex, mPosition),  0);
            meshAttrs[1] = attr(WGPUVertexFormat_Float32x2, offsetof(Vertex, mTexcoord0), 1);
            meshAttrs[2] = attr(WGPUVertexFormat_Float32x2, offsetof(Vertex, mTexcoord1), 2);
            meshAttrs[3] = attr(WGPUVertexFormat_Float32x3, offsetof(Vertex, mNormal),    3);

            const bool inlineColor = (layout == ML_INLINE_COLOR_44);
            const uint64_t stride0 = (layout == ML_NO_COLOR_40 || layout == ML_BAKED_40)
                                         ? sizeof(Vertex) : sizeof(VertexColor);

            buffers[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffers[0].stepMode = WGPUVertexStepMode_Vertex;
            buffers[0].arrayStride = stride0;

            if (inlineColor)
            {
                static WGPUVertexAttribute sInlineAttrs[5];
                for (int i = 0; i < 4; ++i) sInlineAttrs[i] = meshAttrs[i];
                sInlineAttrs[4] = attr(WGPUVertexFormat_Unorm8x4, offsetof(VertexColor, mColor), 4);
                buffers[0].attributeCount = 5;
                buffers[0].attributes = sInlineAttrs;
                bufferCount = 1;
            }
            else
            {
                buffers[0].attributeCount = 4;
                buffers[0].attributes = meshAttrs;

                colorAttr = attr(WGPUVertexFormat_Unorm8x4, 0, 4);
                buffers[1] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
                buffers[1].stepMode = WGPUVertexStepMode_Vertex;
                // Stride 0 = constant attribute (the GL glVertexAttrib4f
                // idiom); stride 4 = per-vertex baked colors.
                buffers[1].arrayStride = (layout == ML_NO_COLOR_40) ? 0 : 4;
                buffers[1].attributeCount = 1;
                buffers[1].attributes = &colorAttr;
                bufferCount = 2;
            }
        }
        else if (prog == PROG_PARTICLE)
        {
            streamAttrs[0] = attr(WGPUVertexFormat_Float32x3, offsetof(VertexParticle, mPosition), 0);
            streamAttrs[1] = attr(WGPUVertexFormat_Float32x2, offsetof(VertexParticle, mTexcoord), 1);
            streamAttrs[2] = attr(WGPUVertexFormat_Unorm8x4,  offsetof(VertexParticle, mColor),    2);
            buffers[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffers[0].stepMode = WGPUVertexStepMode_Vertex;
            buffers[0].arrayStride = sizeof(VertexParticle);
            buffers[0].attributeCount = 3;
            buffers[0].attributes = streamAttrs;
            bufferCount = 1;
        }
        else if (prog == PROG_UI)
        {
            streamAttrs[0] = attr(WGPUVertexFormat_Float32x2, offsetof(VertexUI, mPosition), 0);
            streamAttrs[1] = attr(WGPUVertexFormat_Float32x2, offsetof(VertexUI, mTexcoord), 1);
            streamAttrs[2] = attr(WGPUVertexFormat_Unorm8x4,  offsetof(VertexUI, mColor),    2);
            buffers[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffers[0].stepMode = WGPUVertexStepMode_Vertex;
            buffers[0].arrayStride = sizeof(VertexUI);
            buffers[0].attributeCount = 3;
            buffers[0].attributes = streamAttrs;
            bufferCount = 1;
        }
        else   // PROG_LINE
        {
            streamAttrs[0] = attr(WGPUVertexFormat_Float32x3, offsetof(VertexLine, mPosition), 0);
            streamAttrs[1] = attr(WGPUVertexFormat_Unorm8x4,  offsetof(VertexLine, mColor),    1);
            buffers[0] = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            buffers[0].stepMode = WGPUVertexStepMode_Vertex;
            buffers[0].arrayStride = sizeof(VertexLine);
            buffers[0].attributeCount = 2;
            buffers[0].attributes = streamAttrs;
            bufferCount = 1;
        }

        // --- Blend ---------------------------------------------------------
        WGPUBlendState blendState = WGPU_BLEND_STATE_INIT;
        const WGPUBlendState* blendPtr = nullptr;
        if (blend != BK_OPAQUE)
        {
            blendState.color.operation = WGPUBlendOperation_Add;
            blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
            blendState.color.dstFactor = (blend == BK_ADDITIVE)
                                             ? WGPUBlendFactor_One
                                             : WGPUBlendFactor_OneMinusSrcAlpha;
            blendState.alpha = blendState.color;
            blendPtr = &blendState;
        }

        WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
        colorTarget.format = sSurfaceFormat;
        colorTarget.blend = blendPtr;
        colorTarget.writeMask = WGPUColorWriteMask_All;

        // --- Depth ---------------------------------------------------------
        WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
        depth.format = WGPUTextureFormat_Depth24Plus;
        depth.depthWriteEnabled = depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depth.depthCompare = depthTest ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Always;

        // --- Stages --------------------------------------------------------
        WGPUVertexState vs = WGPU_VERTEX_STATE_INIT;
        vs.module = sModules[prog];
        vs.entryPoint = Sv("vs_main");
        vs.bufferCount = bufferCount;
        vs.buffers = buffers;

        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = sModules[prog];
        fs.entryPoint = Sv("fs_main");
        fs.targetCount = 1;
        fs.targets = &colorTarget;

        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = Sv("PolyPipeline");
        desc.layout = (prog == PROG_LINE) ? sLayoutNoTex : sLayoutWithTex;
        desc.vertex = vs;
        desc.primitive.topology = lines ? WGPUPrimitiveTopology_LineList
                                        : WGPUPrimitiveTopology_TriangleList;
        desc.primitive.frontFace = WGPUFrontFace_CCW;
        desc.primitive.cullMode = WGPUCullMode_None;   // engine content is two-sided-tolerant
        desc.depthStencil = &depth;
        desc.multisample.count = 1;
        desc.multisample.mask = 0xFFFFFFFFu;
        desc.fragment = &fs;

        return wgpuDeviceCreateRenderPipeline(sDevice, &desc);
    }

    void SetPipeline(Program prog, MeshLayout layout, BlendKey blend,
                     bool depthTest, bool depthWrite, bool lines = false)
    {
        const uint32_t key = MakePipelineKey(prog, layout, blend, depthTest, depthWrite, lines);
        auto it = sPipelines.find(key);
        if (it == sPipelines.end())
        {
            WGPURenderPipeline pipe = BuildPipeline(prog, layout, blend, depthTest, depthWrite, lines);
            it = sPipelines.emplace(key, pipe).first;
        }
        wgpuRenderPassEncoderSetPipeline(sPass, it->second);
    }

    // ----- Setup -----------------------------------------------------------

    bool BuildModulesAndLayouts()
    {
        auto makeModule = [](const char* src, const char* label) -> WGPUShaderModule {
            WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code = Sv(src);
            WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            desc.nextInChain = &wgsl.chain;
            desc.label = Sv(label);
            return wgpuDeviceCreateShaderModule(sDevice, &desc);
        };

        sModules[PROG_MESH]     = makeModule(kMeshWGSL, "mesh");
        sModules[PROG_PARTICLE] = makeModule(kParticleWGSL, "particle");
        sModules[PROG_UI]       = makeModule(kUiWGSL, "ui");
        sModules[PROG_LINE]     = makeModule(kLineWGSL, "line");
        for (int i = 0; i < PROG_COUNT; ++i)
        {
            if (sModules[i] == nullptr)
            {
                LogError("Graphics_WebGPU: shader module %d creation failed", i);
                return false;
            }
        }

        // Group 0: one dynamic-offset uniform slice, shared by every program.
        {
            WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entry.binding = 0;
            entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entry.buffer.type = WGPUBufferBindingType_Uniform;
            entry.buffer.hasDynamicOffset = WGPU_TRUE;
            entry.buffer.minBindingSize = 0;

            WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            desc.label = Sv("PolyUniformBGL");
            desc.entryCount = 1;
            desc.entries = &entry;
            sUniformBGL = wgpuDeviceCreateBindGroupLayout(sDevice, &desc);
        }

        // Group 1: sampler + texture.
        {
            WGPUBindGroupLayoutEntry entries[2];
            entries[0] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Fragment;
            entries[0].sampler.type = WGPUSamplerBindingType_Filtering;
            entries[1] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entries[1].binding = 1;
            entries[1].visibility = WGPUShaderStage_Fragment;
            entries[1].texture.sampleType = WGPUTextureSampleType_Float;
            entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

            WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            desc.label = Sv("PolyTextureBGL");
            desc.entryCount = 2;
            desc.entries = entries;
            sTextureBGL = wgpuDeviceCreateBindGroupLayout(sDevice, &desc);
        }

        {
            WGPUBindGroupLayout bgls[2] = { sUniformBGL, sTextureBGL };
            WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            desc.label = Sv("PolyLayoutWithTex");
            desc.bindGroupLayoutCount = 2;
            desc.bindGroupLayouts = bgls;
            sLayoutWithTex = wgpuDeviceCreatePipelineLayout(sDevice, &desc);

            desc.label = Sv("PolyLayoutNoTex");
            desc.bindGroupLayoutCount = 1;
            sLayoutNoTex = wgpuDeviceCreatePipelineLayout(sDevice, &desc);
        }

        return sUniformBGL && sTextureBGL && sLayoutWithTex && sLayoutNoTex;
    }

    WGPUTexture CreateTexture2D(uint32_t w, uint32_t h, const char* label)
    {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = Sv(label);
        desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        desc.dimension = WGPUTextureDimension_2D;
        desc.size.width = w;
        desc.size.height = h;
        desc.size.depthOrArrayLayers = 1;
        desc.format = WGPUTextureFormat_RGBA8Unorm;
        desc.mipLevelCount = 1;
        desc.sampleCount = 1;
        return wgpuDeviceCreateTexture(sDevice, &desc);
    }

    void UploadTexturePixels(WGPUTexture tex, uint32_t w, uint32_t h, const uint8_t* rgba)
    {
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = tex;
        dst.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.offset = 0;
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;

        WGPUExtent3D extent;
        extent.width = w;
        extent.height = h;
        extent.depthOrArrayLayers = 1;

        wgpuQueueWriteTexture(sQueue, &dst, rgba, (size_t)w * h * 4, &layout, &extent);
    }

    void RecreateDepthTexture(uint32_t w, uint32_t h)
    {
        if (sDepthView) { wgpuTextureViewRelease(sDepthView); sDepthView = nullptr; }
        if (sDepthTexture) { wgpuTextureDestroy(sDepthTexture); wgpuTextureRelease(sDepthTexture); sDepthTexture = nullptr; }

        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.label = Sv("PolyDepth");
        desc.usage = WGPUTextureUsage_RenderAttachment;
        desc.dimension = WGPUTextureDimension_2D;
        desc.size.width = w;
        desc.size.height = h;
        desc.size.depthOrArrayLayers = 1;
        desc.format = WGPUTextureFormat_Depth24Plus;
        desc.mipLevelCount = 1;
        desc.sampleCount = 1;
        sDepthTexture = wgpuDeviceCreateTexture(sDevice, &desc);
        sDepthView = wgpuTextureCreateView(sDepthTexture, nullptr);
    }

    void ConfigureSurface(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return;

        WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
        cfg.device = sDevice;
        cfg.format = sSurfaceFormat;
        cfg.usage = WGPUTextureUsage_RenderAttachment;
        cfg.width = w;
        cfg.height = h;
        cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
        cfg.presentMode = WGPUPresentMode_Fifo;
        wgpuSurfaceConfigure(sSurface, &cfg);

        sSurfaceW = w;
        sSurfaceH = h;
        RecreateDepthTexture(w, h);
    }

    // ----- Growable dyn buffers -------------------------------------------

    WebGpuDynBuffer* GetOrCreateDynBuffer(void*& slot)
    {
        if (slot == nullptr) slot = new WebGpuDynBuffer();
        return static_cast<WebGpuDynBuffer*>(slot);
    }

    void DestroyDynBuffer(void*& slot)
    {
        WebGpuDynBuffer* b = static_cast<WebGpuDynBuffer*>(slot);
        if (b == nullptr) return;
        ReleaseBuffer(b->buffer);
        delete b;
        slot = nullptr;
    }

    // Grow-or-write. Safe without orphaning: the buffer is written at most
    // once per frame, and writeBuffer/submit queue ordering isolates frames.
    void UploadDynVerts(WebGpuDynBuffer* b, const void* data, uint32_t bytes)
    {
        if (bytes > b->capacityBytes)
        {
            ReleaseBuffer(b->buffer);
            const uint32_t newCap = AlignUp(bytes + bytes / 2, 256);
            b->buffer = CreateBuffer(newCap,
                                     WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                     "PolyDynVB");
            b->capacityBytes = newCap;
        }
        WriteBufferPadded(b->buffer, 0, data, bytes);
    }

    void UploadGrowableBuffer(WebGpuBuffer& b, const void* data, uint32_t bytes,
                              WGPUBufferUsage usage, const char* label)
    {
        if (bytes > b.capacityBytes)
        {
            ReleaseBuffer(b.buffer);
            const uint32_t newCap = AlignUp(bytes + bytes / 2, 256);
            b.buffer = CreateBuffer(newCap, usage | WGPUBufferUsage_CopyDst, label);
            b.capacityBytes = newCap;
        }
        WriteBufferPadded(b.buffer, 0, data, bytes);
    }

    // ===================================================================
    // Shared mesh-draw core (DrawMeshCommon parity with Graphics_WebGL2)
    // ===================================================================

    void FillLighting(MeshUniformsCPU& u)
    {
        u.ambient = glm::vec4(sAmbient, 0.0f);
        u.dirDir = glm::vec4(sLightDir, sHasDirLight ? 1.0f : 0.0f);
        u.dirColor = glm::vec4(sLightColor, 0.0f);
        u.modes[3] = sNumPointLights;
        for (int i = 0; i < sNumPointLights; ++i)
        {
            u.pointPosRadius[i] = sPointLightPosRadius[i];
            u.pointColor[i] = glm::vec4(sPointLightColor[i], 0.0f);
        }
    }

    void FillFog(MeshUniformsCPU& u, int fogMode, const glm::vec3& camPos)
    {
        u.modes[1] = fogMode;
        u.modes[2] = (sFog.mDensityFunc == FogDensityFunc::Exponential) ? 1 : 0;
        u.params.z = sFog.mNear;
        u.params.w = sFog.mFar;
        u.fogColor = sFog.mColor;
        u.camPos = glm::vec4(camPos, 1.0f);
    }

    // vertexBuffer: slot-0 stream. colorMesh: mesh whose inline colors feed
    // attribute 4 (null for dynamic Vertex-only buffers). bakedColors:
    // per-component baked-lighting buffer; when set, lighting switches to
    // baked mode (Vulkan Forward.frag parity).
    void DrawMeshCommon(WGPUBuffer vertexBuffer, WGPUBuffer indexBuffer, uint32_t numIndices,
                        uint32_t numVerts, bool indexed, const glm::mat4& model,
                        Material* material, glm::vec4 colorMul, bool isSky,
                        const WebGpuMesh* colorMesh = nullptr,
                        WGPUBuffer bakedColors = nullptr)
    {
        World* world = Renderer::Get()->GetCurrentWorld();
        Camera3D* cam = world ? world->GetActiveCamera() : nullptr;
        if (cam == nullptr) return;

        const glm::mat4 mvp = cam->GetViewProjectionMatrix() * model;
        const glm::mat3 nrm = glm::transpose(glm::inverse(glm::mat3(model)));
        const glm::vec3 camPos = cam->GetWorldPosition();

        MaterialLite* lite = sMaterialsEnabled ? Material::AsLite(material) : nullptr;

        glm::vec4  base  = (lite ? lite->GetColor() : glm::vec4(1.0f)) * colorMul;
        const bool unlit = lite ? (lite->GetShadingModel() == ShadingModel::Unlit) : false;
        BlendMode  blend = lite ? lite->GetBlendMode() : BlendMode::Opaque;
        if (blend == BlendMode::Translucent || blend == BlendMode::Additive)
        {
            base.a *= lite ? lite->GetOpacity() : 1.0f;
        }
        const float cutoff = (blend == BlendMode::Masked)
                                 ? (lite ? lite->GetMaskCutoff() : 0.5f) : -1.0f;

        // Sky detection: node type (caller) or material signature (see PVR2).
        if (!isSky && lite)
            isSky = (unlit && lite->IsDepthTestDisabled() && lite->GetSortPriority() < 0);

        int fogMode = WEB_FOG_OFF;
        if (sFog.mEnabled && (material == nullptr || material->ShouldApplyFog()))
            fogMode = isSky ? WEB_FOG_SKY : WEB_FOG_DISTANCE;

        const bool depthTest = !(lite && lite->IsDepthTestDisabled());

        const int lightMode = unlit ? 0 : ((bakedColors != nullptr) ? 2 : 1);
        const bool hasColorStream = (bakedColors != nullptr) ||
                                    (colorMesh != nullptr && colorMesh->hasColor);
        const float vcScale = hasColorStream ? sColorScale : 1.0f;

        // --- Uniform block -------------------------------------------------
        MeshUniformsCPU u = {};
        u.mvp = mvp;
        u.model = model;
        u.normal0 = glm::vec4(nrm[0], 0.0f);
        u.normal1 = glm::vec4(nrm[1], 0.0f);
        u.normal2 = glm::vec4(nrm[2], 0.0f);
        u.baseColor = base;
        u.modes[0] = lightMode;
        u.params.x = vcScale;
        u.params.y = cutoff;
        FillLighting(u);
        FillFog(u, fogMode, camPos);

        // --- Pipeline ------------------------------------------------------
        MeshLayout layout;
        WGPUBuffer slot1 = nullptr;
        if (bakedColors != nullptr)
        {
            layout = (colorMesh && colorMesh->hasColor) ? ML_BAKED_44 : ML_BAKED_40;
            slot1 = bakedColors;
        }
        else if (colorMesh != nullptr && colorMesh->hasColor)
        {
            layout = ML_INLINE_COLOR_44;
        }
        else
        {
            layout = ML_NO_COLOR_40;
            slot1 = sWhiteVertexBuffer;
        }

        const BlendKey bk = BlendKeyFromMode(blend);
        const bool depthWrite = (bk == BK_OPAQUE) && !isSky;   // sky never writes depth

        SetPipeline(PROG_MESH, layout, bk, depthTest, depthWrite);
        BindUniforms(&u, sizeof(u));
        BindTextureOrWhite(lite ? lite->GetTexture(0) : nullptr);

        wgpuRenderPassEncoderSetVertexBuffer(sPass, 0, vertexBuffer, 0, WGPU_WHOLE_SIZE);
        if (layout != ML_INLINE_COLOR_44)
        {
            wgpuRenderPassEncoderSetVertexBuffer(sPass, 1, slot1, 0, WGPU_WHOLE_SIZE);
        }

        if (indexed)
        {
            wgpuRenderPassEncoderSetIndexBuffer(sPass, indexBuffer, WGPUIndexFormat_Uint16,
                                                0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDrawIndexed(sPass, numIndices, 1, 0, 0, 0);
        }
        else
        {
            wgpuRenderPassEncoderDraw(sPass, numVerts, 1, 0, 0);
        }
    }

    // Shared UI submit: streams VertexUI through the transient ring.
    // Fans are expanded to triangle lists (WebGPU has no TRIANGLE_FAN).
    void SubmitUI(const VertexUI* verts, uint32_t n, Texture* tex, glm::vec4 tint,
                  bool fan, glm::vec2 posScale, glm::vec2 posOffset)
    {
        if (!sFrameActive || verts == nullptr || n < 3 || tint.a <= 0.0f) return;

        const VertexUI* drawVerts = verts;
        uint32_t drawCount = n;
        static std::vector<VertexUI> sFanScratch;
        if (fan && n > 3)
        {
            sFanScratch.clear();
            sFanScratch.reserve((size_t)(n - 2) * 3);
            for (uint32_t i = 1; i + 1 < n; ++i)
            {
                sFanScratch.push_back(verts[0]);
                sFanScratch.push_back(verts[i]);
                sFanScratch.push_back(verts[i + 1]);
            }
            drawVerts = sFanScratch.data();
            drawCount = (uint32_t)sFanScratch.size();
        }

        WGPUBuffer vb = nullptr;
        uint32_t vbOffset = 0;
        if (!PushTransientVerts(drawVerts, drawCount * (uint32_t)sizeof(VertexUI), vb, vbOffset))
            return;

        UiUniformsCPU u = {};
        u.scaleOffset = glm::vec4(posScale, posOffset);
        u.screenSize = glm::vec4((float)GetEngineState()->mWindowWidth,
                                 (float)GetEngineState()->mWindowHeight, 0.0f, 0.0f);
        u.tint = tint;

        SetPipeline(PROG_UI, ML_INLINE_COLOR_44, BK_TRANSLUCENT,
                    /*depthTest*/ false, /*depthWrite*/ false);
        BindUniforms(&u, sizeof(u));
        BindTextureOrWhite(tex);
        wgpuRenderPassEncoderSetVertexBuffer(sPass, 0, vb, vbOffset, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(sPass, drawCount, 1, 0, 0);
    }
}

// External hook for Main_Web's resize path: reconfigure the swapchain and
// depth buffer to the new backing-store size.
void WEB_OnWindowResized(uint32_t width, uint32_t height)
{
    LogDebug("Graphics_WebGPU: window resized to %ux%u", width, height);
    if (sInitialised && (width != sSurfaceW || height != sSurfaceH))
    {
        ConfigureSurface(width, height);
    }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void GFX_Initialize()
{
    // shell.html acquired the device in Module.preRun (behind a run
    // dependency), so this import is synchronous and guaranteed to succeed
    // whenever main() actually started.
    sDevice = emscripten_webgpu_get_device();
    if (sDevice == nullptr)
    {
        LogError("Graphics_WebGPU: no preinitialized WebGPU device — was the "
                 "addon's shell.html used for this build?");
        return;
    }

    sInstance = wgpuCreateInstance(nullptr);
    sQueue = wgpuDeviceGetQueue(sDevice);

    sSurfaceFormat = PolyGpuPreferredFormatIsBGRA()
                         ? WGPUTextureFormat_BGRA8Unorm
                         : WGPUTextureFormat_RGBA8Unorm;

    // Canvas surface ("#canvas" in shell.html).
    {
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas =
            WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
        canvas.selector = Sv("#canvas");

        WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
        desc.nextInChain = &canvas.chain;
        desc.label = Sv("PolySurface");
        sSurface = wgpuInstanceCreateSurface(sInstance, &desc);
    }
    if (sSurface == nullptr)
    {
        LogError("Graphics_WebGPU: surface creation failed");
        return;
    }

    if (!BuildModulesAndLayouts())
    {
        LogError("Graphics_WebGPU: shader/layout setup failed");
        return;
    }

    // White fallback texture + its bind group.
    {
        const uint32_t white = 0xFFFFFFFFu;
        sWhiteTex = CreateTexture2D(1, 1, "PolyWhite");
        UploadTexturePixels(sWhiteTex, 1, 1, (const uint8_t*)&white);
        sWhiteView = wgpuTextureCreateView(sWhiteTex, nullptr);
        sWhiteBindGroup = MakeTextureBindGroup(sWhiteView, GetSampler(false, WrapMode::Repeat));

        // Constant-white vertex color: 4 bytes on an arrayStride-0 slot.
        sWhiteVertexBuffer = CreateBuffer(4, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                          "PolyWhiteVtx");
        wgpuQueueWriteBuffer(sQueue, sWhiteVertexBuffer, 0, &white, 4);
    }

    ConfigureSurface(GetEngineState()->mWindowWidth, GetEngineState()->mWindowHeight);

    sInitialised = true;
    LogDebug("Graphics_WebGPU: device up (surface %s, %ux%u)",
             (sSurfaceFormat == WGPUTextureFormat_BGRA8Unorm) ? "bgra8unorm" : "rgba8unorm",
             sSurfaceW, sSurfaceH);
}

void GFX_Shutdown()
{
    if (!sInitialised) return;

    for (auto& kv : sTextMeshBuffers) ReleaseBuffer(kv.second.buffer);
    sTextMeshBuffers.clear();
    for (auto& kv : sTileMapBuffers)
    {
        ReleaseBuffer(kv.second.vertexBuffer.buffer);
        ReleaseBuffer(kv.second.indexBuffer.buffer);
    }
    sTileMapBuffers.clear();
    for (auto& kv : sTerrainBuffers)
    {
        ReleaseBuffer(kv.second.vertexBuffer.buffer);
        ReleaseBuffer(kv.second.indexBuffer.buffer);
    }
    sTerrainBuffers.clear();

    for (auto& kv : sPipelines) wgpuRenderPipelineRelease(kv.second);
    sPipelines.clear();

    for (UniformChunk& c : sUniformChunks)
    {
        if (c.bindGroup) wgpuBindGroupRelease(c.bindGroup);
        ReleaseBuffer(c.buffer);
    }
    sUniformChunks.clear();
    for (TransientChunk& c : sTransientChunks) ReleaseBuffer(c.buffer);
    sTransientChunks.clear();

    if (sWhiteBindGroup) { wgpuBindGroupRelease(sWhiteBindGroup); sWhiteBindGroup = nullptr; }
    if (sWhiteView) { wgpuTextureViewRelease(sWhiteView); sWhiteView = nullptr; }
    if (sWhiteTex) { wgpuTextureDestroy(sWhiteTex); wgpuTextureRelease(sWhiteTex); sWhiteTex = nullptr; }
    ReleaseBuffer(sWhiteVertexBuffer);

    for (int f = 0; f < 2; ++f)
        for (int w = 0; w < 3; ++w)
            if (sSamplers[f][w]) { wgpuSamplerRelease(sSamplers[f][w]); sSamplers[f][w] = nullptr; }

    for (int i = 0; i < PROG_COUNT; ++i)
        if (sModules[i]) { wgpuShaderModuleRelease(sModules[i]); sModules[i] = nullptr; }
    if (sLayoutWithTex) { wgpuPipelineLayoutRelease(sLayoutWithTex); sLayoutWithTex = nullptr; }
    if (sLayoutNoTex) { wgpuPipelineLayoutRelease(sLayoutNoTex); sLayoutNoTex = nullptr; }
    if (sUniformBGL) { wgpuBindGroupLayoutRelease(sUniformBGL); sUniformBGL = nullptr; }
    if (sTextureBGL) { wgpuBindGroupLayoutRelease(sTextureBGL); sTextureBGL = nullptr; }

    if (sDepthView) { wgpuTextureViewRelease(sDepthView); sDepthView = nullptr; }
    if (sDepthTexture) { wgpuTextureDestroy(sDepthTexture); wgpuTextureRelease(sDepthTexture); sDepthTexture = nullptr; }
    if (sSurface) { wgpuSurfaceRelease(sSurface); sSurface = nullptr; }
    if (sQueue) { wgpuQueueRelease(sQueue); sQueue = nullptr; }
    if (sDevice) { wgpuDeviceRelease(sDevice); sDevice = nullptr; }
    if (sInstance) { wgpuInstanceRelease(sInstance); sInstance = nullptr; }

    sInitialised = false;
}

void GFX_BeginFrame()
{
    if (!sInitialised) return;

    // Rewind the per-frame rings. Safe with one frame in flight: this frame's
    // writeBuffer calls are queue-ordered after last frame's submit.
    sUniformChunkIdx = 0;
    sUniformOffset = 0;
    sTransientChunkIdx = 0;
    sTransientOffset = 0;

    WGPUSurfaceTexture surfaceTex = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(sSurface, &surfaceTex);
    if (surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
    {
        if (surfaceTex.texture) wgpuTextureRelease(surfaceTex.texture);
        LogWarning("Graphics_WebGPU: surface texture unavailable (%d) — reconfiguring",
                   (int)surfaceTex.status);
        ConfigureSurface(GetEngineState()->mWindowWidth, GetEngineState()->mWindowHeight);
        sFrameActive = false;
        return;
    }

    sFrameTex = surfaceTex.texture;
    sFrameView = wgpuTextureCreateView(sFrameTex, nullptr);

    sEncoder = wgpuDeviceCreateCommandEncoder(sDevice, nullptr);

    const glm::vec4 cc = Renderer::Get()->GetClearColor();

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = sFrameView;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = { cc.r, cc.g, cc.b, 1.0 };

    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = sDepthView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;

    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.label = Sv("PolyFrame");
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.depthStencilAttachment = &depth;

    sPass = wgpuCommandEncoderBeginRenderPass(sEncoder, &desc);
    sFrameActive = true;
}

void GFX_EndFrame()
{
    if (!sFrameActive) return;

    wgpuRenderPassEncoderEnd(sPass);
    wgpuRenderPassEncoderRelease(sPass);
    sPass = nullptr;

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(sEncoder, nullptr);
    wgpuCommandEncoderRelease(sEncoder);
    sEncoder = nullptr;

    wgpuQueueSubmit(sQueue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);

    wgpuTextureViewRelease(sFrameView);
    sFrameView = nullptr;
    wgpuTextureRelease(sFrameTex);
    sFrameTex = nullptr;

    // The browser presents when the rAF callback returns.
    sFrameActive = false;
}

void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}
bool GFX_ShouldCullLights() { return true; }

void GFX_BeginRenderPass(RenderPassId pass)
{
    sInForwardPass = (pass == RenderPassId::Forward);
    sInUiPass      = (pass == RenderPassId::Ui);
    sInShadowPass  = (pass == RenderPassId::Shadows);

    if (!sInitialised || !sInForwardPass) return;

    // Cache scene lighting once per pass (PVR2/PSP pattern).
    Renderer* rend = Renderer::Get();
    World*    world = rend ? rend->GetCurrentWorld() : nullptr;

    sAmbient = world ? glm::vec3(world->GetAmbientLightColor()) : glm::vec3(0.0f);
    sHasDirLight = false;
    sLightColor = glm::vec3(0.0f);
    sNumPointLights = 0;

    if (rend)
    {
        sColorScale = rend->GetColorScale();
        // Light colors: color * intensity, exactly like Forward.frag —
        // ColorScale must NOT be applied here (it only compensates the
        // pre-halved storage of cooked vertex/baked colors).
        for (const LightData& ld : rend->GetLightData())
        {
            if (ld.mType == LightType::Directional)
            {
                if (sHasDirLight) continue;
                sLightDir = glm::normalize(-ld.mDirection);
                sLightColor = glm::vec3(ld.mColor) * ld.mIntensity;
                sHasDirLight = true;
            }
            else if (sNumPointLights < kMaxPointLights)
            {
                sPointLightPosRadius[sNumPointLights] =
                    glm::vec4(ld.mPosition, glm::max(ld.mRadius, 0.0001f));
                sPointLightColor[sNumPointLights] =
                    glm::vec3(ld.mColor) * ld.mIntensity;
                ++sNumPointLights;
            }
        }
    }
}

void GFX_EndRenderPass()
{
    sInForwardPass = sInUiPass = sInShadowPass = false;
}

void GFX_SetPipelineState(PipelineConfig /*config*/)
{
    // Pipelines are derived per draw from material + pass state (the GL
    // backend's pass-level toggles are all recomputed in DrawMeshCommon /
    // SubmitUI, so there is nothing to latch here).
}

void GFX_SetViewport(int32_t x, int32_t y, int32_t width, int32_t height, bool /*handlePrerotation*/)
{
    if (!sFrameActive || width <= 0 || height <= 0) return;
    // WebGPU framebuffer origin is top-left, same as engine rects: no y-flip.
    // Clamp to the attachment — out-of-bounds viewports are validation errors.
    float fx = (float)glm::clamp<int32_t>(x, 0, (int32_t)sSurfaceW);
    float fy = (float)glm::clamp<int32_t>(y, 0, (int32_t)sSurfaceH);
    float fw = glm::min((float)width,  (float)sSurfaceW - fx);
    float fh = glm::min((float)height, (float)sSurfaceH - fy);
    if (fw <= 0.0f || fh <= 0.0f) return;
    wgpuRenderPassEncoderSetViewport(sPass, fx, fy, fw, fh, 0.0f, 1.0f);
}

void GFX_SetScissor(int32_t x, int32_t y, int32_t width, int32_t height, bool /*handlePrerotation*/)
{
    if (!sFrameActive) return;
    const int32_t screenW = (int32_t)sSurfaceW;
    const int32_t screenH = (int32_t)sSurfaceH;
    if (x <= 0 && y <= 0 && width >= screenW && height >= screenH)
    {
        wgpuRenderPassEncoderSetScissorRect(sPass, 0, 0, sSurfaceW, sSurfaceH);
        return;
    }
    const uint32_t cx = (uint32_t)glm::clamp<int32_t>(x, 0, screenW);
    const uint32_t cy = (uint32_t)glm::clamp<int32_t>(y, 0, screenH);
    const uint32_t cw = (uint32_t)glm::clamp<int32_t>(width,  0, screenW - (int32_t)cx);
    const uint32_t ch = (uint32_t)glm::clamp<int32_t>(height, 0, screenH - (int32_t)cy);
    wgpuRenderPassEncoderSetScissorRect(sPass, cx, cy, cw, ch);
}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    // WebGPU clip depth is [0, 1]. The bundled glm predates the *_ZO helpers
    // (and a TU-local GLM_FORCE_DEPTH_ZERO_TO_ONE would be an ODR hazard), so
    // build the RH zero-to-one projection directly.
    const float f = 1.0f / std::tan(glm::radians(fovyDegrees) * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspectRatio;
    m[1][1] = f;
    m[2][2] = zFar / (zNear - zFar);
    m[2][3] = -1.0f;
    m[3][2] = -(zFar * zNear) / (zFar - zNear);
    return m;
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    glm::mat4 m(1.0f);
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -1.0f / (zFar - zNear);
    m[3][0] = -(right + left) / (right - left);
    m[3][1] = -(top + bottom) / (top - bottom);
    m[3][2] = -zNear / (zFar - zNear);
    return m;
}

void GFX_SetFog(const FogSettings& fogSettings) { sFog = fogSettings; }

void GFX_ResizeWindow()
{
    if (sInitialised)
    {
        const uint32_t w = GetEngineState()->mWindowWidth;
        const uint32_t h = GetEngineState()->mWindowHeight;
        if (w != sSurfaceW || h != sSurfaceH) ConfigureSurface(w, h);
    }
}

void GFX_Reset() {}
Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* /*outInstance*/) { return nullptr; }
uint32_t GFX_GetNumViews() { return 1; }
void GFX_SetFrameRate(int32_t /*frameRate*/) {}   // rAF paces the loop
void GFX_PathTrace() {}
void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }
void GFX_EnableMaterials(bool enable) { sMaterialsEnabled = enable; }
void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}

// ===========================================================================
// Texture
// ===========================================================================

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    if (!sInitialised || texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;

    const uint32_t w = texture->GetWidth();
    const uint32_t h = texture->GetHeight();
    const std::vector<uint8_t>& px = !texture->GetPixels().empty() ? texture->GetPixels() : data;
    if (w == 0 || h == 0 || px.size() < (size_t)w * h * 4)
    {
        return;
    }

    GFX_DestroyTextureResource(texture);

    WebGpuTexture* wt = new WebGpuTexture();
    wt->tex = CreateTexture2D(w, h, "PolyTexture");
    UploadTexturePixels(wt->tex, w, h, px.data());
    wt->view = wgpuTextureCreateView(wt->tex, nullptr);

    // Honour the asset's filter/wrap settings — never hardcode LINEAR/REPEAT
    // (pixel-art assets ship Nearest; see the PSP port findings).
    const bool linear = (texture->GetFilterType() == FilterType::Linear);
    wt->bindGroup = MakeTextureBindGroup(wt->view, GetSampler(linear, texture->GetWrapMode()));

    r->mPixels = wt;
    r->mWidth = w;
    r->mHeight = h;
    r->mBufWidth = w;
    r->mSwizzled = 0;
    r->mPsm = 0;
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr || r->mPixels == nullptr) return;
    WebGpuTexture* wt = static_cast<WebGpuTexture*>(r->mPixels);
    if (wt->bindGroup) wgpuBindGroupRelease(wt->bindGroup);
    if (wt->view) wgpuTextureViewRelease(wt->view);
    if (wt->tex) { wgpuTextureDestroy(wt->tex); wgpuTextureRelease(wt->tex); }
    delete wt;
    r->mPixels = nullptr;
    r->mWidth = r->mHeight = r->mBufWidth = 0;
}

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* src,
                                     uint32_t srcWidth, uint32_t srcHeight)
{
    if (!sInitialised || texture == nullptr || src == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr || r->mPixels == nullptr) return;
    WebGpuTexture* wt = static_cast<WebGpuTexture*>(r->mPixels);

    if (srcWidth == r->mWidth && srcHeight == r->mHeight)
    {
        UploadTexturePixels(wt->tex, srcWidth, srcHeight, src);
        return;
    }

    // Size changed (streaming video): recreate the texture + dependents.
    if (wt->bindGroup) wgpuBindGroupRelease(wt->bindGroup);
    if (wt->view) wgpuTextureViewRelease(wt->view);
    if (wt->tex) { wgpuTextureDestroy(wt->tex); wgpuTextureRelease(wt->tex); }

    wt->tex = CreateTexture2D(srcWidth, srcHeight, "PolyTexture");
    UploadTexturePixels(wt->tex, srcWidth, srcHeight, src);
    wt->view = wgpuTextureCreateView(wt->tex, nullptr);
    const bool linear = (texture->GetFilterType() == FilterType::Linear);
    wt->bindGroup = MakeTextureBindGroup(wt->view, GetSampler(linear, texture->GetWrapMode()));
    r->mWidth = srcWidth;
    r->mHeight = srcHeight;
}

// ===========================================================================
// Material — console-style: no per-material GPU objects.
// ===========================================================================

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// ===========================================================================
// Static meshes
// ===========================================================================

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices,
                                  void* vertices, uint32_t numIndices, IndexType* indices)
{
    if (!sInitialised || staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;

    GFX_DestroyStaticMeshResource(staticMesh);

    const uint32_t stride = hasColor ? (uint32_t)sizeof(VertexColor) : (uint32_t)sizeof(Vertex);

    WebGpuMesh* m = new WebGpuMesh();
    m->hasColor = hasColor;
    m->numIndices = numIndices;

    const uint32_t vbytes = numVertices * stride;
    const uint32_t ibytes = AlignUp(numIndices * (uint32_t)sizeof(IndexType), 4);
    m->vertexBuffer = CreateBuffer(vbytes, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "PolyMeshVB");
    m->indexBuffer  = CreateBuffer(ibytes, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, "PolyMeshIB");
    WriteBufferPadded(m->vertexBuffer, 0, vertices, vbytes);
    WriteBufferPadded(m->indexBuffer, 0, indices, numIndices * sizeof(IndexType));

    r->mVertexData = m;
    r->mIndexData = nullptr;
    r->mNumVertices = numVertices;
    r->mNumIndices = numIndices;
    r->mVertexStride = stride;
    r->mVertexFlags = hasColor ? 1u : 0u;
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;

    WebGpuMesh* m = static_cast<WebGpuMesh*>(r->mVertexData);
    ReleaseBuffer(m->vertexBuffer);
    ReleaseBuffer(m->indexBuffer);
    delete m;
    r->mVertexData = nullptr;
    r->mNumVertices = r->mNumIndices = 0;
}

// Baked-lighting colors: per-component buffer, heap WebGpuBuffer* stashed in
// the addon-arm slot StaticMeshCompResource::mColorVertexData.
static void WebGpuDestroyBakedColors(StaticMesh3D* c)
{
    StaticMeshCompResource* r = c ? c->GetResource() : nullptr;
    if (r == nullptr || r->mColorVertexData == nullptr) return;
    WebGpuBuffer* b = static_cast<WebGpuBuffer*>(r->mColorVertexData);
    ReleaseBuffer(b->buffer);
    delete b;
    r->mColorVertexData = nullptr;
}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*c*/) {}   // lazy

void GFX_DestroyStaticMeshCompResource(StaticMesh3D* c)
{
    WebGpuDestroyBakedColors(c);
}

void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* c)
{
    if (!sInitialised || c == nullptr) return;
    StaticMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;

    const std::vector<uint32_t>& colors = c->GetInstanceColors();
    if (colors.empty())
    {
        WebGpuDestroyBakedColors(c);
        return;
    }

    WebGpuBuffer* b = static_cast<WebGpuBuffer*>(r->mColorVertexData);
    if (b == nullptr)
    {
        b = new WebGpuBuffer();
        r->mColorVertexData = b;
    }
    UploadGrowableBuffer(*b, colors.data(), (uint32_t)(colors.size() * sizeof(uint32_t)),
                         WGPUBufferUsage_Vertex, "PolyBakedColors");
}

void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (!sFrameActive || !sInForwardPass || comp == nullptr) return;
    StaticMesh* mesh = meshOverride ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    WebGpuMesh* m = static_cast<WebGpuMesh*>(r->mVertexData);
    if (m->numIndices == 0) return;

    // Baked vertex lighting replaces ambient+directional (Vulkan parity).
    // Lazy-upload covers components whose colors existed before our resource.
    WGPUBuffer bakedColors = nullptr;
    if (comp->HasBakedLighting() && meshOverride == nullptr)
    {
        StaticMeshCompResource* cr = comp->GetResource();
        if (cr != nullptr && cr->mColorVertexData == nullptr &&
            comp->GetInstanceColors().size() == r->mNumVertices)
        {
            GFX_UpdateStaticMeshCompResourceColors(comp);
        }
        if (cr != nullptr && cr->mColorVertexData != nullptr)
        {
            bakedColors = static_cast<WebGpuBuffer*>(cr->mColorVertexData)->buffer;
        }
    }

    const bool isSkybox = (comp->As<Skybox3D>() != nullptr);
    DrawMeshCommon(m->vertexBuffer, m->indexBuffer, m->numIndices, 0, true,
                   comp->GetRenderTransform(), comp->GetMaterial(),
                   glm::vec4(1.0f), isSkybox, m, bakedColors);
}

// ===========================================================================
// Skeletal meshes (CPU-skinned — GFX_IsCpuSkinningRequired returns true)
// ===========================================================================

void GFX_CreateSkeletalMeshResource(SkeletalMesh* sm, uint32_t /*numVertices*/,
                                    VertexSkinned* /*vertices*/, uint32_t numIndices, IndexType* indices)
{
    if (!sInitialised || sm == nullptr) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr) return;

    GFX_DestroySkeletalMeshResource(sm);

    WebGpuIndexBuffer* ib = new WebGpuIndexBuffer();
    ib->numIndices = numIndices;
    const uint32_t ibytes = AlignUp(numIndices * (uint32_t)sizeof(IndexType), 4);
    ib->buffer = CreateBuffer(ibytes, WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst, "PolySkelIB");
    WriteBufferPadded(ib->buffer, 0, indices, numIndices * sizeof(IndexType));

    r->mIndexData = ib;
    r->mNumIndices = numIndices;
}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* sm)
{
    if (sm == nullptr) return;
    SkeletalMeshResource* r = sm->GetResource();
    if (r == nullptr || r->mIndexData == nullptr) return;
    WebGpuIndexBuffer* ib = static_cast<WebGpuIndexBuffer*>(r->mIndexData);
    ReleaseBuffer(ib->buffer);
    delete ib;
    r->mIndexData = nullptr;
    r->mNumIndices = 0;
}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*c*/) {}   // lazy

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* c)
{
    if (c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    DestroyDynBuffer(r->mVertexData);
    r->mVertexCapacity = r->mNumVertices = 0;
}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, uint32_t numVerts)
{
    if (!sInitialised || c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;
    WebGpuDynBuffer* b = GetOrCreateDynBuffer(r->mVertexData);
    const uint32_t bytes = numVerts * (uint32_t)sizeof(Vertex);
    if (bytes > b->capacityBytes)
    {
        ReleaseBuffer(b->buffer);
        b->buffer = CreateBuffer(bytes, WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "PolySkelVB");
        b->capacityBytes = bytes;
    }
    r->mVertexCapacity = b->capacityBytes;
    r->mVertexStride = (uint32_t)sizeof(Vertex);
}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c,
                                            const std::vector<Vertex>& skinnedVertices)
{
    if (!sInitialised || c == nullptr) return;
    SkeletalMeshCompResource* r = c->GetResource();
    if (r == nullptr) return;

    WebGpuDynBuffer* b = GetOrCreateDynBuffer(r->mVertexData);
    const uint32_t bytes = (uint32_t)(skinnedVertices.size() * sizeof(Vertex));
    UploadDynVerts(b, skinnedVertices.data(), bytes);
    b->numVerts = (uint32_t)skinnedVertices.size();
    r->mVertexCapacity = b->capacityBytes;
    r->mNumVertices = b->numVerts;
    r->mVertexStride = (uint32_t)sizeof(Vertex);
}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* c)
{
    if (!sFrameActive || !sInForwardPass || c == nullptr) return;
    SkeletalMesh* mesh = c->GetSkeletalMesh();
    if (mesh == nullptr) return;
    SkeletalMeshResource*     mr = mesh->GetResource();
    SkeletalMeshCompResource* cr = c->GetResource();
    if (mr == nullptr || cr == nullptr || mr->mIndexData == nullptr || cr->mVertexData == nullptr) return;

    WebGpuIndexBuffer* ib = static_cast<WebGpuIndexBuffer*>(mr->mIndexData);
    WebGpuDynBuffer*   vb = static_cast<WebGpuDynBuffer*>(cr->mVertexData);
    if (ib->numIndices == 0 || vb->numVerts == 0) return;

    DrawMeshCommon(vb->buffer, ib->buffer, ib->numIndices, 0, true,
                   c->GetRenderTransform(), c->GetMaterial(), glm::vec4(1.0f), false);
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/) { return true; }

void GFX_DrawShadowMeshComp(ShadowMesh3D* /*c*/) {}   // stencil shadows: future work

void GFX_DrawInstancedMeshComp(InstancedMesh3D* comp)
{
    if (!sFrameActive || !sInForwardPass || comp == nullptr) return;
    StaticMesh* mesh = comp->GetStaticMesh();
    if (mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    WebGpuMesh* m = static_cast<WebGpuMesh*>(r->mVertexData);
    if (m->numIndices == 0) return;

    // One draw per instance (PSP/WebGL2 parity). Cheap uniform slices make
    // this tolerable; a real instancing path is future work.
    const uint32_t numInstances = comp->GetNumInstances();
    for (uint32_t i = 0; i < numInstances; ++i)
    {
        const glm::mat4 model = comp->GetRenderTransform() * comp->CalculateInstanceTransform((int32_t)i);
        DrawMeshCommon(m->vertexBuffer, m->indexBuffer, m->numIndices, 0, true,
                       model, comp->GetMaterial(), glm::vec4(1.0f), false, m);
    }
}

// ===========================================================================
// Text meshes (3D) — backend-side buffer table (no addon-arm storage)
// ===========================================================================

void GFX_CreateTextMeshCompResource(TextMesh3D* /*c*/) {}   // lazy

void GFX_DestroyTextMeshCompResource(TextMesh3D* c)
{
    if (c == nullptr) return;
    auto it = sTextMeshBuffers.find(c);
    if (it == sTextMeshBuffers.end()) return;
    ReleaseBuffer(it->second.buffer);
    sTextMeshBuffers.erase(it);
}

void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* c, const std::vector<Vertex>& vertices)
{
    if (!sInitialised || c == nullptr) return;

    WebGpuDynBuffer& b = sTextMeshBuffers[c];
    UploadDynVerts(&b, vertices.data(), (uint32_t)(vertices.size() * sizeof(Vertex)));
    b.numVerts = (uint32_t)vertices.size();
}

void GFX_DrawTextMeshComp(TextMesh3D* c)
{
    if (!sFrameActive || !sInForwardPass || c == nullptr) return;
    auto it = sTextMeshBuffers.find(c);
    if (it == sTextMeshBuffers.end() || it->second.numVerts == 0) return;

    DrawMeshCommon(it->second.buffer, nullptr, 0, it->second.numVerts, false,
                   c->GetRenderTransform(), c->GetMaterial(), glm::vec4(1.0f), false);
}

// ===========================================================================
// Voxel — not in the milestone (matches WebGL2). Terrain / TileMap ARE
// implemented: engine-rebuilt indexed VertexColor meshes drawn through the
// standard forward path.
// ===========================================================================

static void LogOnceUnsupported(const char* what)
{
    static std::unordered_map<const char*, bool> sLogged;
    if (!sLogged[what])
    {
        sLogged[what] = true;
        LogWarning("Graphics_WebGPU: %s not implemented on Web yet", what);
    }
}

void GFX_CreateVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_DestroyVoxel3DResource(Voxel3D* /*v*/) {}
void GFX_UpdateVoxel3DResource(Voxel3D* /*v*/, const std::vector<VertexColor>& /*v2*/, const std::vector<IndexType>& /*i*/) {}
void GFX_DrawVoxel3D(Voxel3D* /*v*/) { LogOnceUnsupported("Voxel3D"); }

namespace
{
    void WebGpuUploadIndexedColorMesh(WebGpuIndexedColorMesh& m,
                                      const std::vector<VertexColor>& vertices,
                                      const std::vector<IndexType>& indices)
    {
        UploadGrowableBuffer(m.vertexBuffer, vertices.data(),
                             (uint32_t)(vertices.size() * sizeof(VertexColor)),
                             WGPUBufferUsage_Vertex, "PolyIcmVB");
        UploadGrowableBuffer(m.indexBuffer, indices.data(),
                             (uint32_t)(indices.size() * sizeof(IndexType)),
                             WGPUBufferUsage_Index, "PolyIcmIB");
        m.numIndices = (uint32_t)indices.size();
    }

    void WebGpuDrawIndexedColorMesh(const WebGpuIndexedColorMesh& m,
                                    const glm::mat4& transform, Material* material)
    {
        // Shim so DrawMeshCommon treats the node's own colour stream (and its
        // ColorScale restore) exactly like a coloured static mesh.
        WebGpuMesh shim;
        shim.vertexBuffer = m.vertexBuffer.buffer;
        shim.indexBuffer = m.indexBuffer.buffer;
        shim.numIndices = m.numIndices;
        shim.hasColor = true;

        DrawMeshCommon(shim.vertexBuffer, shim.indexBuffer, m.numIndices, 0, true,
                       transform, material, glm::vec4(1.0f), false, &shim);
    }
}

void GFX_CreateTileMap2DResource(TileMap2D* /*t*/) {}   // lazy (first Update)

void GFX_DestroyTileMap2DResource(TileMap2D* t)
{
    if (t == nullptr) return;
    auto it = sTileMapBuffers.find(t);
    if (it == sTileMapBuffers.end()) return;
    ReleaseBuffer(it->second.vertexBuffer.buffer);
    ReleaseBuffer(it->second.indexBuffer.buffer);
    sTileMapBuffers.erase(it);
}

void GFX_UpdateTileMap2DResource(TileMap2D* t, const std::vector<VertexColor>& vertices,
                                 const std::vector<IndexType>& indices)
{
    if (!sInitialised || t == nullptr) return;
    WebGpuUploadIndexedColorMesh(sTileMapBuffers[t], vertices, indices);
}

void GFX_DrawTileMap2D(TileMap2D* t)
{
    if (!sFrameActive || !sInForwardPass || t == nullptr) return;
    auto it = sTileMapBuffers.find(t);
    if (it == sTileMapBuffers.end() || it->second.numIndices == 0) return;
    WebGpuDrawIndexedColorMesh(it->second, t->GetRenderTransform(), t->GetMaterial());
}

void GFX_CreateTerrain3DResource(Terrain3D* /*t*/) {}   // lazy (first Update)

void GFX_DestroyTerrain3DResource(Terrain3D* t)
{
    if (t == nullptr) return;
    auto it = sTerrainBuffers.find(t);
    if (it == sTerrainBuffers.end()) return;
    ReleaseBuffer(it->second.vertexBuffer.buffer);
    ReleaseBuffer(it->second.indexBuffer.buffer);
    sTerrainBuffers.erase(it);
}

void GFX_UpdateTerrain3DResource(Terrain3D* t, const std::vector<VertexColor>& vertices,
                                 const std::vector<IndexType>& indices)
{
    if (!sInitialised || t == nullptr) return;
    WebGpuUploadIndexedColorMesh(sTerrainBuffers[t], vertices, indices);
}

void GFX_DrawTerrain3D(Terrain3D* t)
{
    if (!sFrameActive || !sInForwardPass || t == nullptr) return;
    auto it = sTerrainBuffers.find(t);
    if (it == sTerrainBuffers.end() || it->second.numIndices == 0) return;
    WebGpuDrawIndexedColorMesh(it->second, t->GetRenderTransform(), t->GetMaterial());
}

// ===========================================================================
// Particles (pre-billboarded quads from the engine, expanded to triangle
// lists at upload)
// ===========================================================================

void GFX_CreateParticleCompResource(Particle3D* /*c*/) {}   // lazy

void GFX_DestroyParticleCompResource(Particle3D* c)
{
    if (c == nullptr) return;
    ParticleCompResource* r = c->GetResource();
    if (r == nullptr) return;
    DestroyDynBuffer(r->mVertexData);
    r->mVertexCapacity = r->mNumVertices = r->mVertexStride = 0;
}

void GFX_UpdateParticleCompVertexBuffer(Particle3D* c, const std::vector<VertexParticle>& vertices)
{
    if (!sInitialised || c == nullptr) return;
    ParticleCompResource* r = c->GetResource();
    if (r == nullptr) return;

    // Expand each engine quad (corners 0=TL 1=BL 2=TR 3=BR) into two
    // triangles (0,1,2)(2,1,3) so the draw is a plain triangle list.
    const uint32_t numQuads = (uint32_t)(vertices.size() / 4);
    static std::vector<VertexParticle> sExpanded;
    sExpanded.clear();
    sExpanded.reserve((size_t)numQuads * 6);
    for (uint32_t q = 0; q < numQuads; ++q)
    {
        const VertexParticle& v0 = vertices[q * 4 + 0];
        const VertexParticle& v1 = vertices[q * 4 + 1];
        const VertexParticle& v2 = vertices[q * 4 + 2];
        const VertexParticle& v3 = vertices[q * 4 + 3];
        sExpanded.push_back(v0); sExpanded.push_back(v1); sExpanded.push_back(v2);
        sExpanded.push_back(v2); sExpanded.push_back(v1); sExpanded.push_back(v3);
    }

    WebGpuDynBuffer* b = GetOrCreateDynBuffer(r->mVertexData);
    UploadDynVerts(b, sExpanded.data(), (uint32_t)(sExpanded.size() * sizeof(VertexParticle)));
    b->numVerts = (uint32_t)sExpanded.size();
    r->mNumVertices = b->numVerts;
    r->mVertexStride = (uint32_t)sizeof(VertexParticle);
    r->mVertexCapacity = b->capacityBytes;
}

void GFX_DrawParticleComp(Particle3D* c)
{
    if (!sFrameActive || !sInForwardPass || c == nullptr) return;
    if (c->GetNumParticles() == 0) return;
    ParticleCompResource* r = c->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    WebGpuDynBuffer* b = static_cast<WebGpuDynBuffer*>(r->mVertexData);
    if (b->numVerts == 0) return;

    World* world = Renderer::Get()->GetCurrentWorld();
    Camera3D* cam = world ? world->GetActiveCamera() : nullptr;
    if (cam == nullptr) return;

    const glm::mat4 model = c->GetUseLocalSpace() ? c->GetTransform() : glm::mat4(1.0f);
    const glm::mat4 mvp = cam->GetViewProjectionMatrix() * model;
    const glm::vec3 camPos = cam->GetWorldPosition();

    Material*     mat  = c->GetMaterial();
    MaterialLite* lite = Material::AsLite(mat);
    const BlendMode blend = lite ? lite->GetBlendMode() : BlendMode::Additive;
    const int fogMode = (sFog.mEnabled && (mat == nullptr || mat->ShouldApplyFog()))
                            ? WEB_FOG_DISTANCE : WEB_FOG_OFF;

    ParticleUniformsCPU u = {};
    u.mvp = mvp;
    u.model = model;
    u.modes[0] = fogMode;
    u.modes[1] = (sFog.mDensityFunc == FogDensityFunc::Exponential) ? 1 : 0;
    u.params.z = sFog.mNear;
    u.params.w = sFog.mFar;
    u.fogColor = sFog.mColor;
    u.camPos = glm::vec4(camPos, 1.0f);

    const BlendKey bk = BlendKeyFromMode(blend);
    SetPipeline(PROG_PARTICLE, ML_INLINE_COLOR_44, bk,
                /*depthTest*/ true, /*depthWrite*/ bk == BK_OPAQUE);
    BindUniforms(&u, sizeof(u));
    BindTextureOrWhite(lite ? lite->GetTexture(0) : nullptr);
    wgpuRenderPassEncoderSetVertexBuffer(sPass, 0, b->buffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDraw(sPass, b->numVerts, 1, 0, 0);
}

// ===========================================================================
// 2D UI (Quad / QuadBorder / Text / Poly) — transient vertex ring
// ===========================================================================

void GFX_CreateQuadResource(Quad* /*quad*/) {}
void GFX_DestroyQuadResource(Quad* /*quad*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuad(Quad* quad)
{
    if (!sFrameActive || !sInUiPass || quad == nullptr) return;
    SubmitUI(quad->GetVertices(), quad->GetNumVertices(), quad->GetTexture(), quad->GetColor(),
             true, glm::vec2(1.0f), glm::vec2(0.0f));
}

void GFX_CreateQuadBorderResource(Quad* /*quad*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*quad*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuadBorder(Quad* quad)
{
    if (!sFrameActive || !sInUiPass || quad == nullptr) return;
    SubmitUI(quad->GetBorderVertices(), quad->GetNumVertices(), nullptr, quad->GetColor(),
             true, glm::vec2(1.0f), glm::vec2(0.0f));
}

void GFX_CreateTextResource(Text* /*text*/) {}
void GFX_DestroyTextResource(Text* /*text*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*text*/) {}
void GFX_DrawText(Text* text)
{
    if (!sFrameActive || !sInUiPass || text == nullptr) return;
    Font* font = text->GetFont();
    Texture* atlas = font ? font->GetTexture() : nullptr;
    const uint32_t n = text->GetNumVisibleCharacters() * TEXT_VERTS_PER_CHAR;

    // Glyph verts are widget-LOCAL at the font's native point size; bake in the
    // anchor rect + justification and the scaledTextSize/fontSize scale.
    const int32_t fontSize = font ? font->GetSize() : 32;
    const float   scale    = (fontSize > 0) ? (text->GetScaledTextSize() / (float)fontSize) : 1.0f;
    const Rect    rect     = text->GetRect();
    const glm::vec2 just   = text->GetJustifiedOffset();

    SubmitUI(text->GetVertices(), n, atlas, text->GetColor(), false,
             glm::vec2(scale, scale), glm::vec2(rect.mX + just.x, rect.mY + just.y));
}

void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* /*poly*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*poly*/) {}
void GFX_DrawPoly(Poly* poly)
{
    if (!sFrameActive || !sInUiPass || poly == nullptr) return;
    SubmitUI(poly->GetVertices(), poly->GetNumVertices(), poly->GetTexture(), poly->GetColor(),
             true, glm::vec2(1.0f), glm::vec2(0.0f));
}

// ===========================================================================
// Debug draws
// ===========================================================================

void GFX_DrawLines(const std::vector<Line>& lines)
{
    if (!sFrameActive || lines.empty()) return;
    World* world = Renderer::Get()->GetCurrentWorld();
    Camera3D* cam = world ? world->GetActiveCamera() : nullptr;
    if (cam == nullptr) return;

    static std::vector<VertexLine> sVerts;
    sVerts.clear();
    sVerts.reserve(lines.size() * 2);
    for (const Line& l : lines)
    {
        const uint32_t c =
            ((uint32_t)(glm::clamp(l.mColor.a, 0.0f, 1.0f) * 255.0f) << 24) |
            ((uint32_t)(glm::clamp(l.mColor.b, 0.0f, 1.0f) * 255.0f) << 16) |
            ((uint32_t)(glm::clamp(l.mColor.g, 0.0f, 1.0f) * 255.0f) << 8)  |
             (uint32_t)(glm::clamp(l.mColor.r, 0.0f, 1.0f) * 255.0f);
        sVerts.push_back({ l.mStart, c });
        sVerts.push_back({ l.mEnd, c });
    }

    WGPUBuffer vb = nullptr;
    uint32_t vbOffset = 0;
    if (!PushTransientVerts(sVerts.data(), (uint32_t)(sVerts.size() * sizeof(VertexLine)),
                            vb, vbOffset))
        return;

    LineUniformsCPU u = {};
    u.viewProj = cam->GetViewProjectionMatrix();

    SetPipeline(PROG_LINE, ML_INLINE_COLOR_44, BK_OPAQUE,
                /*depthTest*/ true, /*depthWrite*/ true, /*lines*/ true);
    BindUniforms(&u, sizeof(u));
    wgpuRenderPassEncoderSetVertexBuffer(sPass, 0, vb, vbOffset, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderDraw(sPass, (uint32_t)sVerts.size(), 1, 0, 0);
}

void GFX_DrawFullscreen() {}

void GFX_DrawSplats(const GaussianSplatInstance* /*instances*/, uint32_t /*count*/,
                    const glm::vec3& /*cameraRight*/, const glm::vec3& /*cameraUp*/)
{
    LogOnceUnsupported("GaussianSplats");
}

void GFX_DrawStaticMesh(StaticMesh* mesh, Material* material, const glm::mat4& transform, glm::vec4 color)
{
    if (!sFrameActive || mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    WebGpuMesh* m = static_cast<WebGpuMesh*>(r->mVertexData);
    if (m->numIndices == 0) return;

    DrawMeshCommon(m->vertexBuffer, m->indexBuffer, m->numIndices, 0, true,
                   transform, material, color, false, m);
}

void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON
