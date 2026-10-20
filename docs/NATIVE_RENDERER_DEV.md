# Native Renderer Implementation — experimental development branch

This branch is a **work-in-progress replacement for the game GPU interpreter**.
Do not distribute it as a fully native, gameplay-compatible Vulkan renderer yet.
The source contains real native Vulkan graphics/compute command recording, but
the old Gnm/PM4 decoder is still the source of Vulkan resource and shader state.

## Source changes

### Game-level API capture (PS4 GPU encoding bypass point)

- `port/src/probe.c`: for the verified **v01.00** executable only,
  six signature-checked high-level entry hooks: indexed draw, instanced
  indexed draw, non-indexed Draw, PrepareDraw, `JsDevice::CreateBuffer`
  and `JsDevice::CreatePixelShader`. Their original guest routines
  still run; the interception preserves the incoming integer, SIMD and
  flags state before jumping through the original prologues.
- `port/src/native_js_commands.h`: a bounded 4,096-entry command stream
  captures ordinary Draw, indexed draws, instanced indexed draws, PrepareDraw,
  buffer creation and pixel-shader creation BEFORE PM4 encoding. Buffer-description and shader-header snapshots
  are taken at function entry, when their pointers are still live.
- The GPU-side consumer never follows game pointers. It retains only
  immutable integer/header observations and counts dropped/invalid entries.
  No Vulkan resource is substituted from an unverified guest pointer.
- v01.03 high-level entry interception is deliberately disabled: its function
  signatures and guest data layouts have not been independently verified.

### Verified v01.00 draw-call ABI

Disassembly of the original v01.00 executable confirms:

- `JsDeviceContext::DrawIndexedCommon` at guest `0x13198b0`:
  RSI = index count, RDX = guest index-buffer offset, RCX = vertex base
  (stored at context + `0x479c`); instance count is one.
- `JsDeviceContext::DrawIndexedInstanced` at guest `0x1319a00`:
  RSI = index count, RDX = instance count, RCX = guest index offset,
  and R8 = vertex base (stored at context + `0x479c`).
- These methods ultimately generate `sceGnmDrawIndexOffset` calls, with
  different register assignments. The native semantic decoder now treats
  those operations separately. The Vulkan binding offset is subtracted
  from the game's index offset and its remaining capacity checked.

### Game-level nonindexed draw and PrepareDraw state mapping

The original v01.00 executable was independently disassembled for two
additional high-level render calls:

- `JsDeviceContext::Draw` at guest image `0x1312480`: first 18
  relocation-free bytes are a verified trampoline; the original ESI is the
  vertex count and EDX is the starting vertex index. The native nonindexed
  registry path does not require any index buffer, but **does** require
  independently validated vertex fetch and GPU barriers.
- `JsDeviceContext::PrepareDraw` at guest `0x13143e0`: first 24
  relocation-free bytes are a verified trampoline. The snapshot copies the
  three shader-related context slots at +`0x4678`, +`0x4680`, and
  +`0x4688`, plus the original RSI/RDX arguments. These values are
  **opaque guest state**, not safe Vulkan handles.
- `tlg_js_prepare_match.h` pairs each Draw/DrawIndexed event with the next
  PrepareDraw snapshot of that **same** game context. Captured event loss
  invalidates pending pairs, notifies diagnostics and never triggers a Vulkan
  submission. A matched pair is **not proof PrepareDraw succeeded** or
  that the game eventually submitted that draw.
- Synthetic execution of both new trampolines passed GCC/Clang -O2 local
  tests and the 18- and 24-byte prologues exactly match the supplied v01.00
  executable. Full Windows integration and shader binding are unverified.

### Actual native Vulkan resource allocation

- `port/gpu/shim/tlg_native_vk_resources.h` tracks opaque Js resource
  identity, Vulkan handle generations, pipeline/target/index bindings,
  upload epochs and barrier acknowledgment, with a fail-closed draw check.
