/*
Trigglow Dynamic Delay for OBS
Copyright (C) 2026 Trigglow (VirosMs)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "gpu-info.hpp"

#ifdef _WIN32
#include <dxgi.h>
#endif

namespace trigglow {

#ifdef _WIN32

uint64_t QueryDedicatedVramBytes()
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
		return 0;

	// Largest real adapter, not the first one enumerated -- multi-GPU
	// machines (a discrete card plus an integrated one, this developer's
	// own dev box included) can otherwise pick the weaker adapter
	// depending on enumeration order, and there's no public API to ask
	// libobs which one it's actually rendering with.
	uint64_t largestVram = 0;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 desc = {};
		if (SUCCEEDED(adapter->GetDesc1(&desc))) {
			// Skip the Microsoft Basic Render Driver (software rasterizer
			// fallback) -- never the GPU actually doing the work.
			bool isSoftware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
			if (!isSoftware && static_cast<uint64_t>(desc.DedicatedVideoMemory) > largestVram)
				largestVram = desc.DedicatedVideoMemory;
		}
		adapter->Release();
		adapter = nullptr;
	}

	factory->Release();
	return largestVram;
}

#else

uint64_t QueryDedicatedVramBytes()
{
	// macOS/Linux: not implemented yet (Metal / VAAPI-or-similar would be
	// needed, each a separate platform-specific query) -- callers already
	// treat 0 as "unknown, use a conservative default", which is exactly
	// what happens here.
	return 0;
}

#endif

} // namespace trigglow
