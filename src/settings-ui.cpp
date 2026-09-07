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

#include "settings-ui.hpp"
#include "auth-manager.hpp"
#include "i18n.hpp"
#include "logging.hpp"
#include "report-bug-dialog.hpp"
#include "scene-combo-box.hpp"
#include "update-checker.hpp"

#include <plugin-support.h>

#include <algorithm>
#include <thread>

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace trigglow {

namespace {
constexpr const char *kComponent = "settings-ui";

// Semantic status/accent colors -- 2026-09-07 redesign pass (ui-ux-pro-max
// skill's "Dark Mode (OLED)" developer-tool design system, adapted by hand
// since Qt Widgets/QSS isn't one of that skill's output stacks). Fixed
// hex, unlike everything else in this file's styling (which uses
// palette(...) roles so the dock always matches whatever OBS theme --
// Yami, Grey, System, a light theme -- the user actually has active):
// status meaning has to read the same regardless of theme, so these stay
// constant instead of following palette().
constexpr const char *kColorSuccess = "#22C55E"; // Active / logged in / fits the full request.
constexpr const char *kColorWarning = "#F59E0B"; // Filling / logging in / duration got trimmed / update available.
constexpr const char *kColorError = "#EF4444";   // Error state, and Disable's button tint.
constexpr const char *kColorAccent = "#38BDF8";  // Links/secondary actions (account hover, report-bug button).

// Str() (i18n.hpp) returns const char*/UTF-8 -- every user-facing string in
// this file goes through this tiny wrapper instead of a bare
// QString::fromUtf8(Str(...)) at each call site.
QString T(const char *key)
{
	return QString::fromUtf8(Str(key));
}
} // namespace

TrigglowDelayDock::TrigglowDelayDock(BufferModeController &bufferController, AuthManager &authManager, QWidget *parent)
	: QWidget(parent),
	  bufferController_(bufferController),
	  authManager_(authManager)
{
	BuildUi();
	CheckForUpdateAsync();

	bufferController_.SetStatusChangedCallback([this](const BufferModeStatus &status) { OnStatusChanged(status); });
	bufferController_.SetSceneListRefreshCallback([this] { RefreshAvailableScenes(); });
	authManager_.SetStatusChangedCallback([this] { RefreshAccountUi(); });
	// Cascades into RefreshFromStatus() itself -- see RefreshAccountUi()'s
	// comment -- so this alone initializes the whole dock's state.
	RefreshAccountUi();
}

void TrigglowDelayDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	// Small, uppercased field labels ("ESCENA EN DIRECTO") placed ABOVE
	// their control rather than beside it -- reads better than a label+field
	// horizontal row once the panel narrows to a typical dock width
	// (~300-380px), and matches how most native settings panels group a
	// label with the field it describes.
	auto makeSectionLabel = [this](const QString &text) {
		auto *label = new QLabel(text.toUpper(), this);
		label->setObjectName(QStringLiteral("sectionLabel"));
		return label;
	};

	// --- Update notice: hidden until/unless CheckForUpdateAsync() (fired
	// from the constructor) actually finds a newer published release. Above
	// even the status card -- rare and important when it does show, and
	// costs nothing (no reserved space) while hidden, same as Qt skipping
	// any hidden widget in a layout. ---
	updateNoticeButton_ = new QPushButton(this);
	updateNoticeButton_->setObjectName(QStringLiteral("updateNoticeButton"));
	updateNoticeButton_->setCursor(Qt::PointingHandCursor);
	updateNoticeButton_->setVisible(false);
	root->addWidget(updateNoticeButton_);
	connect(updateNoticeButton_, &QPushButton::clicked, this, [this] {
		if (!updateReleaseUrl_.isEmpty())
			QDesktopServices::openUrl(QUrl(updateReleaseUrl_));
	});

	// --- Status card: state + detail/countdown, always the first thing the
	// user sees -- unchanged content, just now visually separated from
	// everything below it instead of floating at the top of one long column.
	auto *statusCard = new QFrame(this);
	statusCard->setObjectName(QStringLiteral("card"));
	auto *statusLayout = new QVBoxLayout(statusCard);
	statusLayout->setContentsMargins(12, 10, 12, 10);
	statusLayout->setSpacing(3);
	stateLabel_ = new QLabel(this);
	statusLayout->addWidget(stateLabel_);
	detailLabel_ = new QLabel(this);
	detailLabel_->setWordWrap(true);
	detailLabel_->setObjectName(QStringLiteral("mutedLabel"));
	statusLayout->addWidget(detailLabel_);
	root->addWidget(statusCard);

	// --- Free-account row -- see RefreshAccountUi(). Its own card, placed
	// above the scene pickers, not below the buttons, so it's the first
	// thing a logged-out user notices, before they've configured anything
	// and hit an Error state at Enable() time. ---
	auto *accountCard = new QFrame(this);
	accountCard->setObjectName(QStringLiteral("card"));
	auto *accountLayout = new QHBoxLayout(accountCard);
	accountLayout->setContentsMargins(12, 10, 12, 10);
	accountLabel_ = new QLabel(this);
	accountLabel_->setWordWrap(true);
	accountButton_ = new QPushButton(this);
	accountButton_->setObjectName(QStringLiteral("accountButton"));
	accountLayout->addWidget(accountLabel_, /*stretch=*/1);
	accountLayout->addWidget(accountButton_);
	root->addWidget(accountCard);

	// --- Configuration card: every control the user tweaks before pressing
	// Enable, grouped together instead of scattered as separate rows. ---
	auto *configCard = new QFrame(this);
	configCard->setObjectName(QStringLiteral("card"));
	auto *configLayout = new QVBoxLayout(configCard);
	configLayout->setContentsMargins(12, 10, 12, 10);
	configLayout->setSpacing(6);

	configLayout->addWidget(makeSectionLabel(T("Dock.Scene.Live")));
	liveSceneCombo_ = new SceneComboBox(this);
	liveSceneCombo_->setToolTip(T("Dock.Scene.Live.Tooltip"));
	liveSceneCombo_->SetRefreshCallback(
		[this] { RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false); });
	configLayout->addWidget(liveSceneCombo_);
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);

	configLayout->addWidget(makeSectionLabel(T("Dock.Scene.Loading")));
	loadingSceneCombo_ = new SceneComboBox(this);
	loadingSceneCombo_->setToolTip(T("Dock.Scene.Loading.Tooltip"));
	loadingSceneCombo_->SetRefreshCallback([this] {
		RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	});
	configLayout->addWidget(loadingSceneCombo_);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);

	// Delay + quality side by side -- two related, similarly-sized controls;
	// no reason to spend a full row each in a narrow dock.
	auto *tuningRow = new QHBoxLayout();
	tuningRow->setSpacing(10);

	auto *delayColumn = new QVBoxLayout();
	delayColumn->setSpacing(4);
	delayColumn->addWidget(makeSectionLabel(T("Dock.Delay.Label")));
	secondsSpin_ = new QSpinBox(this);
	secondsSpin_->setRange(1, 60);
	secondsSpin_->setValue(static_cast<int>(bufferController_.GetStatus().delaySeconds));
	secondsSpin_->setToolTip(T("Dock.Delay.Tooltip"));
	delayColumn->addWidget(secondsSpin_);
	tuningRow->addLayout(delayColumn, /*stretch=*/1);

	auto *qualityColumn = new QVBoxLayout();
	qualityColumn->setSpacing(4);
	qualityColumn->addWidget(makeSectionLabel(T("Dock.Quality.Label")));
	minResolutionCombo_ = new QComboBox(this);
	minResolutionCombo_->addItem(QStringLiteral("480p"), 480);
	minResolutionCombo_->addItem(QStringLiteral("720p"), 720);
	minResolutionCombo_->addItem(QStringLiteral("1080p"), 1080);
	minResolutionCombo_->setToolTip(T("Dock.Quality.Tooltip"));
	qualityColumn->addWidget(minResolutionCombo_);
	tuningRow->addLayout(qualityColumn, /*stretch=*/1);

	configLayout->addLayout(tuningRow);

	// Live-updated by RefreshFitEstimate() whenever secondsSpin_/
	// minResolutionCombo_ change -- see that method and
	// BufferModeController::EstimateBufferFit's comment.
	fitLabel_ = new QLabel(this);
	fitLabel_->setWordWrap(true);
	configLayout->addWidget(fitLabel_);

	// Informational only, computed once from real hardware where possible
	// (VideoDelayFilter::GetBufferBudgetBytes, see src/hardware-info.hpp) --
	// "aconsejar segun el hardware, pero a su eleccion": never restricts
	// delaySeconds/minResolutionHeight above, just shows the user what their
	// choices are actually working with.
	uint64_t budgetMb = bufferController_.GetBufferBudgetBytes() / (1024 * 1024);
	auto *budgetLabel = new QLabel(T("Dock.Budget.Label").arg(budgetMb), this);
	budgetLabel->setWordWrap(true);
	budgetLabel->setObjectName(QStringLiteral("mutedLabel"));
	configLayout->addWidget(budgetLabel);

	root->addWidget(configCard);

	// --- Primary actions: color-coded (green go / red stop) so their
	// purpose reads at a glance, not just from their labels. ---
	auto *buttonRow = new QHBoxLayout();
	buttonRow->setSpacing(8);
	enableButton_ = new QPushButton(T("Dock.Enable"), this);
	enableButton_->setObjectName(QStringLiteral("enableButton"));
	disableButton_ = new QPushButton(T("Dock.Disable"), this);
	disableButton_->setObjectName(QStringLiteral("disableButton"));
	buttonRow->addWidget(enableButton_);
	buttonRow->addWidget(disableButton_);
	root->addLayout(buttonRow);

	auto *hint = new QLabel(T("Dock.Hint"), this);
	hint->setWordWrap(true);
	hint->setObjectName(QStringLiteral("mutedLabel"));
	root->addWidget(hint);

	// Always visible/enabled, independent of buffer state -- a user hitting
	// lag or a stuck buffer needs this reachable exactly when things are
	// going wrong, not just while Inactive. See OnReportBugClicked(). Styled
	// as a flat, link-like button (not a full QPushButton box) so it doesn't
	// visually compete with Enable/Disable for attention -- this is a rare,
	// secondary action, not a primary one.
	auto *reportRow = new QHBoxLayout();
	reportRow->addStretch(1);
	reportBugButton_ = new QPushButton(T("Dock.ReportBug.Button"), this);
	reportBugButton_->setObjectName(QStringLiteral("reportBugButton"));
	reportBugButton_->setFlat(true);
	reportBugButton_->setCursor(Qt::PointingHandCursor);
	reportBugButton_->setToolTip(T("Dock.ReportBug.Tooltip"));
	reportRow->addWidget(reportBugButton_);
	root->addLayout(reportRow);

	root->addStretch(1);

	// Single consolidated stylesheet for everything above, keyed by
	// objectName -- every color here is either palette(...) (follows
	// whatever OBS theme is active) or one of the semantic kColor* constants
	// above (status meaning, deliberately theme-independent). stateLabel_'s
	// color is set per-status in RefreshFromStatus() instead of here (it's
	// the one thing that actually changes color at runtime).
	setStyleSheet(QStringLiteral(
		"QFrame#card { background-color: palette(base); border: 1px solid palette(mid); border-radius: 8px; }"
		"QLabel#sectionLabel { color: palette(placeholderText); font-size: 8pt; font-weight: 600; }"
		"QLabel#mutedLabel { color: palette(placeholderText); font-size: 8pt; }"
		"QComboBox, QSpinBox { padding: 4px 6px; border: 1px solid palette(mid); border-radius: 5px; "
		"background-color: palette(window); }"
		"QPushButton#enableButton { background-color: %1; color: #FFFFFF; border: none; "
		"border-radius: 6px; padding: 8px; font-weight: 600; font-size: 9pt; }"
		"QPushButton#enableButton:hover { background-color: #16A34A; }"
		"QPushButton#enableButton:disabled { background-color: palette(button); color: palette(placeholderText); }"
		"QPushButton#disableButton { background-color: %2; color: #FFFFFF; border: none; "
		"border-radius: 6px; padding: 8px; font-weight: 600; font-size: 9pt; }"
		"QPushButton#disableButton:hover { background-color: #DC2626; }"
		"QPushButton#disableButton:disabled { background-color: palette(button); color: palette(placeholderText); }"
		"QPushButton#accountButton { background: transparent; border: 1px solid palette(mid); "
		"border-radius: 6px; padding: 5px 12px; }"
		"QPushButton#accountButton:hover { border-color: %3; color: %3; }"
		"QPushButton#accountButton:disabled { color: palette(placeholderText); }"
		"QPushButton#reportBugButton { color: %3; background: transparent; border: none; padding: 2px 4px; "
		"font-size: 9pt; text-decoration: underline; }"
		"QPushButton#reportBugButton:hover { color: #7DD3FC; }"
		"QPushButton#reportBugButton:disabled { color: palette(placeholderText); text-decoration: none; }"
		"QPushButton#updateNoticeButton { background-color: %4; color: #1A1200; border: none; "
		"border-radius: 6px; padding: 6px 10px; font-weight: 600; font-size: 9pt; text-align: left; }"
		"QPushButton#updateNoticeButton:hover { background-color: #D97706; }")
			.arg(QString::fromUtf8(kColorSuccess), QString::fromUtf8(kColorError),
			     QString::fromUtf8(kColorAccent), QString::fromUtf8(kColorWarning)));

	// stateLabel_'s color is per-status (RefreshFromStatus()); font here is
	// the constant part.
	stateLabel_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13pt;"));

	connect(liveSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		bufferController_.SetLiveScene(index < 0 ? std::string{}
							 : liveSceneCombo_->currentText().toStdString());
		RefreshFitEstimate(); // Different scene = different resolution/fps to estimate against.
	});
	connect(loadingSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		bufferController_.SetLoadingScene(index <= 0 ? std::string{}
							     : loadingSceneCombo_->currentText().toStdString());
	});
	connect(secondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
		bufferController_.SetDelaySeconds(static_cast<uint32_t>(value));
		RefreshFitEstimate();
	});
	connect(minResolutionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (index < 0)
			return;
		bufferController_.SetMinResolutionHeight(
			static_cast<uint32_t>(minResolutionCombo_->itemData(index).toInt()));
		RefreshFitEstimate();
	});
	connect(enableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Enable pressed in dock");
		bufferController_.Enable();
	});
	connect(disableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Disable pressed in dock");
		bufferController_.Disable();
	});
	connect(accountButton_, &QPushButton::clicked, this, [this] {
		if (authManager_.IsLoggedIn()) {
			authManager_.Logout();
		} else if (!authManager_.IsLoggingIn()) {
			TRIGGLOW_LOG_INFO(kComponent, "Sign in pressed in dock");
			authManager_.StartLogin();
		}
	});
	connect(reportBugButton_, &QPushButton::clicked, this, [this] { OnReportBugClicked(); });

	fillTimer_ = new QTimer(this);
	fillTimer_->setSingleShot(true);
	connect(fillTimer_, &QTimer::timeout, this, [this] { bufferController_.OnFillTimerElapsed(); });

	fillProgressTimer_ = new QTimer(this);
	connect(fillProgressTimer_, &QTimer::timeout, this, [this] { UpdateFillProgress(); });

	RefreshFitEstimate();
}

