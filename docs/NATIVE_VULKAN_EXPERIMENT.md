# Native Vulkan renderer transition — experimental branch

This is a staged, reversible conversion from PS4 Gnm/PM4 processing to a native
Vulkan renderer. **It is not yet a complete native renderer**; the current code
still uses Liverpool PM4, the shadPS4 shader recompiler, and the existing Vulkan
resource manager. Do not merge into main as a performance upgrade without testing.

## Implemented in this branch

### Frame rate and stability — applies through the normal launcher

- The built-in launcher supports **verified v01.00 AND v01.03 executables**.
- v01.00 uses independently identified flip-rate and smooth-delta-time offsets;
  v01.03 retains its original offsets.
- Unknown or modified executables do not receive unverified FPS writes.
- 30 FPS: no FPS patch; 60 FPS: four writes; Unlocked: one write.
- The launcher includes a recommended stability checkbox (defaults ON):
  `TLG_PREP_WORKERS=2` and `TLG_ASYNC_SHADERS=0`.
  This follows the workaround for the GPU page-fault/EOP deadlock in upstream issue #37.
- Any explicit environment variable overrides the checkbox defaults. Setting
  `TLG_PREP_WORKERS=0` tests the more conservative workaround.

The 01.00 patch generator was validated against an original 01.00
`eboot.bin` supplied for development. Complete 01.00 gameplay and matching
module compatibility remain unverified. **No game executable is committed**.

### GPU phase A — Gnm draw bridge

- `TLG_NATIVE_DRAW_BRIDGE=1` enables validated hints for Gnm `DrawIndexAuto`.
- The Gnm writer captures the original, unmodified PM4 packet. When the GPU
  interpreter sees the matching packet, it directly invokes the existing Vulkan
  `Rasterizer::Draw` path (without the normal diagnostic marker wrapper).
- Missing, overwritten or modified packets fall back to the legacy path.
- The original register-state updates, resource binding, draw-preparation,
  Vulkan synchronization, and event/fence ordering are preserved.
- This path **does not eliminate PM4 decoding** or shader translation. It is an
  integration scaffold, not a general-purpose native renderer.

### GPU phase B — immutable Vulkan compute-operation recipes

- `TLG_NATIVE_COMPUTE_RECIPES=1` uses immutable `ComputePipeline` candidate
  metadata to preclassify possible Vulkan-native image copies, clears and metadata
  clears. Existing runtime size/format/alias/readback checks still run for
  eligible operations; ineligible compute pipelines skip unnecessary checks.
- The port's existing native Vulkan `runtime.CopyColorAndDepth` and
  `runtime.ClearImage` implementations remain the actual execution mechanisms.
- Disabled by default. The launcher `Experimental Vulkan draw bridge` test
  option enables both phase A and phase B.

This only removes a small part of repeated selection work. No FPS uplift is
claimed without controlled benchmarks.

## How to test

1. Build the experimental branch using the repo's CI or `port/build.sh`.
2. Select your own game files and 30/60/Unlocked mode in the launcher.
3. Leave **Stable GPU mode** on. Start with the experimental Vulkan toggle OFF
   to establish a baseline; test the same scene with it ON.
4. Collect `out/last_run.log` from both runs. The draw bridge reports counts
   of validated, rejected and unmatched packets at five-second intervals.
5. Compare the port's `Frame stats`, `Frame pacing`, shader compilation time,
   CPU GPU-command-thread time, p99 frame duration, and GPU utilization.
6. Watch for missing effects, texture artifacts, changes in gameplay timing,
   delayed fences, crashes, and freezes over a long session.

Unit/regression tests:
```bash
cd port
python3 -m unittest discover -s tests -p 'test_tlg_patches.py'
python3 -m unittest discover -s tests -p 'test_native_launcher.py'
ninja -C out/gpu native-draw-bridge-test
TLG_NATIVE_DRAW_BRIDGE=1 ./out/gpu/native-draw-bridge-test  # Windows: .exe
```
The test executable does not require the game or a Vulkan device. CI builds
are necessary but cannot replace actual gameplay validation.

## Roadmap — full native renderer still required

1. **Rendering API capture**: identify `JsDevice`/`JsDeviceContext` and
   `JsComputeContext` entry points in both 01.00 and 01.03, with verified
   signatures and call conventions (no guessed detours).
2. **Native resource objects**: map the game's textures, buffers, render targets,
   samplers and view lifetimes to Vulkan objects; detile textures once and track
   updates, aliasing, flushes and guest/host memory coherence.
3. **Offline shader translation**: extract and translate available .sb GCN shaders
   and known variants to SPIR-V; cache pipeline configurations under a key that
   includes shader contents, guest version, Vulkan device/driver features and
   recompiler version. Dynamic combinations still need runtime fallback.
4. **Native per-frame command recording**: transform the Js-level state setters,
   Draw/DrawIndexed/Dispatch/Clear/Copy/Present/Map into direct Vulkan operations.
   Keep guest synchronization semantics and the GPU/CPU readback paths.
5. **Progressive fallback removal**: compare visual outputs, saves, AI/Trico
   physics, controls, audio and shader behavior across many scenes; remove PM4
   only when coverage is 100% for both validated game builds.

The last four steps require substantial engine reverse engineering, source
implementation, Windows and Linux builds, and gameplay validation. Full native
renderer replacement has **not** been completed in this branch.
