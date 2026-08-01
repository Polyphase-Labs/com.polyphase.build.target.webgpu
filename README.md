# Polyphase Web (WebGPU) Build Target

Adds a **Web (WebGPU)** entry to the Polyphase editor's Build Profile dropdown.
Sibling of `com.polyphase.build.target.webgl2` — the same compile → package →
serve pipeline, but rendering through **WebGPU** (Dawn's `emdawnwebgpu` port,
WGSL ubershaders) instead of WebGL2. Produces a **single-threaded WebAssembly
module** that runs from **any static host** — no SharedArrayBuffer, no
COOP/COEP headers.

Requires a WebGPU-capable browser: **Chrome/Edge 113+, Safari 18+,
Firefox 141+**. Browsers without WebGPU get a clear message in the shell
instead of a black canvas.

```
Packaged/web.webgpu/
  index.html            <- open this (or host the folder)
  <name>.html/.js/.wasm <- the emcc module
  <name>.data/.data.js  <- MEMFS preload bundle (Content.pak, Config.ini, ...)
```

## Coexistence with the WebGL2 target

Both targets can live in one project. Everything that must differ, does:

| | WebGL2 | WebGPU |
| --- | --- | --- |
| targetId / package dir | `web.webgl2` | `web.webgpu` |
| Profile option keys | `web.*` | `webgpu.*` |
| Staging dirs | `Build/Web`, `Intermediate/Web` | `Build/WebGPU`, `Intermediate/WebGPU` |
| Serve port default | 6931 | 6932 |
| Makefile | `Makefile_Web` | `Makefile_WebGPU` |

## Requirements

- **Emscripten SDK** (`emsdk install latest && emsdk activate latest`).
  Same probing/toolchain options as the WebGL2 addon (native, or WSL on
  Windows; Windows native also needs a GNU make — devkitPro's MSYS2 and
  MSYS2 are auto-detected).
- **The first build needs network access once**: emscripten's
  `--use-port=emdawnwebgpu` downloads Dawn's pinned `emdawnwebgpu_pkg` zip
  into the emscripten cache. Subsequent builds are offline.
- **Node 16+**, only if you want multiplayer (relay in `Tools/relay/`).

## Recommended profile

- **Static Content ON + Content Pak ON, Embedded OFF** (*Embedded* is
  rejected by this target). Lua scripts always ship embedded in the wasm.

## Target Options

Same set as the WebGL2 addon (toolchain, emsdk/make paths, optimization,
initial memory, jobs, server port, relay URL, makefile override), stored under
the `webgpu.*` namespace so profiles that switch between the two web targets
don't cross-contaminate.

## How the renderer works

`Runtime/Web/Graphics_WebGPU/` implements the engine's 91-function `GFX_*`
surface in the single-file console-backend style, at feature parity with the
WebGL2 backend: static meshes, CPU-skinned skeletal meshes, text meshes,
terrain, tilemaps, particles, UI widgets, debug lines, fog, ambient + 1
directional + 8 point lights, unlit/dynamic/baked light modes, blend modes,
alpha masking. Shader-graph materials degrade to the standard WGSL ubershader
(WebGPUShaders.h); baked material SPIR-V is ignored. Stubs (as on WebGL2):
shadows, voxels, splats, post-process, hit-check, GPU timestamps. Mipmaps are
not generated (no glGenerateMipmap in WebGPU; a blit-based generator is future
work).

WebGPU-specific notes, for anyone maintaining the backend:

- **Async device**: `shell.html` requests adapter+device in `Module.preRun`
  behind a run dependency, so `main()` (and the synchronous
  `GFX_Initialize`) never runs without a device. The wasm side imports it
  via `emscripten_webgpu_get_device()`.
- **One render pass per frame**, submitted in `GFX_EndFrame`; the browser
  presents when the rAF callback returns.
- **Per-draw uniforms ride a dynamic-offset ring** (256-aligned slices in
  256 KB chunks) because `writeBuffer` executes before submit — rewriting one
  uniform buffer per draw GL-style would give every draw the last value.
- **Pipeline cache** keyed by {program, vertex layout, blend, depthTest,
  depthWrite, topology}; ~15 pipelines in practice.
- **GL idioms emulated**: TRIANGLE_FAN → CPU fan→list expansion; constant
  vertex attribute → `arrayStride: 0` buffer.
- **[0,1] clip depth** → the backend builds RH zero-to-one projection
  matrices itself (the bundled glm predates `*_ZO`).

## HTTP and multiplayer

Identical to the WebGL2 addon — the runtime files are the same:

- **HTTP** works out of the box via `fetch()` (`Http_Web.cpp`). No
  `Http::SendSync` (browsers can't block); CORS applies.
- **Multiplayer** tunnels UDP over a WebSocket to `Tools/relay/`
  (`npm start`, no dependencies). Set **Network Relay URL** in Target
  Options; empty = networking off. A page served over `https://` needs
  `wss://`. The relay is a UDP proxy — keep it on loopback/private ranges
  unless you know what you're doing. See `Tools/relay/README.md`.

## Build & Run

The editor's **Build & Run** serves `Packaged/web.webgpu/` on
`http://localhost:<port>/` (default 6932) and opens the default browser.

Manual compile (verifies compile+link only — no content bundle without the
editor's PostPackage):

```bash
. <emsdk>/emsdk_env.sh
mkdir -p <projectDir>/Intermediate/WebGPU && cd <projectDir>/Intermediate/WebGPU
emmake make -f <addon>/Makefile_WebGPU PROJECT_ROOT=<projectDir> \
    POLYPHASE_PATH=<engineRoot> -j8
```

## Layout

```
package.json                          manifest (buildTargets: web.webgpu)
Source/ComPolyphaseBuildTargetWebgpu.cpp   editor-side descriptor + callbacks
Makefile_WebGPU                       emcc build (--use-port=emdawnwebgpu)
Runtime/Web/                          Variant-2 engine runtime
  Main_Web.cpp System_Web.cpp Input_Web.cpp Audio_Web.cpp
  Http_Web.cpp Network_Web.cpp        (shared with the WebGL2 addon)
  Graphics_WebGPU/Graphics_WebGPU.cpp WebGPU backend
  Graphics_WebGPU/WebGPUShaders.h     embedded WGSL ubershaders
  shell.html                          emcc --shell-file (WebGPU preRun +
                                      PostPackage markers)
  *_Platform.h                        engine platform-extension headers
Tools/relay/                          WebSocket<->UDP relay for multiplayer
```
