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
#include "logging.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

namespace trigglow {

namespace {
constexpr const char *kComponent = "settings-ui";
// How long we wait after requesting a reconnect before the "modo seguro"
// watchdog gives up. See docs/SPEC.md §3 and DelayController::OnApplyTimeout().
constexpr int kApplyTimeoutMs = 12000;
} // namespace

TrigglowDelayDock::TrigglowDelayDock(DelayController &controller, QWidget *parent)
	: QWidget(parent),
	  controller_(controller)
{
	BuildUi();

	controller_.SetStatusChangedCallback([this](const DelayStatus &status) { OnStatusChanged(status); });
	RefreshFromStatus(controller_.GetStatus());
}

void TrigglowDelayDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(8);

	// --- Status row ---
	stateLabel_ = new QLabel(this);
	stateLabel_->setStyleSheet("font-weight: 600; font-size: 13px;");
	root->addWidget(stateLabel_);

	detailLabel_ = new QLabel(this);
	detailLabel_->setWordWrap(true);
	detailLabel_->setStyleSheet("color: palette(mid); font-size: 11px;");
	root->addWidget(detailLabel_);

	// --- Config row: seconds + safe mode ---
	auto *configRow = new QHBoxLayout();
	auto *secondsLabel = new QLabel(QStringLiteral("Delay (segundos):"), this);
	configRow->addWidget(secondsLabel);

	secondsSpin_ = new QSpinBox(this);
	secondsSpin_->setRange(0, 1800);
	secondsSpin_->setValue(static_cast<int>(controller_.GetStatus().configuredSeconds));
	secondsSpin_->setToolTip(QStringLiteral("Segundos de delay a aplicar. Por defecto: 10s."));
	configRow->addWidget(secondsSpin_);
	root->addLayout(configRow);

	safeModeCheck_ = new QCheckBox(QStringLiteral("Modo seguro (recomendado)"), this);
	safeModeCheck_->setChecked(controller_.GetStatus().safeMode);
	safeModeCheck_->setToolTip(
		QStringLiteral("Si la reconexion no se confirma a tiempo, el plugin no reintenta en bucle: "
				"pasa a estado de Error y espera una accion manual tuya."));
	root->addWidget(safeModeCheck_);

	// --- Action buttons ---
	auto *buttonRow = new QHBoxLayout();
	enableButton_ = new QPushButton(QStringLiteral("Enable Delay"), this);
	disableButton_ = new QPushButton(QStringLiteral("Disable Delay"), this);
	toggleButton_ = new QPushButton(QStringLiteral("Toggle Delay"), this);
	buttonRow->addWidget(enableButton_);
	buttonRow->addWidget(disableButton_);
	buttonRow->addWidget(toggleButton_);
	root->addLayout(buttonRow);

	auto *hint = new QLabel(
		QStringLiteral("Si ya estas en directo, aplicar un cambio provoca una breve reconexion "
				"del stream (ver docs/SPEC.md)."),
		this);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: palette(mid); font-size: 10px;");
	root->addWidget(hint);

	root->addStretch(1);

	connect(secondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		[this](int value) { controller_.SetDelaySeconds(static_cast<uint32_t>(value)); });
	connect(safeModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { controller_.SetSafeMode(checked); });
	connect(enableButton_, &QPushButton::clicked, this, [this] { controller_.Enable(); });
	connect(disableButton_, &QPushButton::clicked, this, [this] { controller_.Disable(); });
	connect(toggleButton_, &QPushButton::clicked, this, [this] { controller_.Toggle(); });

	applyWatchdog_ = new QTimer(this);
	applyWatchdog_->setSingleShot(true);
	applyWatchdog_->setInterval(kApplyTimeoutMs);
	connect(applyWatchdog_, &QTimer::timeout, this, [this] { controller_.OnApplyTimeout(); });
}

void TrigglowDelayDock::OnStatusChanged(const DelayStatus &status)
{
	RefreshFromStatus(status);

	if (status.state == DelayState::Applying)
		ArmApplyWatchdog();
	else
		DisarmApplyWatchdog();
}

void TrigglowDelayDock::RefreshFromStatus(const DelayStatus &status)
{
	QString stateText;
	QString color;
	switch (status.state) {
	case DelayState::Inactive:
		stateText = QStringLiteral("● Inactive");
		color = QStringLiteral("palette(text)");
		break;
	case DelayState::Applying:
		stateText = QStringLiteral("● Applying...");
		color = QStringLiteral("#d8a400");
		break;
	case DelayState::Active:
		stateText = QStringLiteral("● Active (%1s)").arg(status.activeSeconds ? status.activeSeconds
											      : status.configuredSeconds);
		color = QStringLiteral("#2e9e44");
		break;
	case DelayState::Error:
		stateText = QStringLiteral("● Error");
		color = QStringLiteral("#c0392b");
		break;
	}
	stateLabel_->setText(stateText);
	stateLabel_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(color));

	detailLabel_->setText(QString::fromStdString(status.message));
	detailLabel_->setVisible(!status.message.empty());

	// Keep the spin box/checkbox in sync without re-triggering their
	// valueChanged/toggled signals (which would call back into the
	// controller and could cause an unwanted re-apply loop).
	const QSignalBlocker blockSpin(secondsSpin_);
	const QSignalBlocker blockCheck(safeModeCheck_);
	secondsSpin_->setValue(static_cast<int>(status.configuredSeconds));
	safeModeCheck_->setChecked(status.safeMode);

	bool busy = status.state == DelayState::Applying;
	enableButton_->setEnabled(!busy);
	disableButton_->setEnabled(!busy);
	toggleButton_->setEnabled(!busy);
}

void TrigglowDelayDock::ArmApplyWatchdog()
{
	if (applyWatchdog_)
		applyWatchdog_->start();
}

void TrigglowDelayDock::DisarmApplyWatchdog()
{
	if (applyWatchdog_)
		applyWatchdog_->stop();
}

} // namespace trigglow
