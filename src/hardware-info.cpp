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

#include "hardware-info.hpp"

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

namespace trigglow {

#ifdef _WIN32

uint64_t QueryTotalSystemRamBytes()
{
	MEMORYSTATUSEX status = {};
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status))
		return 0;
	return static_cast<uint64_t>(status.ullTotalPhys);
}

std::vector<GpuAdapterInfo> QueryGpuAdapters()
{
	std::vector<GpuAdapterInfo> adapters;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
		return adapters;
	for (UINT i = 0;; ++i) {
		IDXGIAdapter1 *adapter = nullptr;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			GpuAdapterInfo info;
			info.index = i;
			info.vendorId = desc.VendorId;
			info.dedicatedVideoMemory = static_cast<uint64_t>(desc.DedicatedVideoMemory);
			char name[256] = "";
			const int length = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
							       static_cast<int>(sizeof(name)), nullptr, nullptr);
			if (length > 0)
				info.name = name;
			adapters.push_back(std::move(info));
		}
		adapter->Release();
	}
	factory->Release();
	return adapters;
}

#else

uint64_t QueryTotalSystemRamBytes()
{
	// macOS/Linux: not implemented yet (sysctlbyname("hw.memsize", ...) /
	// sysinfo(2) or parsing /proc/meminfo, each a separate platform query)
	// -- callers already treat 0 as "unknown, use a conservative default".
	return 0;
}

std::vector<GpuAdapterInfo> QueryGpuAdapters()
{
	return {}; // v0.7.0: the hardware storage codec is Windows only.
}

#endif

} // namespace trigglow
