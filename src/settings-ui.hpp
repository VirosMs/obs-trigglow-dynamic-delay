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

#include <QElapsedTimer>
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

	// Live "Ns restantes" countdown shown in stateLabel_ while Filling --
	// added 2026-08-26 after the MVP-complete pass: before this, Filling
	// just showed a static "Llenando buffer..." for the whole wait with no
	// indication of how much longer, which is exactly the kind of silent
	// wait a user has no way to distinguish from the plugin being stuck.
	// Ticks fillProgressTimer_ against fillElapsed_/fillTotalSeconds_;
	// doesn't touch anything RefreshFromStatus already owns (color,
	// controls-enabled state) since no real status change is happening,
	// just the passage of time within the same Filling status.
	void UpdateFillProgress();

	// Shared by liveSceneCombo_ and loadingSceneCombo_ — repopulates
	// `combo` from bufferController_.ListAvailableScenes() and reselects
	// `currentValue` without re-triggering signals.
	void RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption);

	// Runs RefreshSceneCombo() for both scene combos, re-selecting whatever
	// BufferModeController::GetStatus() currently holds. Wired to
	// BufferModeController::SetSceneListRefreshCallback in the constructor
	// so this fires automatically once OBS finishes loading the scene
	// collection -- see that callback's comment for the bug this fixes
	// (both combos coming up blank on every fresh OBS start/reinstall).
	void RefreshAvailableScenes();

	// Live-updates fitLabel_ from bufferController_.EstimateBufferFit() for
	// whatever's currently in secondsSpin_/minResolutionCombo_ -- called
	// whenever either changes, so the user sees the tradeoff of their
	// choice immediately, before pressing Enable. "Aconsejar segun el
	// hardware, pero a su eleccion": never blocks anything, just warns.
	void RefreshFitEstimate();

	BufferModeController &bufferController_;

	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	SceneComboBox *liveSceneCombo_ = nullptr;
	SceneComboBox *loadingSceneCombo_ = nullptr;
	QSpinBox *secondsSpin_ = nullptr;
	// "Calidad minima" -- floor on the delayed video's resolution; see
	// BufferModeStatus::minResolutionHeight / VideoDelayFilter::EnsureRingSized.
	QComboBox *minResolutionCombo_ = nullptr;
	// Shows what secondsSpin_/minResolutionCombo_'s current combo would
	// actually achieve -- see RefreshFitEstimate().
	QLabel *fitLabel_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	QTimer *fillTimer_ = nullptr;

	// See UpdateFillProgress()'s comment. fillElapsed_ is armed alongside
	// fillTimer_ in ArmFillTimer(); fillTotalSeconds_ is what the countdown
	// counts down FROM.
	QTimer *fillProgressTimer_ = nullptr;
	QElapsedTimer fillElapsed_;
	uint32_t fillTotalSeconds_ = 0;
};

} // namespace trigglow
