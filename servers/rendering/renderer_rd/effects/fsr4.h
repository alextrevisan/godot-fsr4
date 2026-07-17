/**************************************************************************/
/*  fsr4.h                                                                */
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

#pragma once

#ifdef FSR4_ENABLED

#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"

// AMD FSR 4 integration (Direct3D 12 only). FSR 4 is a machine-learning temporal upscaler from the
// AMD FidelityFX SDK, shipped as signed DLLs and only reachable through the FidelityFX API on a
// native D3D12 device. The upscale is recorded into Godot's frame command list via the render
// graph's driver-callback mechanism (RenderingDevice::driver_callback_add).

namespace RendererRD {

class FSR4Context {
public:
	void *ffx_context = nullptr; // ffxContext (opaque).
	Size2i internal_size;
	Size2i target_size;

	~FSR4Context();
};

class FSR4Effect {
public:
	struct Parameters {
		FSR4Context *context = nullptr;
		Size2i internal_size;
		Size2i target_size;
		RID color;
		RID depth;
		RID velocity;
		RID reactive; // Optional (RID() to skip).
		RID exposure; // Optional (RID() to skip).
		RID output;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		Vector2 jitter;
		float delta_time = 0.0f;
		float sharpness = 0.0f;
		bool reset_accumulation = false;
	};

	// Probes (once, cached) whether FSR 4 can run: the backend is Direct3D 12, the FidelityFX
	// loader DLL is present and ABI-compatible, and an upscale context can be created on Godot's
	// D3D12 device. Logs the outcome the first time it runs.
	static bool is_supported();

	// Version string reported by the selected FSR upscaler provider (empty until a successful probe).
	static String get_provider_version();

	FSR4Effect();
	~FSR4Effect();

	// Creates a per-viewport upscale context sized for the given render/output resolutions.
	// Returns nullptr on failure.
	FSR4Context *create_context(Size2i p_internal_size, Size2i p_target_size);

	// Records an FSR 4 upscale for one view into the current frame via the render graph.
	void upscale(const Parameters &p_params);
};

} // namespace RendererRD

#endif // FSR4_ENABLED