void TrigglowDelayDock::RefreshFitEstimate()
{
	auto estimate =
		bufferController_.EstimateBufferFit(static_cast<uint32_t>(secondsSpin_->value()),
						    static_cast<uint32_t>(minResolutionCombo_->currentData().toInt()));

	if (estimate.width == 0 || estimate.height == 0) {
		// No live scene chosen yet, or it doesn't resolve -- nothing
		// concrete to estimate against.
		fitLabel_->setText(T("Dock.Fit.ChooseScene"));
		fitLabel_->setStyleSheet(QStringLiteral("color: palette(placeholderText); font-size: 8pt;"));
		return;
	}

	if (estimate.fitsFullDuration) {
		fitLabel_->setText(T("Dock.Fit.FullyFits")
					   .arg(secondsSpin_->value())
					   .arg(estimate.width)
					   .arg(estimate.height));
		fitLabel_->setStyleSheet(
			QStringLiteral("color: %1; font-size: 8pt;").arg(QString::fromUtf8(kColorSuccess)));
	} else {
		fitLabel_->setText(T("Dock.Fit.Trimmed")
					    .arg(estimate.actualSeconds, 0, 'f', 1)
					    .arg(secondsSpin_->value())
					    .arg(estimate.width)
					    .arg(estimate.height));
		fitLabel_->setStyleSheet(
			QStringLiteral("color: %1; font-size: 8pt;").arg(QString::fromUtf8(kColorWarning)));
	}
}

