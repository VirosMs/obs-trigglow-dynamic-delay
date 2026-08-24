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
#include "delay-controller.hpp"

class QLabel;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QTimer;

// TrigglowDelayDock is the plugin's ONLY Qt-facing file. It is a thin view
// over DelayController and BufferModeController: every button click calls
// straight into a controller's public methods, and the dock repaints
// itself whenever a controller reports a status change — the exact same
// callback path used after a hotkey press, so the dock and the hotkeys can
// never show inconsistent state.
//
// Two modes, one panel (issue #173 phase 2 - previously the buffer mode
// required manually adding an OBS filter via the Filters dialog, which
// testing showed was confusing/unusable for a non-technical streamer). A
// combo at the top shows/hides one of two control groups; only one
// controller is ever "armed" at a time from the user's point of view, but
// both controllers exist independently the whole time (each owns its own
// settings/state regardless of which group is currently visible).
//
// Ownership: once passed to ObsFrontendBridge::AddDock() ->
// obs_frontend_add_dock_by_id(), OBS's dock system owns this widget's
// lifetime. plugin-main.cpp must not delete it manually.
namespace trigglow {

class SceneComboBox;

class TrigglowDelayDock : public QWidget {
	Q_OBJECT

public:
	explicit TrigglowDelayDock(DelayController &controller, BufferModeController &bufferController,
				   QWidget *parent = nullptr);

private:
	void BuildUi();

	// --- Reconnect mode (existing, v0.1.0) ---
	void OnStatusChanged(const DelayStatus &status);
	void RefreshFromStatus(const DelayStatus &status);
	// "Modo seguro" watchdog (docs/SPEC.md §3): if we've been stuck in
	// Applying for too long, tell the controller to give up cleanly instead
	// of leaving the dock showing "Applying..." forever. Lives here (not in
	// delay-controller) so that file can stay Qt-free — see its header.
	void ArmApplyWatchdog();
	void DisarmApplyWatchdog();

	// --- Buffer mode (new, phase 2) ---
	void OnBufferStatusChanged(const BufferModeStatus &status);
	void RefreshFromBufferStatus(const BufferModeStatus &status);
	// Single-shot fill countdown, armed for bufferController_.GetStatus().
	// delaySeconds when Filling starts — same pattern as applyWatchdog_
	// above, just a plain elapsed-time trigger instead of a "give up"
	// timeout (see BufferModeController::OnFillTimerElapsed()'s comment).
	void ArmFillTimer(uint32_t seconds);
	void DisarmFillTimer();

	// Shared by reconnectSceneCombo_, liveSceneCombo_, and
	// loadingSceneCombo_ — repopulates whichever SceneComboBox is passed
	// from controller_.ListAvailableScenes() (any of them works; both
	// controllers proxy the same ObsFrontendBridge::ListSceneNames()) and
	// reselects `currentValue` without re-triggering signals.
	void RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption);

	DelayController &controller_;
	BufferModeController &bufferController_;

	QWidget *modeSelectorRow_ = nullptr;
	// Index 0 = reconnect mode, 1 = buffer mode — matches the order the
	// two groups are added to the stack in BuildUi().
	QComboBox *modeCombo_ = nullptr;
	QWidget *reconnectGroup_ = nullptr;
	QWidget *bufferGroup_ = nullptr;

	// Reconnect mode widgets.
	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	QSpinBox *secondsSpin_ = nullptr;
	QCheckBox *safeModeCheck_ = nullptr;
	SceneComboBox *reconnectSceneCombo_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	QPushButton *toggleButton_ = nullptr;
	QTimer *applyWatchdog_ = nullptr;

	// Buffer mode widgets.
	QLabel *bufferStateLabel_ = nullptr;
	QLabel *bufferDetailLabel_ = nullptr;
	SceneComboBox *liveSceneCombo_ = nullptr;
	SceneComboBox *loadingSceneCombo_ = nullptr;
	QSpinBox *bufferSecondsSpin_ = nullptr;
	QPushButton *bufferEnableButton_ = nullptr;
	QPushButton *bufferDisableButton_ = nullptr;
	QTimer *fillTimer_ = nullptr;
};

} // namespace trigglow
