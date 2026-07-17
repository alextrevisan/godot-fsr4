# AMD FSR 4 for Godot (experimental fork)

This fork adds **AMD FidelityFX Super Resolution 4 (FSR 4)** as a 3D scaling mode to the
Godot editor/runtime, alongside the existing FSR 1.0 and FSR 2.2.

FSR 4 is a machine-learning temporal upscaler reachable through the AMD FidelityFX API (FfxApi)
on a native **Direct3D 12** device, so this integration is **D3D12-only**. The FSR 4 runtime is
provided by the **AMD driver** (`amd_fidelityfx_dx12.dll`, installed in `System32`) — no AMD
binary is shipped or redistributed with Godot.

## Requirements

- **Windows** + the **Direct3D 12** rendering driver (`--rendering-driver d3d12`).
- A **recent AMD driver** (Adrenalin) that provides FSR 4, on a supported GPU:
  **RDNA 4 (RX 9000)** or **RDNA 3 (RX 7000)**.

The FfxApi headers needed at build time are vendored in `thirdparty/amd-fsr4`, so no external SDK
is required. Nothing from the AMD SDK is needed at runtime.

## Building

```sh
scons platform=windows target=editor d3d12=yes fsr4=yes
```

`fsr4=yes` requires `d3d12=yes`. The build is self-contained (FfxApi headers are vendored); the
FSR 4 runtime comes from the AMD driver, so there are **no DLLs to copy** next to the binary.

## Using it

- **Project Settings → Rendering → Scaling 3D → Mode → "FSR 4 (D3D12/AMD only)"**, and set
  **Scale** below 1.0 (e.g. 0.5).
- Run with the Direct3D 12 driver. When active, the console lists the upscaler providers offered
  by the driver, e.g. `FSR 4: available upscaler providers: 4.1.0 *, 3.1.3, 2.3.2` (the `*` marks
  the active provider).
- If the driver/hardware doesn't provide FSR 4, it transparently falls back to FSR 2.

## Implementation notes

- The FSR 4 runtime is loaded from the AMD driver's `amd_fidelityfx_dx12.dll` (interface-compatible
  with the FidelityFX SDK loader). No AMD binary is redistributed.
- The upscale is recorded into Godot's own frame command list via the render graph's
  `RenderingDevice::driver_callback_add`, so barriers/ordering are handled by the graph.
- Godot's sentinel motion vectors are resolved via the existing `MotionVectorsStore` (shared with
  MetalFX) before the dispatch, which is required for temporal stability.
- The FfxApi context is GPU-synchronized before destruction to survive rapid render-scale changes.

## Known limitations

- FSR 4 requires an AMD GPU + recent AMD driver, and the Direct3D 12 backend. It is not available
  on other vendors/backends (the viewport falls back to FSR 2 there).
- Running with `--gpu-validation` crashes inside the D3D12 GPU-based-validation layer during the
  FSR 4 ML dispatch (a validation-tool incompatibility, not an integration bug). Do not use it.