- `port/gpu/shim/tlg_native_device_resources.{h,cpp}` allocates real
  Vulkan index buffers and 2D color images/views with the port's existing
  `vk::Device` and Vulkan Memory Allocator. It uploads initial index data,
  supports copy-on-write updates (in-flight draws retain the old allocation),
  and defers physical destruction until GPU work is idle. These updates copy
  the entire old index buffer and are a correctness prototype, not an optimal
  streaming strategy. All resource publication follows the same lock order.
- A hidden opt-in `TLG_NATIVE_DEVICE_RESOURCES=1` initializes this
  independent resource owner alongside the existing renderer.
- `TLG_NATIVE_GPU_SMOKE=1` additionally tries a 3-index Vulkan buffer and
  a 2×2 Vulkan color target and logs allocation success/failure.
  **This is a GPU memory smoke, not a fully native game rendering test.**
- New color images start in `VK_IMAGE_LAYOUT_UNDEFINED`. A real Vulkan
  layout transition and resource binding must be recorded before their
  registry can acknowledge barrier readiness. No guest texture layout is
  guessed or used without validation.

Local GCC/Clang C++23 tests of the native resource-registry logic passed.
The complete Vulkan allocation owner cannot be executed in this environment
because Vulkan development headers, a Vulkan ICD/device and the game's
asset archives are not available. No complete Windows game test was run.

### Vulkan command recording

- `port/gpu/shim/tlg_native_vk_draw.h`: typed, PM4-independent Vulkan
  indexed/nonindexed draw and compute-dispatch encoders.
- `port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp`: original
  renderer now calls the native command encoder for ordinary draw and dispatch
  recording, using currently resolved pipeline and buffer state.
- `port/gpu/shim/tlg_js_native_translate.h`: semantic game draw arguments
  resolve to Vulkan indexed draws **only if** an independent native resource
  snapshot already validates the guest context, shader and resource generations,
  index-buffer bounds, render targets, pipeline and necessary barriers.
  This guard is intentionally strict. The native binding snapshot cannot yet
  be populated for real game frames.

### GPU-thread transport

- `port/gpu/tlggpu.h`: callback API for native draw sources.
- `port/gpu/shim/tlg_js_shadow.cpp`: the GPU command thread drains high-level
  commands per submission, before they overflow the game-side capture ring.
  It reports decoded draws and losses, and makes no duplicate Vulkan submissions.
  Its statistics are aggregated once per submission rather than per draw.
- Default behavior remains the original stable renderer. The experimental
  native game-level capture requires the existing `native_js_trace` launcher
  setting / `TLG_JS_NATIVE_TRACE=1` and a signature-verified v01.00 game.

## Manual standalone verification (no GitHub workflows)

Pure CPU-side tests are in `port/tests/`:

- `test_native_js_commands.c`: preserve SysV x86-64 guest ABI and capture
  register arguments with bounded-stream overflow recovery
- `test_native_js_shadow.cpp`: consume semantic commands from the GPU owner
- `test_native_vk_draw.cpp`: emit matching Vulkan draw and compute commands
  into a recording mock, no driver needed
- `test_native_js_to_vk.cpp`: reject incomplete/foreign Vulkan resources
  and out-of-bounds index ranges before generating a draw
- `test_native_vk_resources.cpp`: check native Vulkan resource identity,
  GPU memory generations, guest context ownership, staging write epochs,
  indexed-buffer offset math and invalidation on object replacement
- `test_native_js_pixel_shader_hook.c`: synthetic v01.00 PixelShader
  trampoline, copied header and SysV ABI preservation
- `test_native_js_draw_hook.c`: v01.00 ordinary nonindexed Draw entry
  trampoline and captured vertex-count semantics
- `test_native_js_prepare_draw.c`: high-level PrepareDraw stack-preserving
  trampoline and opaque shader-slot state snapshots
- `test_native_js_prepare_match.cpp`: game context pairing and event-loss
  invalidation, with no Vulkan driver
- `test_native_texture_layout.cpp`: size-checked RGBA8 staging, padded row
  copies, and monotonic Vulkan texture upload/retirement state transitions
- `test_native_vk_resources.cpp`: additionally covers texture/sampler
  generation invalidation and shader-readiness requirements
