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
#include <QLocale>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

#include "buffer-mode-controller.hpp"
#include "ui-theme.hpp"

class QLabel;
class QSpinBox;
class QBoxLayout;
class QCheckBox;
class QComboBox;
class QFrame;
class QProgressBar;
class QPushButton;
class QTimer;
class QResizeEvent;

// TrigglowDelayDock is the plugin's main Qt view. It is a thin view
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
// Look: ui-theme.hpp (tokens from OBS's palette, brand fonts, SVG icons), one
// style sheet set on the dock and recomputed when OBS switches theme. Widgets
// carry dynamic properties (tgRole, tgTone, tgVariant...) instead of their own
// setStyleSheet() calls, so colors live in one place.
//
// Ownership: once passed to ObsFrontendBridge::AddDock() ->
// obs_frontend_add_dock_by_id(), OBS's dock system owns this widget's
// lifetime. plugin-main.cpp must not delete it manually.
namespace trigglow {

class SceneComboBox;
class AuthManager;
class FieldIcon;
struct UpdateCheckResult;

class TrigglowDelayDock : public QWidget {
	Q_OBJECT

public:
	explicit TrigglowDelayDock(BufferModeController &bufferController, AuthManager &authManager,
				   QWidget *parent = nullptr);

	// Recomputes tokens, fonts and icons from the application palette (OBS's
	// active theme). Wired to OBS_FRONTEND_EVENT_THEME_CHANGED and to palette
	// change events; a no-op when nothing changed.
	void ApplyTheme(bool force = false);

protected:
	void changeEvent(QEvent *event) override;
	// Wheel guard: the mouse wheel over a combo/spin box that doesn't have
	// focus scrolls the dock instead of silently changing the live scene,
	// the seconds or the quality.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void BuildUi();
	void resizeEvent(QResizeEvent *event) override;
	// Tags `widget` with a QSS role (tgRole) and remembers its font role, so
	// ApplyTheme() can re-apply capitalization/letter spacing on theme change.
	void SetRole(QWidget *widget, const char *qssRole, ui::FontRole fontRole);
	void SetRole(QWidget *widget, const char *qssRole);
	QColor ToneColor(const QString &tone) const;
	void SetIcon(QLabel *label, ui::IconId icon, const QString &tone, int size);
	// The icon sits at `alignment` inside the button's (wider) icon size, so
	// the gap to the text is part of the icon box.
	void SetButtonIcon(QPushButton *button, ui::IconId icon, const QColor &color,
			   Qt::Alignment alignment = Qt::AlignLeft);
	// OBS themes give QPushButton a fixed height; the banner takes the
	// height of the layout inside it instead.
	void FitUpdateBanner();
	// Icons that don't depend on state (banner, footer, buttons).
	void ApplyStaticIcons();
	// On a real state change, announces `message` (headline and detail) to
	// screen readers -- assertively for errors.
	void AnnounceStatus(BufferModeState state, const QString &message);
	// A QAccessibleAnnouncementEvent on the status card (Qt 6.8+; earlier Qt
	// has no such event and this does nothing), only while a screen reader
	// is active.
	void Announce(const QString &message, bool assertive);
	QLocale UiLocale() const;
	// Reflows rows that don't fit side by side at the current dock width.
	void ApplyNarrowLayout(int width);
	void OnStatusChanged(const BufferModeStatus &status);
	void RefreshFromStatus(const BufferModeStatus &status);

	// Degradation notice (v0.6.0): asks BufferModeController::GetPipelineHealth()
	// (cheap, never blocks on the graphics thread) whether the buffer runs
	// below full quality, and shows, updates or hides the notice under the
	// status card's detail. Runs every second on healthTimer_ while the delay
	// is Filling or Active (the only states with a buffer) and on every
	// status change; anywhere else the notice is hidden and nothing is polled.
	void RefreshPipelineHealth(const BufferModeStatus &status);
	// The status card's accessible name -- state, detail and, while shown,
	// the degradation notice's title -- and its description (what the notice
	// says was lowered).
	void UpdateStatusAccessibleName();

