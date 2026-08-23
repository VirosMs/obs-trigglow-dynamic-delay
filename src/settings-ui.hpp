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

#include "delay-controller.hpp"

class QLabel;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QTimer;

// TrigglowDelayDock is the plugin's ONLY Qt-facing file. It is a thin view
// over DelayController: every button click calls straight into the
// controller's public Enable()/Disable()/Toggle()/SetDelaySeconds()/
// SetSafeMode(), and the dock repaints itself whenever the controller reports
// a status change — the exact same callback path used after a hotkey press,
// so the dock and the hotkeys can never show inconsistent state.
//
// Ownership: once passed to ObsFrontendBridge::AddDock() ->
// obs_frontend_add_dock_by_id(), OBS's dock system owns this widget's
// lifetime. plugin-main.cpp must not delete it manually.
namespace trigglow {

class TrigglowDelayDock : public QWidget {
	Q_OBJECT

public:
	explicit TrigglowDelayDock(DelayController &controller, QWidget *parent = nullptr);

private:
	void BuildUi();
	void OnStatusChanged(const DelayStatus &status);
	void RefreshFromStatus(const DelayStatus &status);

	// "Modo seguro" watchdog (docs/SPEC.md §3): if we've been stuck in
	// Applying for too long, tell the controller to give up cleanly instead
	// of leaving the dock showing "Applying..." forever. Lives here (not in
	// delay-controller) so that file can stay Qt-free — see its header.
	void ArmApplyWatchdog();
	void DisarmApplyWatchdog();

	DelayController &controller_;

	QLabel *stateLabel_ = nullptr;
	QLabel *detailLabel_ = nullptr;
	QSpinBox *secondsSpin_ = nullptr;
	QCheckBox *safeModeCheck_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	QPushButton *toggleButton_ = nullptr;
	QTimer *applyWatchdog_ = nullptr;
};

} // namespace trigglow
