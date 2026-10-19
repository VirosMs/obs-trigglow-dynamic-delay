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

#include "support-diagnostics.hpp"

// Fills the optional "OBS technical data" of a Trigi conversation
// (support-diagnostics.hpp) from OBS and the plugin: versions, OS family,
// renderer, the GPU's vendor (from its PCI vendor ID, never its name), total
// RAM, the stream encoder's family (never its ID or name), the delay's state
// and settings, canvas and output sizes, fps, OBS's lag counters and the
// buffer pipeline's health. Scene names, sources, paths, URLs, keys and the
// like can't get in: the snapshot type has no field that could hold them.
namespace trigglow {

class BufferModeController;

// UI thread. Cheap: reads the controller's status (already on the UI thread)
// and OBS's video settings; never waits on OBS's graphics thread.
diag::Snapshot CollectDiagnostics(const BufferModeController &controller);

} // namespace trigglow