- `test_native_spirv_registry.cpp`: shader cache key matching, replacement
  generations, budget refusal and nonblocking retained lifetime
- `test_native_shader_inventory.py`: synthetic embedded and extracted
  .sb metadata validation; no proprietary shader data

For a configured native build these are standalone Ninja targets
`native-js-command-test`, `native-js-shadow-test`,
`native-vk-draw-test`, `native-js-to-vk-test`,
`native-vk-resources-test`, `native-js-pixel-shader-hook-test`,
and `native-spirv-registry-test`, plus
`native-js-draw-hook-test`, `native-js-prepare-draw-test`, and
`native-js-prepare-match-test`. They do not use
shaders, game assets, or graphics-driver initialization.

The Vulkan command encoder and register-capture ring were checked locally
with GCC and Clang. The whole fork has **not** been rebuilt or gameplay-tested
for this revision.

## Vulkan-only offscreen rendering prototype

This branch now includes **a real Vulkan-only offscreen rendering path**,
independent of PS4 PM4 command decoding:

- `port/gpu/shadps4/video_core/host_shaders/native_triangle.frag` contains
  a synthetic fragment shader compiled to SPIR-V by the existing
  `glslangValidator` host-shader build, paired with the port's `fs_tri.vert`.
- `port/gpu/shim/tlg_native_triangle_smoke.cpp` creates a native Vulkan
  graphics pipeline using dynamic rendering, recording a three-vertex
  full-screen triangle into an isolated 64×64 color image.
