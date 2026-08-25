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

#include <QWidget>

#include "buffer-mode-controller.hpp"

class QLabel;
class QSpinBox;
class QComboBox;
class QPushButton;
class QTimer;

// TrigglowDelayDock is the plugin's ONLY Qt-facing file. It is a thin view
// over BufferModeController: every button click calls straight into the
// controller's public methods, and the dock repaints itself whenever the
// controller reports a status change — the exact same callback path used
// after a hotkey press, so the dock and the hotkeys can never show
// inconsistent state.
//
// Buffer mode (no-reconnect delay, issue #173) only, as of 2026-08-24 —
// reconnect mode (native obs_output_set_delay, DelayController) is no
// longer wired into the dock or hotkeys after live testing confirmed
// buffer mode works well and having two mechanisms + a mode selector was
// confusing for a non-technical streamer. DelayController/its hotkeys
// aren't deleted from the repo (may come back), just not instantiated —
// see plugin-main.cpp.
//
// Ownership: once passed to ObsFrontendBridge::AddDock() ->
// obs_frontend_add_dock_by_id(), OBS's dock system owns this widget's
// lifetime. plugin-main.cpp must not delete it manually.
namespace trigglow {

class SceneComboBox;

class TrigglowDelayDock : public QWidget {
	Q_OBJECT

public:
	explicit TrigglowDelayDock(BufferModeController &bufferController, QWidget *parent = nullptr);

private:
	void BuildUi();
	void OnStatusChanged(const BufferModeStatus &status);
	void RefreshFromStatus(const BufferModeStatus &status);

	// Single-shot fill countdown, armed for bufferController_.GetStatus().
	// delaySeconds when Filling starts (same pattern DelayController's own
	// apply watchdog used).
	void ArmFillTimer(uint32_t seconds);
	void DisarmFillTimer();

	// Shared by liveSceneCombo_ and loadingSceneCombo_ — repopulates
	// `combo` from bufferController_.ListAvailableScenes() and reselects
	// `currentValue` without re-triggering signals.
	void RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption);

	BufferModeController &bufferController_;

	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	SceneComboBox *liveSceneCombo_ = nullptr;
	SceneComboBox *loadingSceneCombo_ = nullptr;
	QSpinBox *secondsSpin_ = nullptr;
	// "Calidad minima" -- floor on the delayed video's resolution; see
	// BufferModeStatus::minResolutionHeight / VideoDelayFilter::EnsureRingSized.
	QComboBox *minResolutionCombo_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	QTimer *fillTimer_ = nullptr;
};

} // namespace trigglow