void TrigglowDelayDock::OnStatusChanged(const BufferModeStatus &status)
{
	RefreshFromStatus(status);

	if (status.state == BufferModeState::Filling)
		ArmFillTimer(status.delaySeconds);
	else
		DisarmFillTimer();
}

void TrigglowDelayDock::RefreshFromStatus(const BufferModeStatus &status)
{
	QString stateText;
	QString color;
	switch (status.state) {
	case BufferModeState::Inactive:
		stateText = T("Dock.Status.Inactive");
		color = QStringLiteral("palette(text)");
		break;
	case BufferModeState::Filling:
		stateText = T("Dock.Status.Filling");
		color = QString::fromUtf8(kColorWarning);
		break;
	case BufferModeState::Active:
		stateText = T("Dock.Status.Active").arg(status.delaySeconds);
		color = QString::fromUtf8(kColorSuccess);
		break;
	case BufferModeState::Error:
		stateText = T("Dock.Status.Error");
		color = QString::fromUtf8(kColorError);
		break;
	}
	stateLabel_->setText(stateText);
	stateLabel_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13pt; color: %1;").arg(color));

	detailLabel_->setText(QString::fromStdString(status.message));
	detailLabel_->setVisible(!status.message.empty());

	bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	liveSceneCombo_->setEnabled(!busy);
	loadingSceneCombo_->setEnabled(!busy);
	secondsSpin_->setEnabled(status.state != BufferModeState::Filling);
	minResolutionCombo_->setEnabled(!busy);
	// Also requires a live scene chosen -- previously Enable was clickable
	// with no scene selected, which just bounced back into an Error state
	// ("Elige primero una escena en directo") instead of preventing the
	// click in the first place (2026-08-26 UX pass). Same reasoning for the
	// free-account gate: BufferModeController::Enable() already refuses and
	// sets Error if authManager_ says logged out, but disabling the button
	// up front is a clearer signal than a click that visibly bounces.
	enableButton_->setEnabled(!busy && !status.liveSceneName.empty() && authManager_.IsLoggedIn());
	disableButton_->setEnabled(busy);

	const QSignalBlocker blockSeconds(secondsSpin_);
	secondsSpin_->setValue(static_cast<int>(status.delaySeconds));

	const QSignalBlocker blockQuality(minResolutionCombo_);
	int qualityIndex = minResolutionCombo_->findData(static_cast<int>(status.minResolutionHeight));
	if (qualityIndex >= 0)
		minResolutionCombo_->setCurrentIndex(qualityIndex);

	const QSignalBlocker blockLive(liveSceneCombo_);
	int liveIndex = liveSceneCombo_->findText(QString::fromStdString(status.liveSceneName));
	if (liveIndex >= 0)
		liveSceneCombo_->setCurrentIndex(liveIndex);

	const QSignalBlocker blockLoading(loadingSceneCombo_);
	int loadingIndex = status.loadingSceneName.empty()
				   ? 0
				   : loadingSceneCombo_->findText(QString::fromStdString(status.loadingSceneName));
	loadingSceneCombo_->setCurrentIndex(loadingIndex >= 0 ? loadingIndex : 0);

	// The signal blockers above mean secondsSpin_/minResolutionCombo_/
	// liveSceneCombo_ may have just changed to their real values without
	// RefreshFitEstimate() having run for them yet -- refresh explicitly so
	// the estimate is never stale.
	if (fitLabel_)
		RefreshFitEstimate();
}

