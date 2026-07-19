# AMD FSR 4 for Godot (experimental fork)

This fork adds **AMD FidelityFX Super Resolution 4 (FSR 4)** as a 3D scaling mode to the
Godot editor/runtime, alongside the existing FSR 1.0 and FSR 2.2.

FSR 4 is a machine-learning temporal upscaler reachable through the AMD FidelityFX API (FfxApi)
on a native **Direct3D 12** device, so this integration is **D3D12-only**. The FSR 4 runtime is
provided by the **AMD driver** (`amd_fidelityfx_dx12.dll`, installed in `System32`) — no AMD
binary is shipped or redistributed with Godot.

## Requirements

- **Windows** + the **Direct3D 12** rendering driver (`--rendering-driver d3d12`).
- For the **driver-based FSR 4** (ML upscaler): a **recent AMD driver** (Adrenalin)
  that provides FSR 4, on a supported GPU: **RDNA 4 (RX 9000)** or **RDNA 3 (RX 7000)**.
- For the **SDK-based fallback** (FSR 3.1 / 2.x via the FfxApi upscaler DLL): any
  Direct3D 12 GPU. You must **manually download** the FidelityFX SDK runtime
  DLLs from AMD and place them next to the Godot executable (see
  [Obtaining the runtime DLLs](#obtaining-the-runtime-dlls) below). If the
  loader DLL is absent, the viewport falls back to Godot's built-in FSR 2.

The FfxApi headers needed at build time are vendored in `thirdparty/amd-fsr4`,
so no external SDK is required to build. The runtime DLLs are **not**
redistributed (AMD's license does not allow it); end users must obtain them
manually.

## Building

```sh
scons platform=windows target=editor d3d12=yes fsr4=yes
```

`fsr4=yes` requires `d3d12=yes`. The build only needs the FfxApi headers
vendored in `thirdparty/amd-fsr4` — no external SDK is required to build.
The runtime DLLs are **not** redistributed; see below for how to obtain them.

## Obtaining the runtime DLLs

You must obtain the runtime DLLs yourself and place them next to the Godot
executable (`bin/`) before launching the editor or a packaged game.

### AMD GPU users (RDNA 3/4) with a recent driver

Nothing to do. The AMD driver ships `amd_fidelityfx_dx12.dll` in
`C:\Windows\System32\`, which Godot detects and loads automatically at runtime.

### Non-AMD GPUs, or older AMD drivers

Download the **FidelityFX SDK** from AMD and extract these two DLLs into `bin/`
(next to `godot.windows.editor.x86_64.exe`):

- `amd_fidelityfx_loader_dx12.dll`
- `amd_fidelityfx_upscaler_dx12.dll`

The SDK is available from AMD's GPUOpen portal:
<https://gpuopen.com/fidelityfx-sdk/>

Look under `bin/` (or `sdk/bin/x64/`) in the extracted SDK for those two files
and copy them next to the Godot executable. Without them, FSR 4 falls back to
Godot's built-in FSR 2 at runtime.

## Using it

- **Project Settings → Rendering → Scaling 3D → Mode → "FSR 4 (D3D12/AMD only)"**, and set
  **Scale** below 1.0 (e.g. 0.5).
- Run with the Direct3D 12 driver. When active, the console lists the upscaler providers offered
  by the driver, e.g. `FSR 4: available upscaler providers: 4.1.0 *, 3.1.3, 2.3.2` (the `*` marks
  the active provider).
- If the driver/hardware doesn't provide FSR 4, it transparently falls back to FSR 2.

## Implementation notes

- The FSR 4 runtime is loaded from the AMD driver's `amd_fidelityfx_dx12.dll` when
  present (interface-compatible with the FidelityFX SDK loader). On non-AMD GPUs,
  it falls back to the user-provided `amd_fidelityfx_loader_dx12.dll` +
  `amd_fidelityfx_upscaler_dx12.dll` (from the FidelityFX SDK), which expose
  FSR 3.1 / 2.x providers cross-vendor. No AMD binary is redistributed; users
  must download the SDK DLLs manually (see above).
- The upscale is recorded into Godot's own frame command list via the render graph's
  `RenderingDevice::driver_callback_add`, so barriers/ordering are handled by the graph.
- Godot's sentinel motion vectors are resolved via the existing `MotionVectorsStore` (shared with
  MetalFX) before the dispatch, which is required for temporal stability.
- The FfxApi context is GPU-synchronized before destruction to survive rapid render-scale changes.
- No Resource Heap Tier gate is applied: the FidelityFX loader lets a context be
  created on Tier 1 devices (it only emits a warning), and the dispatch works after
  resetting Godot's cached command-list state (PSO, root signature CRCs, descriptor
  heap flag) post-dispatch so Godot rebinds its own state on the next draw.

## Known limitations

- FSR 4 requires an AMD GPU + recent AMD driver, and the Direct3D 12 backend. It is not available
  on other vendors/backends (the viewport falls back to FSR 2 there).
- Running with `--gpu-validation` crashes inside the D3D12 GPU-based-validation layer during the
  FSR 4 ML dispatch (a validation-tool incompatibility, not an integration bug). Do not use it.
