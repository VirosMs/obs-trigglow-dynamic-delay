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

#include <QHash>
#include <QImage>
#include <QString>

// The live "DELAY 30 s" overlay as ONE image the plugin paints itself with
// QPainter: ink plate, brand rule, Dynamic Delay mark, the bundled Archivo
// (or the system UI font when it isn't bundled) and, under it, the small
// "By Trigglow" signature in the bundled Geist Mono. It replaces v0.5.x's badge
// PNG + separate GDI+/FreeType text source, which used Segoe UI (Windows only),
// needed magic offsets to line the two up, and couldn't be centered outside
// Windows. Same pixels on Windows, macOS and Linux, and one OBS source to move.
//
// Geometry: overlay-layout.hpp (500x180 PNG, 2.5x the 200x72 design).
// Threading: QPainter text rendering on a QImage is only done on the Qt UI
// thread here -- which is where every BufferModeController call runs (dock,
// hotkeys via OBS_TASK_UI, frontend events) -- never on OBS's graphics thread.
namespace trigglow::ui {

// Paints the badge. `label` is drawn uppercase ("DELAY"), `value` as given
// ("30 s"), `signature` as given on a second, smaller line ("By Trigglow";
// nothing when empty). Shrinks each line to fit if a fallback font runs wider
// than the bundled ones.
QImage RenderDelayBadge(const QString &label, const QString &value, const QString &signature);

// Writes one PNG per seconds value into a folder (the plugin's config folder,
// "overlay/") and hands OBS its path. A value is (re)rendered the first time
// it is asked for in a session, and again whenever its texts change (OBS
// language switched) -- the files are tiny, at most one per value 1-60. Their
// names carry overlaylayout::kRenderVersion; the first render of a session
// deletes the badges of other versions left in the folder.
class DelayBadgeFiles {
public:
	explicit DelayBadgeFiles(QString directory);

	// UTF-8 path of the PNG for `seconds`, rendering it if needed. Empty if it
	// couldn't be written (logged once per value), or when called off the UI
	// thread with nothing rendered yet.
	QString PathFor(uint32_t seconds, const QString &label, const QString &value, const QString &signature);

private:
	void RemoveStaleFiles();

	QString directory_;
	bool staleFilesRemoved_ = false;
	QHash<uint32_t, QString> renderedKeys_; // seconds -> texts + fonts they were rendered with.
	QHash<uint32_t, bool> warned_;
};

} // namespace trigglow::ui