void TrigglowDelayDock::RefreshAccountUi()
{
	if (authManager_.IsLoggedIn()) {
		QString name = QString::fromStdString(authManager_.DisplayName());
		accountLabel_->setText(name.isEmpty() ? T("Dock.Account.FreeAccountConnected")
						      : T("Dock.Account.SignedInAs").arg(name));
		accountLabel_->setStyleSheet(
			QStringLiteral("color: %1; font-size: 9pt;").arg(QString::fromUtf8(kColorSuccess)));
		accountButton_->setText(T("Dock.Account.SignOut"));
		accountButton_->setEnabled(true);
	} else if (authManager_.IsLoggingIn()) {
		accountLabel_->setText(T("Dock.Account.WaitingConfirmation"));
		accountLabel_->setStyleSheet(
			QStringLiteral("color: %1; font-size: 9pt;").arg(QString::fromUtf8(kColorWarning)));
		accountButton_->setText(T("Dock.Account.Waiting"));
		accountButton_->setEnabled(false);
	} else {
		accountLabel_->setText(T("Dock.Account.RequireLogin"));
		accountLabel_->setStyleSheet(QStringLiteral("color: palette(windowText); font-size: 9pt;"));
		accountButton_->setText(T("Dock.Account.SignIn"));
		accountButton_->setEnabled(true);
	}

	// Re-derives the Enable button's visual enabled state too (real
	// enforcement lives in BufferModeController::Enable() itself, see
	// SetAuthorizationCheck) -- single source of truth for that condition,
	// see RefreshFromStatus()'s enableButton_ line.
	RefreshFromStatus(bufferController_.GetStatus());
}

