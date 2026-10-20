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

For a configured native build these are standalone Ninja targets
`native-js-command-test`, `native-js-shadow-test`,
`native-vk-draw-test`, and `native-js-to-vk-test`. They do not use
shaders, game assets, or graphics-driver initialization.

The Vulkan command encoder and register-capture ring were checked locally
with GCC and Clang. The whole fork has **not** been rebuilt or gameplay-tested
for this revision.

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
