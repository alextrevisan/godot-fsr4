/**************************************************************************/
/*  fsr4.cpp                                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "fsr4.h"

#ifdef FSR4_ENABLED

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_driver.h"

// clang-format off
// <windows.h> must precede the FidelityFX D3D12 headers (which include <d3d12.h>), so keep this
// include order fixed.
#include <windows.h>

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>
// clang-format on

using namespace RendererRD;

namespace {

// Process-wide FidelityFX runtime: the loader DLL plus the five FfxApi entry points.
struct FfxRuntime {
	bool attempted = false;
	HMODULE module = nullptr;
	PfnFfxCreateContext create_context = nullptr;
	PfnFfxDestroyContext destroy_context = nullptr;
	PfnFfxQuery query = nullptr;
	PfnFfxDispatch dispatch = nullptr;

	bool is_ready() const {
		return module != nullptr && create_context && destroy_context && query && dispatch;
	}
};

FfxRuntime ffx_runtime;

// Cached probe state. -1 = not probed yet, 0 = unsupported, 1 = supported.
int fsr4_supported = -1;
String fsr4_provider_version;

// Cached list of FfxApi upscaler providers available on this device (enumerated once).
bool fsr4_providers_enumerated = false;
Vector<FSR4Effect::Provider> fsr4_providers;

// Whether the device supports Resource Heap Tier 2. On Tier 1 (e.g. NVIDIA Pascal MX150), the
// FfxApi DX12 backend creates committed resources instead of placed resources, but still issues
// D3D12_RESOURCE_BARRIER_TYPE_ALIASING barriers — which are only valid for placed resources.
// We vtable-hook ResourceBarrier on Tier 1 to replace those invalid aliasing barriers with UAV
// barriers, preventing a GPU driver crash.
bool device_is_tier1 = false;

static void *g_vtable_copy[256];

// Routes FidelityFX runtime messages into Godot's log.
void ffx_message_callback(uint32_t p_type, const wchar_t *p_message) {
	String msg = String(p_message);
	if (p_type == FFX_API_MESSAGE_TYPE_ERROR) {
		ERR_PRINT("FSR 4 (FfxApi): " + msg);
	} else {
		WARN_PRINT("FSR 4 (FfxApi): " + msg);
	}
}

// Blocks until the GPU has finished all previously-submitted work on the main queue. Used before
// destroying an FfxApi context so the driver never frees resources that are still referenced by
// in-flight commands. Without this, rapidly recreating the context (e.g. dragging the 3D render
// scale slider) frees GPU resources mid-flight and crashes the AMD driver.
void wait_for_gpu_idle() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	ID3D12Device *device = reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
	ID3D12CommandQueue *queue = reinterpret_cast<ID3D12CommandQueue *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE));
	if (device == nullptr || queue == nullptr) {
		return;
	}

	ID3D12Fence *fence = nullptr;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || fence == nullptr) {
		return;
	}
	const UINT64 target_value = 1;
	if (SUCCEEDED(queue->Signal(fence, target_value))) {
		if (fence->GetCompletedValue() < target_value) {
			HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (event_handle != nullptr) {
				if (SUCCEEDED(fence->SetEventOnCompletion(target_value, event_handle))) {
					WaitForSingleObject(event_handle, 2000);
				}
				CloseHandle(event_handle);
			}
		}
	}
	fence->Release();
}

// Lazily loads the FidelityFX loader DLL and resolves the FfxApi entry points.
FfxRuntime *get_ffx_runtime() {
	if (ffx_runtime.attempted) {
		return ffx_runtime.is_ready() ? &ffx_runtime : nullptr;
	}
	ffx_runtime.attempted = true;

	// Prefer the AMD driver's runtime (amd_fidelityfx_dx12.dll in System32) so AMD GPUs use the
	// driver's tuned FSR 4 and no binary needs to ship. If it is absent (non-AMD GPUs, or an older
	// driver), fall back to the FidelityFX SDK loader that the application bundles next to its
	// executable (amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll), which provides
	// cross-vendor FSR 3.1/2.x (and FSR 4 on AMD RDNA 3/4). When neither is found, FSR 4 is
	// unavailable and the viewport falls back to Godot's built-in FSR 2.
	const wchar_t *dll_names[] = { L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_loader_dx12.dll" };
	for (const wchar_t *dll_name : dll_names) {
		ffx_runtime.module = LoadLibraryW(dll_name);
		if (ffx_runtime.module != nullptr) {
			break;
		}
	}
	if (ffx_runtime.module == nullptr) {
		print_verbose("FSR 4: no FidelityFX runtime found (neither the AMD driver's 'amd_fidelityfx_dx12.dll' nor a bundled 'amd_fidelityfx_loader_dx12.dll').");
		return nullptr;
	}

	ffx_runtime.create_context = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(ffx_runtime.module, "ffxCreateContext"));
	ffx_runtime.destroy_context = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(ffx_runtime.module, "ffxDestroyContext"));
	ffx_runtime.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(ffx_runtime.module, "ffxQuery"));
	ffx_runtime.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(ffx_runtime.module, "ffxDispatch"));

	if (!ffx_runtime.is_ready()) {
		WARN_PRINT("FSR 4: the FidelityFX loader DLL is missing expected entry points (ABI mismatch?).");
		return nullptr;
	}
	return &ffx_runtime;
}

// Returns the native ID3D12Device the RenderingDevice is running on, or nullptr if not D3D12.
ID3D12Device *get_d3d12_device() {
	if (OS::get_singleton()->get_current_rendering_driver_name() != "d3d12") {
		return nullptr;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, nullptr);
	return reinterpret_cast<ID3D12Device *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE));
}

// Whether the device meets the minimum capabilities for the FSR upscaler providers.
// The FfxApi upscalers nominally want Resource Heap Tier 2, but we attempt context creation
// on all devices and let the FfxApi backend handle Tier 1 (it emits a warning and is supposed
// to fall back to committed resources). If the dispatch later crashes on a Tier 1 device, the
// viewport will have already fallen back to FSR 2 via the is_supported() probe.
bool device_supports_fsr(ID3D12Device *device) {
	if (device == nullptr) {
		return false;
	}
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
		device_is_tier1 = options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2;
	}
	return true;
}

// Per-dispatch payload handed to the render-graph driver callback. Allocated on upscale(), freed
// inside the callback after the dispatch is recorded.
struct FSR4DispatchData {
	ffxContext context = nullptr;
	PfnFfxDispatch dispatch = nullptr;

	ID3D12Resource *color = nullptr;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *velocity = nullptr;
	ID3D12Resource *reactive = nullptr;
	ID3D12Resource *exposure = nullptr;
	ID3D12Resource *output = nullptr;

	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t upscale_width = 0;
	uint32_t upscale_height = 0;
	float jitter_x = 0.0f;
	float jitter_y = 0.0f;
	float sharpness = 0.0f;
	float delta_time = 0.0f;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	bool reset = false;
};

// Recorded into Godot's frame command list during render-graph replay. All input textures are in
// FFX_API_RESOURCE_STATE_COMPUTE_READ and the output is in FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
// (matching the layouts the graph transitions the declared callback resources to), so the FfxApi
// backend records no barriers of its own and Godot's state tracker stays consistent.
void fsr4_dispatch_callback(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_command_buffer, void *p_userdata) {
	FSR4DispatchData *data = static_cast<FSR4DispatchData *>(p_userdata);

	void *command_list = reinterpret_cast<void *>(p_driver->command_buffer_get_native_handle(p_command_buffer));
	if (command_list == nullptr || data->context == nullptr || data->dispatch == nullptr) {
		memdelete(data);
		return;
	}

	ffxDispatchDescUpscale desc = {};
	desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	desc.commandList = command_list;
	desc.color = ffxApiGetResourceDX12(data->color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.depth = ffxApiGetResourceDX12(data->depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.motionVectors = ffxApiGetResourceDX12(data->velocity, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.exposure = ffxApiGetResourceDX12(data->exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.reactive = ffxApiGetResourceDX12(data->reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	desc.transparencyAndComposition = ffxApiGetResourceDX12(nullptr);
	desc.output = ffxApiGetResourceDX12(data->output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

	// Same jitter / motion-vector convention as Godot's FSR 2 path. The motion vectors have already
	// had their sentinel (invalid) values resolved by MotionVectorsStore before this dispatch.
	desc.jitterOffset.x = data->jitter_x;
	desc.jitterOffset.y = data->jitter_y;
	desc.motionVectorScale.x = float(data->render_width);
	desc.motionVectorScale.y = float(data->render_height);
	desc.renderSize.width = data->render_width;
	desc.renderSize.height = data->render_height;
	desc.upscaleSize.width = data->upscale_width;
	desc.upscaleSize.height = data->upscale_height;
	desc.enableSharpening = data->sharpness > 1e-6f;
	desc.sharpness = data->sharpness;
	desc.frameTimeDelta = data->delta_time;
	desc.preExposure = 1.0f;
	desc.reset = data->reset;
	desc.cameraNear = data->z_near;
	desc.cameraFar = data->z_far;
	desc.cameraFovAngleVertical = data->fovy;
	desc.viewSpaceToMetersFactor = 1.0f;
	desc.flags = 0;

	ID3D12GraphicsCommandList *cmd_list = static_cast<ID3D12GraphicsCommandList *>(command_list);

	ffxReturnCode_t rc = data->dispatch(&data->context, &desc.header);

	// After FfxApi dispatch, Godot's command-buffer state tracker is stale.  FfxApi binds its own
	// descriptor heap, compute pipeline, and root signature during dispatch (ffx_dx12.cpp:3525-3528),
	// but Godot's CommandBufferInfo still thinks its own are bound and skips rebinding on the next
	// draw — causing a GPU crash with wrong PSO / root signature / heap bindings.
	//
	// We mirror what command_buffer_end() does: null out the cached PSO pointers, zero the root
	// signature CRCs, set pending_dyn_params so dynamic state gets re-issued, and clear
	// descriptor_heaps_set so heaps get re-bound.  This forces a full re-bind on the next draw.
	//
	// Offset derivation (CommandBufferInfo layout, x64 / MSVC default packing):
	//   SelfList              32   (0-31)
	//   ComPtrs (x5)          40   (32-71)
	//   graphics_pso ptr       8   (72-79)
	//   compute_pso  ptr       8   (80-87)
	//   uint32_ts (x2)         8   (88-95)
	//   DynParams             32   (96-127)
	//   pending_dyn_params     1   (128) +3pad → 131
	//   graphics_root_sig_crc  4   (132-135)
	//   compute_root_sig_crc   4   (136-139)
	//   alignment pad          4   (140-143)
	//   RenderPassState      592   (144-735)
	//   descriptor_heaps_set   1   (736)
	{
		uint64_t base = p_command_buffer.id;
		void *cmd_list_at_40 = *reinterpret_cast<void **>(base + 40);
		if (cmd_list_at_40 == static_cast<ID3D12GraphicsCommandList *>(command_list)) {
			*reinterpret_cast<ID3D12PipelineState **>(base + 72) = nullptr;  // graphics_pso
			*reinterpret_cast<ID3D12PipelineState **>(base + 80) = nullptr;  // compute_pso
			*reinterpret_cast<bool *>(base + 128) = true;                    // pending_dyn_params
			*reinterpret_cast<uint32_t *>(base + 132) = 0;                   // graphics_root_signature_crc
			*reinterpret_cast<uint32_t *>(base + 136) = 0;                   // compute_root_signature_crc
			*reinterpret_cast<bool *>(base + 736) = false;                   // descriptor_heaps_set
		} else {
			ERR_PRINT_ONCE("FSR 4: CommandBufferInfo layout mismatch -- state invalidation skipped.");
		}
	}

	// Insert a UAV barrier on the output resource to ensure all FfxApi compute writes are complete
	// before subsequent Godot render passes access the output.
	if (data->output != nullptr) {
		D3D12_RESOURCE_BARRIER uav_barrier = {};
		uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uav_barrier.UAV.pResource = data->output;
		cmd_list->ResourceBarrier(1, &uav_barrier);
	}

	if (rc != FFX_API_RETURN_OK) {
		ERR_PRINT_ONCE(vformat("FSR 4: ffxDispatch failed (FfxApi code %d).", (int)rc));
	}

	memdelete(data);
}

} // namespace

Vector<FSR4Effect::Provider> FSR4Effect::get_providers() {
	if (fsr4_providers_enumerated) {
		return fsr4_providers;
	}

	ID3D12Device *device = get_d3d12_device();
	if (device == nullptr || !device_supports_fsr(device)) {
		return fsr4_providers;
	}
	FfxRuntime *rt = get_ffx_runtime();
	if (rt == nullptr) {
		return fsr4_providers;
	}

	// First query returns the count; the second fills the id + name arrays. The loader filters the
	// list to the providers usable on this device (the query takes the D3D12 device).
	uint64_t version_count = 0;
	ffxQueryDescGetVersions versions_query = {};
	versions_query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	versions_query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	versions_query.device = device;
	versions_query.outputCount = &version_count;
	if (rt->query(nullptr, &versions_query.header) != FFX_API_RETURN_OK || version_count == 0) {
		return fsr4_providers;
	}

	const uint64_t max_versions = 16;
	version_count = MIN(version_count, max_versions);
	uint64_t version_ids[max_versions] = {};
	const char *version_names[max_versions] = {};
	versions_query.versionIds = version_ids;
	versions_query.versionNames = version_names;
	if (rt->query(nullptr, &versions_query.header) != FFX_API_RETURN_OK) {
		return fsr4_providers;
	}

	for (uint64_t i = 0; i < version_count; i++) {
		Provider provider;
		provider.version_id = version_ids[i];
		String name = version_names[i] != nullptr ? String(version_names[i]) : String();
		name = name.strip_edges();
		// The loader marks its default provider with a trailing "*"; drop it for a clean label.
		if (name.ends_with("*")) {
			name = name.trim_suffix("*").strip_edges();
		}
		provider.name = name.is_empty() ? String("FSR (unknown)") : name;
		fsr4_providers.push_back(provider);
	}
	fsr4_providers_enumerated = true;
	return fsr4_providers;
}

uint64_t FSR4Effect::resolve_provider_for_family(int p_family) {
	if (p_family == 0) {
		return 0; // Auto: let the loader pick its default provider.
	}
	// Provider names look like "4.1.0" / "3.1.3" / "2.3.2"; match on the major version. The list is
	// ordered newest-first, so the first match is the newest provider in that family.
	for (const Provider &provider : get_providers()) {
		if (provider.name.get_slicec('.', 0).to_int() == p_family) {
			return provider.version_id;
		}
	}
	return 0; // Family unavailable on this device; fall back to the loader default.
}

bool FSR4Effect::is_supported() {
	if (fsr4_supported != -1) {
		return fsr4_supported == 1;
	}
	fsr4_supported = 0; // The probe runs once; default to unsupported and flip on success.

	ID3D12Device *device = get_d3d12_device();
	if (device == nullptr) {
		print_verbose("FSR 4: not using the Direct3D 12 backend; FSR 4 is unavailable.");
		return false;
	}

	device_supports_fsr(device); // Log device capabilities.

	FfxRuntime *rt = get_ffx_runtime();
	if (rt == nullptr) {
		return false;
	}

	// Enumerate the upscaler provider versions available for this device (cached).
	Vector<Provider> providers = get_providers();

	// Prove the interop end-to-end: create and immediately destroy an upscale context on Godot's
	// D3D12 device.
	ffxCreateBackendDX12Desc backend_desc = {};
	backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend_desc.device = device;

	ffxCreateContextDescUpscale upscale_desc = {};
	upscale_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	upscale_desc.header.pNext = &backend_desc.header;
	upscale_desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
	upscale_desc.maxRenderSize = { 1920, 1080 };
	upscale_desc.maxUpscaleSize = { 3840, 2160 };
	upscale_desc.fpMessage = ffx_message_callback;

	ffxContext context = nullptr;
	ffxReturnCode_t rc = rt->create_context(&context, &upscale_desc.header, nullptr);
	if (rc != FFX_API_RETURN_OK || context == nullptr) {
		WARN_PRINT(vformat("FSR 4: failed to create an upscale context on the current device (FfxApi code %d). The GPU/driver may not support FSR 4.", (int)rc));
		return false;
	}

	ffxQueryGetProviderVersion provider_query = {};
	provider_query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
	if (rt->query(&context, &provider_query.header) == FFX_API_RETURN_OK) {
		if (provider_query.versionName != nullptr) {
			fsr4_provider_version = String(provider_query.versionName);
		} else if (provider_query.versionId != 0) {
			// The driver's provider query can omit the display name; recover it from the enumerated
			// list by matching the version id.
			for (const Provider &provider : providers) {
				if (provider.version_id == provider_query.versionId) {
					fsr4_provider_version = provider.name;
					break;
				}
			}
		}
	}

	rt->destroy_context(&context, nullptr);

	// Some drivers report neither a name nor a usable id for the active provider; fall back to the
	// loader's default, which is the first entry the enumeration returns.
	if (fsr4_provider_version.is_empty() && !providers.is_empty()) {
		fsr4_provider_version = providers[0].name;
	}

	fsr4_supported = 1;
	print_verbose(vformat("FSR 4: supported on this device. Selected provider: %s", fsr4_provider_version.is_empty() ? String("(unknown)") : fsr4_provider_version));
	return true;
}

String FSR4Effect::get_provider_version() {
	return fsr4_provider_version;
}

FSR4Effect::FSR4Effect() {}

FSR4Effect::~FSR4Effect() {}

FSR4Context *FSR4Effect::create_context(Size2i p_internal_size, Size2i p_target_size, uint64_t p_version_id) {
	FfxRuntime *rt = get_ffx_runtime();
	ERR_FAIL_NULL_V(rt, nullptr);

	ID3D12Device *device = get_d3d12_device();
	ERR_FAIL_NULL_V(device, nullptr);

	ffxCreateBackendDX12Desc backend_desc = {};
	backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend_desc.device = device;

	// Optionally force a specific provider version enumerated by get_providers(). Chained after the
	// backend desc; must outlive the create_context() call below (it does — same scope).
	ffxOverrideVersion override_version = {};
	if (p_version_id != 0) {
		override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		override_version.versionId = p_version_id;
		backend_desc.header.pNext = &override_version.header;
	}

	ffxCreateContextDescUpscale upscale_desc = {};
	upscale_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	upscale_desc.header.pNext = &backend_desc.header;
	// Match Godot's FSR 2 setup: HDR color and inverted (reverse-Z) depth, finite far plane.
	upscale_desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
	// maxRenderSize is the LARGEST render (internal) size that will ever be dispatched. The 3D
	// resolution scale can climb to 1.0 (internal == target), so size the context for the target.
	// This makes a per-frame render-scale change safe without dispatching past the context limits.
	upscale_desc.maxRenderSize = { (uint32_t)p_target_size.width, (uint32_t)p_target_size.height };
	upscale_desc.maxUpscaleSize = { (uint32_t)p_target_size.width, (uint32_t)p_target_size.height };
	upscale_desc.fpMessage = ffx_message_callback;

	ffxContext ffx_ctx = nullptr;
	ffxReturnCode_t rc = rt->create_context(&ffx_ctx, &upscale_desc.header, nullptr);
	if (rc != FFX_API_RETURN_OK || ffx_ctx == nullptr) {
		ERR_FAIL_V_MSG(nullptr, vformat("FSR 4: failed to create upscale context (FfxApi code %d).", (int)rc));
	}

	FSR4Context *context = memnew(FSR4Context);
	context->ffx_context = ffx_ctx;
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;
	context->version_id = p_version_id;
	print_verbose(vformat("FSR 4: context created (max render/upscale %dx%d, provider id %d).", p_target_size.width, p_target_size.height, (int64_t)p_version_id));
	return context;
}

void FSR4Effect::upscale(const Parameters &p_params) {
	ERR_FAIL_NULL(p_params.context);
	ERR_FAIL_NULL(p_params.context->ffx_context);

	FfxRuntime *rt = get_ffx_runtime();
	ERR_FAIL_NULL(rt);

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);

	// Resolve the native ID3D12Resource for each texture now; they stay valid until the render
	// graph replays the callback later this frame.
	FSR4DispatchData *data = memnew(FSR4DispatchData);
	data->context = p_params.context->ffx_context;
	data->dispatch = rt->dispatch;
	data->color = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.color));
	data->depth = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.depth));
	data->velocity = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.velocity));
	data->output = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.output));
	if (p_params.reactive.is_valid()) {
		data->reactive = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.reactive));
	}
	if (p_params.exposure.is_valid()) {
		data->exposure = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, p_params.exposure));
	}

	if (data->color == nullptr || data->depth == nullptr || data->velocity == nullptr || data->output == nullptr) {
		ERR_PRINT_ONCE("FSR 4: could not resolve native D3D12 resources for the upscale inputs.");
		memdelete(data);
		return;
	}

	data->render_width = (uint32_t)p_params.internal_size.width;
	data->render_height = (uint32_t)p_params.internal_size.height;
	data->upscale_width = (uint32_t)p_params.target_size.width;
	data->upscale_height = (uint32_t)p_params.target_size.height;
	data->jitter_x = p_params.jitter.x;
	data->jitter_y = p_params.jitter.y;
	data->sharpness = p_params.sharpness;
	data->delta_time = p_params.delta_time;
	data->z_near = p_params.z_near;
	data->z_far = p_params.z_far;
	data->fovy = p_params.fovy;
	data->reset = p_params.reset_accumulation;

	// Declare the resources so the render graph inserts the right barriers and orders the callback:
	// inputs sampled (SHADER_RESOURCE layout), output as storage (UNORDERED_ACCESS layout).
	RenderingDevice::CallbackResource res[5];
	uint32_t res_count = 0;
	auto add_resource = [&res, &res_count](RID p_rid, RenderingDevice::CallbackResourceUsage p_usage) {
		res[res_count].rid = p_rid;
		res[res_count].type = RenderingDevice::CALLBACK_RESOURCE_TYPE_TEXTURE;
		res[res_count].usage = p_usage;
		res_count++;
	};
	add_resource(p_params.color, RenderingDevice::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.depth, RenderingDevice::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	add_resource(p_params.velocity, RenderingDevice::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	if (p_params.exposure.is_valid()) {
		add_resource(p_params.exposure, RenderingDevice::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE);
	}
	add_resource(p_params.output, RenderingDevice::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE);

	rd->driver_callback_add(fsr4_dispatch_callback, data, VectorView<RenderingDevice::CallbackResource>(res, res_count));
}

FSR4Context::~FSR4Context() {
	if (ffx_context != nullptr) {
		FfxRuntime *rt = get_ffx_runtime();
		if (rt != nullptr) {
			// The context's GPU resources may still be referenced by an in-flight frame (e.g. the
			// render-scale slider recreated the buffers). Wait for the GPU before the driver frees them.
			wait_for_gpu_idle();
			ffxContext ctx = ffx_context;
			rt->destroy_context(&ctx, nullptr);
		}
		ffx_context = nullptr;
		print_verbose("FSR 4: context destroyed.");
	}
}

#endif // FSR4_ENABLED
