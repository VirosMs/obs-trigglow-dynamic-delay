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

#include <functional>

#include <QComboBox>

namespace trigglow {

// A QComboBox that repopulates itself (via a caller-supplied callback)
// every time its dropdown is about to open, instead of only once at
// construction.
//
// Why this matters here: the dock is constructed during obs_module_load(),
// which runs BEFORE OBS finishes loading the user's scene collection from
// disk — a populate-once-at-startup scene combo can show nothing even when
// the user has scenes. Refreshing on every popup also naturally covers the
// user adding/renaming/deleting scenes while the dock stays open, which a
// one-time "wait for FinishedLoading" fix would not.
class SceneComboBox : public QComboBox {
	Q_OBJECT

public:
	explicit SceneComboBox(QWidget *parent = nullptr);

	// Called right before the dropdown opens. The callback owns clearing
	// and re-adding this combo's items (typically also restoring whatever
	// was selected — see TrigglowDelayDock::RefreshSceneList()).
	void SetRefreshCallback(std::function<void()> callback) { refreshCallback_ = std::move(callback); }

protected:
	void showPopup() override;

private:
	std::function<void()> refreshCallback_;
};

} // namespace trigglow