void TrigglowDelayDock::ArmFillTimer(uint32_t seconds)
{
	if (!fillTimer_)
		return;
	fillTimer_->setInterval(static_cast<int>(seconds) * 1000);
	fillTimer_->start();

	fillTotalSeconds_ = seconds;
	fillElapsed_.start();
	if (fillProgressTimer_)
		fillProgressTimer_->start(250);
	UpdateFillProgress(); // Show "Ns restantes" immediately, not after the first 250ms tick.
}

void TrigglowDelayDock::DisarmFillTimer()
{
	if (fillTimer_)
		fillTimer_->stop();
	if (fillProgressTimer_)
		fillProgressTimer_->stop();
}

void TrigglowDelayDock::UpdateFillProgress()
{
	if (!stateLabel_ || fillTotalSeconds_ == 0)
		return;
	double elapsedSeconds = fillElapsed_.elapsed() / 1000.0;
	double remaining = std::max(0.0, static_cast<double>(fillTotalSeconds_) - elapsedSeconds);
	// Only touches the TEXT, never the color/stylesheet -- RefreshFromStatus
	// already set the amber Filling color once and owns it; this just
	// updates what number is shown while that same status holds.
	stateLabel_->setText(T("Dock.Status.Filling.Countdown").arg(remaining, 0, 'f', 0));
}

