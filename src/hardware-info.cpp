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

#else

uint64_t QueryTotalSystemRamBytes()
{
	// macOS/Linux: not implemented yet (sysctlbyname("hw.memsize", ...) /
	// sysinfo(2) or parsing /proc/meminfo, each a separate platform query)
	// -- callers already treat 0 as "unknown, use a conservative default".
	return 0;
}

#endif

} // namespace trigglow
