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

// Replaces the earlier gpu-info.hpp (VRAM/DXGI) after the ring buffer moved
// from one GPU texture per buffered frame to system RAM (2026-08-25) --
// see video-delay-filter.hpp's header comment for why. With only a small
// fixed number of GPU objects now (one capture texrender, a couple of
// staging surfaces, one playback texture), VRAM is no longer the
// constraint worth sizing the buffer against; system RAM is.
namespace trigglow {

// Returns total (not "available") physical RAM in bytes, or 0 if it can't
// be determined (non-Windows for now, or the query failed). Total rather
// than available/free: available fluctuates with whatever else is running
// and would make the recommended budget jump around for reasons unrelated
// to what this plugin itself needs; total is stable for the whole session.
// Callers must treat 0 as "unknown", never as "no RAM".
uint64_t QueryTotalSystemRamBytes();

} // namespace trigglow