void TrigglowDelayDock::RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption)
{
	const QSignalBlocker block(combo);
	QString previousValue = QString::fromStdString(currentValue);
	combo->clear();
	if (includeNoneOption)
		combo->addItem(T("Dock.NoneOption"));
	for (const auto &name : bufferController_.ListAvailableScenes())
		combo->addItem(QString::fromStdString(name));

	if (!previousValue.isEmpty()) {
		int idx = combo->findText(previousValue);
		if (idx >= 0)
			combo->setCurrentIndex(idx);
	}
}

void TrigglowDelayDock::RefreshAvailableScenes()
{
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	// The combos above are repopulated under a QSignalBlocker (see
	// RefreshSceneCombo), so selecting the live scene for the first time
	// here won't have triggered RefreshFitEstimate() via the normal
	// currentIndexChanged path -- refresh it explicitly so the estimate
	// isn't left showing "elige una escena" when one was actually restored.
	RefreshFitEstimate();
}

void TrigglowDelayDock::OnReportBugClicked()
{
	TRIGGLOW_LOG_INFO(kComponent, "Report a problem pressed in dock");

	// ReportBugDialog does everything itself: copies the current OBS log,
	// collects a short description, submits a real support ticket (category
	// "dynamic_delay") with that log attached, and opens the resulting
	// ticket in the browser on success -- see its own header comment. exec()
	// (modal) is fine here: this is a deliberate, occasional action, not
	// something that needs to coexist with using the rest of the dock.
	ReportBugDialog dialog(QString::fromUtf8(PLUGIN_VERSION), QString::fromStdString(authManager_.DisplayName()),
			       this);
	dialog.exec();
}

void TrigglowDelayDock::CheckForUpdateAsync()
{
	// Same background-thread + QPointer + QMetaObject::invokeMethod(qApp, ...)
	// pattern AuthManager::RunHttp and ReportBugDialog's own submit both use
	// -- CheckForUpdate() is a BLOCKING network call (update-checker.hpp),
	// must never run on this (the UI) thread.
	QPointer<TrigglowDelayDock> self(this);
	std::string currentVersion = PLUGIN_VERSION;
	std::thread([currentVersion, self]() {
		UpdateCheckResult result = CheckForUpdate(currentVersion);
		QMetaObject::invokeMethod(
			qApp,
			[result, self]() {
				// Silent on any failure/inconclusive result AND when
				// already up to date -- see UpdateCheckResult::checked's
				// comment: this is a nice-to-have notice, never an error
				// surfaced to the user, and staying quiet when there's
				// nothing to say is the correct "no news" case, not a bug.
				if (!self || !result.checked || !result.updateAvailable)
					return;
				self->ShowUpdateNotice(result);
			},
			Qt::QueuedConnection);
	}).detach();
}

void TrigglowDelayDock::ShowUpdateNotice(const UpdateCheckResult &result)
{
	updateNoticeButton_->setText(T("Dock.Update.Notice").arg(QString::fromUtf8(result.latestVersion.c_str())));
	updateReleaseUrl_ = QString::fromUtf8(result.releaseUrl.c_str());
	updateNoticeButton_->setVisible(true);
}

} // namespace trigglow
