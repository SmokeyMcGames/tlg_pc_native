# Native Renderer Implementation — experimental development branch

This branch is a **work-in-progress replacement for the game GPU interpreter**.
Do not distribute it as a fully native, gameplay-compatible Vulkan renderer yet.
The source contains real native Vulkan graphics/compute command recording, but
the old Gnm/PM4 decoder is still the source of Vulkan resource and shader state.

## Source changes

### Game-level API capture (PS4 GPU encoding bypass point)

- `port/src/probe.c`: for the verified **v01.00** executable only,
  two signature-checked JsDeviceContext draw-entry trampolines capture every
  original argument register without changing guest execution or any XMM state.
- `port/src/native_js_commands.h`: a bounded 4,096-entry command stream
  captures indexed and indexed-instanced requests BEFORE PM4 encoding.
  The GPU-side consumer copies only opaque integer values, never follows game
  pointers. Overflow is counted, not allowed to block the game.
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

For a configured native build these are standalone Ninja targets
`native-js-command-test`, `native-js-shadow-test`,
`native-vk-draw-test`, `native-js-to-vk-test`, and
`native-vk-resources-test`. They do not use
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
- The smoke destructor submits any deferred recording and waits for GPU
  execution before freeing its pipeline and image.
- **No image readback or pixel comparison is implemented**, and the
  current environment cannot run the full Vulkan code with a GPU device.
  Successful initialization or scheduling is not proof of an actual
  correct rendered image. Full Windows/Linux build and runtime test
  remain unverified on this development branch.

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