	// Wired to authManager_'s own status-changed callback (same pattern as
	// bufferController_'s), so the account row and the Enable button's
	// visual disabled state update the moment login completes/expires --
	// without the user needing to touch any other control first. The real
	// enforcement is BufferModeController::Enable()'s own check (see
	// SetAuthorizationCheck in plugin-main.cpp); this is UX only.
	void RefreshAccountUi();

	// Single-shot fill countdown, armed by OnStatusChanged() once per fill
	// window -- when BufferModeStatus::fillGeneration changes while Filling --
	// for the seconds that window has to fill (same pattern DelayController's
	// own apply watchdog used).
	void ArmFillTimer(uint32_t seconds);
	void DisarmFillTimer();

	// Live "18 s left" countdown + progress bar shown while Filling --
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
	// hardware, pero a su eleccion": never blocks anything, just warns. It's
	// the RAM estimate (rung 0 of the buffer): what the buffer runs at right
	// now under load is the degradation notice's job.
	void RefreshFitEstimate();

	// Wired to reportBugButton_: opens ReportBugDialog (report-bug-dialog.hpp),
	// which collects a short description and submits a real support ticket
	// to trigglow.com with OBS's redacted log attached when ticked -- see
	// that dialog's own header comment. Modal, but never blocks Enable/
	// Disable or anything else outside of itself while open.
	void OnReportBugClicked();

	// Kicks off update-checker.hpp's CheckForUpdate() on a background
	// thread (same RunHttp-style pattern ReportBugDialog uses for its own
	// HTTP calls), once, from the constructor. A public GitHub API GET, no
	// new trigglow.com backend needed -- see that header's comment for why
	// this is the plugin's only update-awareness at all right now.
	void CheckForUpdateAsync();
	// Marshaled back onto the UI thread by CheckForUpdateAsync(); only ever
	// called when a real update is available (a failed/inconclusive check
	// just stays silent, never shown to the user -- see
	// UpdateCheckResult::checked's comment).
	void ShowUpdateNotice(const UpdateCheckResult &result);

	BufferModeController &bufferController_;
	AuthManager &authManager_;

	ui::Theme theme_;
	bool themeApplied_ = false;
	bool applyingTheme_ = false;
	std::vector<std::pair<QPointer<QWidget>, ui::FontRole>> fontRoles_;
	bool hasAnnouncedState_ = false;
	BufferModeState announcedState_ = BufferModeState::Inactive;

