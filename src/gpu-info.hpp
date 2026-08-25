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

#pragma once

#include <cstdint>

// libobs's own public graphics API has no VRAM query at all -- verified,
// 2026-08-25: graphics.h's gs_enum_adapters() gives only a name and an
// index, nothing about memory size. OBS's own startup log prints "Dedicated
// VRAM: ..." for each adapter, but that comes from OBS core talking to
// DXGI/Metal/etc. directly internally, not from anything exposed to
// plugins. So this talks to the platform graphics API directly, the same
// way OBS itself does for that log line, just narrower (Windows/DXGI only
// for now -- this plugin's priority platform, see the download page).
namespace trigglow {

// Returns the largest dedicated VRAM (bytes) among the real (non-software)
// GPU adapters on this machine, or 0 if it can't be determined -- wrong
// adapter selection, query failure, or a non-Windows build. Callers must
// treat 0 as "unknown" and fall back to a conservative assumption, never as
// "this GPU has no VRAM."
uint64_t QueryDedicatedVramBytes();

} // namespace trigglow