- `DeviceResources` creates the 2D image and image view with VMA,
  transitions it to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`, and
  the existing Vulkan GPU command scheduler records `vkCmdBeginRendering`,
  `vkCmdBindPipeline`, `vkCmdDraw`, and `vkCmdEndRendering`.
- **`TLG_NATIVE_VULKAN_TRIANGLE=1`** enables this smoke in an existing
  GPU-enabled run. It executes independently from the game's translated
  draw commands and does not touch the presented swapchain image.
  It attempts to record the pass once on the GPU submission thread and
  logs native pipeline initialization and recording outcomes.
- The graphics smoke uses a registry-validated native index buffer and
  `vkCmdDrawIndexed`; the renderer copies the 64×64 result to a host-visible
  buffer and verifies the center pixel against its synthetic shader output.
- The destructor completes any deferred recording before freeing the image,
  pipeline and readback buffer.
- The current environment cannot execute the complete Vulkan GPU path:
  **native GPU pixel readback and full Windows gameplay are unverified**.
  A recorded pass is not proof that the test returned the expected pixels.

This validates a narrow **native Vulkan rendering capability**, not a
native renderer for *The Last Guardian*: game shaders, scene resources,
textures, alpha/depth, command ordering, and guest synchronization still
come from the existing translator.

## Offline shader identification

`port/scripts/native_shader_inventory.py` recognizes the original
v01.00/v01.03 executable SHA-256, scans executable ELF segments for
validated `OrbShdr` metadata and code boundaries, and writes an inventory
of shader hashes and types. Optional raw GCN code extraction requires the
user's own `eboot.bin` and keeps copyrighted bytes in a local folder.
The supplied v01.00 executable contains **49 embedded auxiliary shader
binaries** (1 vertex, 1 pixel, 47 compute). Those do **not** represent
all of the actual game's material shaders in PSARC assets.

The inventory does not yet compile GCN to SPIR-V; shader translation
requires verified resource descriptions and per-stage specialization
to preserve original behavior.

## Independent native Vulkan compute verification

The opt-in flag **`TLG_NATIVE_COMPUTE_SMOKE=1`** creates a storage buffer,
a Vulkan compute descriptor-set layout and a compute pipeline compiled from
`native_storage.comp`. It records a native storage-buffer descriptor,
`vkCmdDispatch`, a compute-write-to-host-read barrier, and GPU/CPU readback.
Four known 32-bit values are checked after submission. This executes
independently of the game's Gnm/PM4 processing.

The smoke shader, descriptor layout, pipeline and buffer are synthetic; they
do **not** translate the game's compute effects. The path is opt-in because
its one-shot readback calls `Scheduler::Finish()`. Its GPU result and
Windows build have **not** yet been verified.

## Independent sampled-texture and sampler ownership

The native Vulkan resource manager now implements a first usable
**texture-to-fragment-shader descriptor path**, completely independent of
PM4 texture-binding packets:

- `tlg_native_texture_layout.h` accepts verified **linear RGBA8, 2D,
  one-mip-level** texture data. It validates width, height, row pitch,
  staging-size bounds and copies padded rows to a tight private buffer.
  PS4 tiled/swizzled data, compressed formats, array textures, depth
  textures and unverified mip layouts are rejected rather than guessed.
- `DeviceResources::CreateSampledTextureRGBA8` allocates an optimal-tiled
  Vulkan image, an image view and host-visible transfer staging. It
  copies source bytes immediately; the original guest pointer is never
  retained by the GPU recording thread.
- `RecordSampledTextureUpload` records host-write-to-transfer-read,
  undefined-to-transfer-destination and transfer-write-to-shader-read
  barriers around `vkCmdCopyBufferToImage` on the existing Vulkan scheduler.
  A texture is bindable only after those commands are enqueued in order.
- `ResourceRegistry` maintains separate image-view and sampler generation
  IDs, explicit shader-readiness state, and rejects stale/replaced resources.
  `DeviceResources` owns nearest/clamp VkSamplers independently of PM4
  sampler decoding and defers physical destruction until the GPU is idle.
- `CreateGraphicsPipeline` accepts explicit descriptor layouts for sampled
  images, samplers, storage images or storage buffers. No guessed binding
  numbers are accepted.
- The hidden **`TLG_NATIVE_VULKAN_TEXTURE=1`** option enables a synthetic
  4×4 solid-color texture shader in the existing offscreen indexed-triangle
  test (it supersedes the gradient triangle when both are set). The
  pipeline uses two push descriptors, a Vulkan image upload and
  `vkCmdDrawIndexed`, then copies the 64×64 result to CPU memory and checks
  for RGBA approximately `16,64,128,255`. Nothing is presented in the
  game's swapchain.

**Local validation:** The portable row-pitch/lifetime state tests passed
with GCC and Clang. The full Vulkan-device and Windows-game versions of
this path have **not** been built or run on a real GPU here. It is a
standalone native rendering proof, not the game's material system.

## Exact PS4 RGBA8 detiling (CPU validation + native Vulkan upload)

The native texture owner now supports **four explicitly identified tiled
RGBA8 layouts** in addition to linear uploads:

- `Display1DThin` (tile mode 9) and `Thin1DThin` (tile mode 13):
  8×8 32bpp microtiles, using the precise display/thin bit permutations
  from the port's reference `tiling.comp`. The API requires verified
  8-pixel-aligned tiled pitch and allocated height, and validates source
  memory and 256 MiB upload limits.
- `Display2DThin` (tile mode 10) and `Thin2DThin` (tile mode 14):
  **single-sample, single-slice, non-alt, 32-bit RGBA8**,
  `PIPE_CONFIG=P8_32x32_16x16`, 16 banks, 8 pipes, bank width/height 1,
  macro aspect 2. These produce **128×64 macro tiles (32 KiB)** with
  GCN bank/pipe interleaving. The caller supplies the verified 4-bit
  `bank_swizzle` value and pitch/allocated height aligned to 128×64.
- All four paths convert to a private tightly packed linear buffer,
  then reuse `CreateSampledTextureRGBA8` to allocate a Vulkan image
  and record its staging upload. The source game pointer is not kept
  after the call, and existing textures are not overwritten in flight.
- `test_native_rgba8_detile.cpp` and
  `test_native_rgba8_macro_detile.cpp` are standalone CPU reference
  regressions. The macro test checks **every physical pixel address** in
  each padded tile is unique, in range, and roundtrips for multiple
  dimensions, tile variants and bank swizzles. GCC and Clang C++23
  optimized builds of the detached algorithms passed locally.

Two **opt-in GPU** tests exercise the conversion through actual Vulkan
image creation, sampler binding, draw, and pixel readback:

- `TLG_NATIVE_VULKAN_TILED_TEXTURE=1`: 1D-thin 8×8 RGBA8 with a varying
  color gradient.
- `TLG_NATIVE_VULKAN_MACRO_TEXTURE=1`: 2D-thin 128×64 RGBA8,
  bank-swizzle **7**, and a different gradient. This takes precedence
  over other triangle smoke flags when multiple are enabled.

**These GPU tests are committed but unverified on a real Vulkan device.**
This is not a complete PS4 image-format implementation. Full native
gameplay still needs texture descriptor interception, 2D macro variants
with different pipe/bank settings, BGRA/channel swizzles, BCn compression,
mipmaps, cube/array slices, render-target metadata, aliasing and
CPU/GPU memory ownership. Never treat unsupported layouts as linear.

## Native GCN shader translation and reuse

- `tlg_native_gcn_compiler.{h,cpp}` is a PM4-independent entry point into
  the existing GCN IR translator and SPIR-V emitter. It accepts explicit,
  verified shader bytes, user-data registers and runtime specialization.
  Unverified input, unsupported stages and unknown descriptors fail closed.
- `DeviceResources::CreateResourceFreeGraphicsPipelineFromGCN` connects
  verified descriptor-free vertex/fragment shaders to the independent Vulkan
  pipeline factory. It cannot render arbitrary materials yet.
- `tlg_native_spirv_registry.h` caches immutable SPIR-V for the exact game,
  shader code, runtime specialization, resource layout, recompiler revision
  and Vulkan vendor/device/driver. Reuse across any mismatched key is rejected.
  A bounded memory budget prevents unchecked accumulation.
- `DeviceResources::CreateGraphicsPipelineFromCachedSpirv` consumes those
  verified cached stages, avoiding repeated translation for an identical
  pipeline. It still uses the same safe native resource-lifetime rules.
- The Vulkan pipeline layout reserves the canonical `Shader::PushData`
  range for GCN-generated stage constants. Supplying actual game state for
  that range is part of the missing JsDeviceContext work.

The cache and shader interception have local CPU-only regression coverage;
full game shader SPIR-V translation has **not** been validated. The source
still needs verified per-shader resource descriptors, image formats,
vertex fetch/input layout and stage specializations.

## Independent SPIR-V pipeline ownership

`DeviceResources::CreateGraphicsPipeline` now accepts pretranslated SPIR-V
vertex/fragment modules and creates a Vulkan graphics pipeline through
the existing device. It registers a generation-checked native pipeline
handle with the context resource registry. Replacement and retirement
keep old pipeline allocations alive until the GPU is idle. The initial
fixed-function configuration is a triangle-list pipeline with one color
attachment and no vertex attributes or descriptor sets, suitable for the
offscreen native prototype but **not** for arbitrary Last Guardian
materials.

This is the next structural step toward removing the legacy renderer;
the game shader ABI, reflected descriptor layouts, vertex input and
specialization must still be reconstructed and plugged into these APIs.
Neither independent game-pipeline creation nor full gameplay rendering
has been verified in Windows.

## Critical work that remains before removing the old GPU interpreter

1. Identify and signature-check all **game-level** resource creation,
   destruction and update operations (`JsDevice`, `JsDeviceContext`,
   `JsComputeContext`, `JsSwapChain`) in both v01.00 and v01.03.
2. Build an independent native Vulkan resource owner mapping the game's
   shader, sampler, texture, vertex/index buffer and render target objects,
   including tiling formats and aliasing.
3. Translate GCN shader binaries and all required runtime permutations to
   SPIR-V, then create native graphics/compute pipelines and descriptors.
4. Replace high-level Js draw/dispatch/clear/copy/present operations with
   native recording, preserving submission order and all CPU/GPU readbacks,
   page protection and event/fence semantics. The current native adapter
   will reject draws until these requirements are met.
5. Compare frame images, depth, motion, effects, AI/physics readback and
   frametimes across real gameplay scenes, and only then turn off PM4.

**A complete native Last Guardian renderer has not yet been achieved.**
Do not delete the original Vulkan/PM4 implementation.