	// Status card: eyebrow + seconds chip, icon + headline, detail, and while
	// Filling a 4 px progress bar with "12 / 30 s" and "18 s left".
	QFrame *statusCard_ = nullptr;
	QFrame *statusRule_ = nullptr; // 2 px top rule: amber Filling, gradient Active, red Error.
	QLabel *statusChip_ = nullptr;
	QLabel *statusIcon_ = nullptr;
	QLabel *stateLabel_ = nullptr; // Headline.
	QLabel *detailLabel_ = nullptr;
	QWidget *fillRow_ = nullptr;
	QProgressBar *fillBar_ = nullptr;
	QLabel *fillProgressLabel_ = nullptr;
	QLabel *fillRemainingLabel_ = nullptr;
	// "Status: Delay on. Viewers see..." without the degradation notice.
	QString statusAccessibleName_;
	// Degradation notice (see RefreshPipelineHealth()): a hairline, the
	// sliders icon and title in the warning tone, what was lowered and why
	// (the delay itself never changes), and the repeat/drop rates when > 0.
	QWidget *healthNotice_ = nullptr;
	QLabel *healthIcon_ = nullptr;
	QLabel *healthTitle_ = nullptr;
	QLabel *healthBody_ = nullptr;
	QLabel *healthStats_ = nullptr;
	QTimer *healthTimer_ = nullptr;
	bool healthShown_ = false;
	// The cause the notice gave last (None while hidden): a new cause is
	// announced, a new rung for the same cause isn't.
	PipelineLoad healthCause_ = PipelineLoad::None;
	SceneComboBox *liveSceneCombo_ = nullptr;
	SceneComboBox *loadingSceneCombo_ = nullptr;
	QSpinBox *secondsSpin_ = nullptr;
	// "Turn the delay off to change it.": shown under the delay | quality row
	// while both are locked (Filling, Active).
	QWidget *delayLockedRow_ = nullptr;
	QLabel *delayLockedIcon_ = nullptr;
	// (field, lock): the lock drawn inside the live/loading scene pickers, the
	// seconds and the quality while they're locked (Filling, Active), where
	// their arrows were.
	std::vector<std::pair<QWidget *, FieldIcon *>> fieldLocks_;
	// "Calidad minima" -- floor on the delayed video's resolution; see
	// BufferModeStatus::minResolutionHeight / VideoDelayFilter::EnsureRingSized.
	QComboBox *minResolutionCombo_ = nullptr;
	// "Show overlay" -- BufferModeStatus::showOverlay (the "Delay 30s" text
	// shown on the delayed output while Active).
	QCheckBox *overlayCheck_ = nullptr;
	// The check box's text, as a word-wrapping label (a QCheckBox can't wrap,
	// which pushed narrow docks wider than their window). Clicking it toggles.
	QLabel *overlayText_ = nullptr;
	QComboBox *overlayCornerCombo_ = nullptr; // Which canvas corner the overlay sits in.
	// Rows that reflow to a column when the dock is narrow -- see ApplyNarrowLayout().
	QBoxLayout *accountLayout_ = nullptr; // Text | inline button; stacked when narrow.
	QBoxLayout *tuningRow_ = nullptr;
	QBoxLayout *buttonRow_ = nullptr;
	QBoxLayout *footerRow_ = nullptr;
	// Shows what secondsSpin_/minResolutionCombo_'s current combo would
	// actually achieve in RAM -- see RefreshFitEstimate(). fitRow_ holds both;
	// hidden while the degradation notice shows.
	QWidget *fitRow_ = nullptr;
	QLabel *fitIcon_ = nullptr;
	QLabel *fitLabel_ = nullptr;
	QLabel *budgetValue_ = nullptr;
	QPushButton *enableButton_ = nullptr;
	QPushButton *disableButton_ = nullptr;
	// Under a disabled Enable button: which of the two prerequisites is missing.
	QWidget *requirements_ = nullptr;
	QLabel *reqAccountIcon_ = nullptr;
	QLabel *reqAccountText_ = nullptr;
	QLabel *reqSceneIcon_ = nullptr;
	QLabel *reqSceneText_ = nullptr;
	QLabel *hintLabel_ = nullptr;
	QTimer *fillTimer_ = nullptr;

	// Free-account row (see RefreshAccountUi()): icon + text, an inline
	// secondary button (Sign out / Cancel) and, below, a full-width one
	// (Sign in / Try again, or Copy link when the browser didn't open).
	QLabel *accountIcon_ = nullptr;
	QLabel *accountLabel_ = nullptr;
	QPushButton *accountButton_ = nullptr;
	QPushButton *signInButton_ = nullptr;
	QPushButton *copyLinkButton_ = nullptr;
	bool linkCopied_ = false;

	// See OnReportBugClicked(). Always enabled, regardless of buffer state.
	QPushButton *reportBugButton_ = nullptr;

	// See CheckForUpdateAsync()/ShowUpdateNotice(). Hidden (setVisible(false))
	// until/unless a real update is found -- Qt layouts skip hidden widgets
	// entirely, so this reserves no space while empty. updateReleaseUrl_ is
	// what its click handler opens. The whole banner is one button.
	QPushButton *updateNoticeButton_ = nullptr;
	QLabel *updateIcon_ = nullptr;
	QLabel *updateTitle_ = nullptr;
	QLabel *updateVersion_ = nullptr;
	QLabel *updateExternalIcon_ = nullptr;
	QString updateReleaseUrl_;

	// See UpdateFillProgress()'s comment. fillElapsed_ is armed alongside
	// fillTimer_ in ArmFillTimer(); fillTotalSeconds_ is what the countdown
	// counts down FROM.
	QTimer *fillProgressTimer_ = nullptr;
	QElapsedTimer fillElapsed_;
	uint32_t fillTotalSeconds_ = 0;
	// The fill window fillTimer_ is armed for (BufferModeStatus::fillGeneration);
	// fillArmed_ is false whenever the timer is disarmed.
	uint32_t armedFillGeneration_ = 0;
	bool fillArmed_ = false;
};

} // namespace trigglow
