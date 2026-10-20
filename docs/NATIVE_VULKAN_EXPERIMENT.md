# Experimental native Gnm → Vulkan draw bridge (phase 1)

This branch introduces a **strictly opt-in, source-level experiment**. It is not yet
a full JsDevice/JsDeviceContext native renderer, and no performance improvement is claimed.

## What is implemented

- At the `sceGnmDrawIndexAuto` Gnm API boundary, capture a 3-word type-3 draw packet
  *after* the Gnm writer has emitted it.
- At the Liverpool GPU-command processor, validate the recorded packet against its
  source address and full contents. If valid, call the existing Vulkan rasterizer's
  typed `Draw(false, 0, prepared)` directly, without the generic draw marker wrapper.
- Any missing, overwritten, or modified hint takes the original PM4 path.
- The original command stream, register updates, prepared draw consumption,
  resource binding, Vulkan synchronization, GPU page faults, and EOP handling
  remain under the existing code, unchanged.
- No game code, save files, shaders, or game data are rewritten.

This feature is intentionally limited. The `ApplyGraphicsRegisterPacket` path still
runs and the GPU does not bypass full PS4 command interpretation. Most of the
expensive per-draw resource-binding work still executes. It is an integration
and validation scaffold for subsequent, larger native translation passes.

## Test

Default: `TLG_NATIVE_DRAW_BRIDGE=0` (or unset), existing rendering only.

Opt in: launch the game with `TLG_NATIVE_DRAW_BRIDGE=1`.

When enabled, search `out/last_run.log` for:
`Native Gnm->Vulkan draw bridge: accepted ..., missing ..., modified ...`.

Use the same save, camera path, resolution, frame-rate option, and shader-cache
state when comparing against the baseline. **Do not expect a meaningful FPS gain**
from this initial branch.

Restore the known-stable freeze-workaround environment:
`TLG_PREP_WORKERS=2`, `TLG_ASYNC_SHADERS=0`.
Avoid raising preparation concurrency while issue #37 remains unresolved.

## Build

The fork's `.github/workflows/build.yml` runs Linux and Windows builds on
pull requests to `main`. A PR build creates artifacts but does not publish a
release. Review those builds before testing any uploaded package.

## Known work still required

1. **v1.00 FPS patch**: the upstream fork currently supports only v1.03.
   The separately verified v1.00 patch must be merged into source as a separate
   change with tests before distributing a version supporting both.
2. **Native renderer**: the real next stage is to hook high-level Js* state and
   resource operations in the guest executable with version-specific verified
   addresses. A 1.00 address cannot be inferred from 1.03.
3. **Performance**: implement validated reusable native Vulkan resource bindings
   and command plans before expecting sustained CPU-time savings.
4. **Compatibility**: repeat long gameplay, GPU synchronization, shader, input,
   and rendering-validation tests on Windows. CI compilation is not gameplay QA.

A successful compile does not demonstrate native rendering, parity, or faster FPS.
