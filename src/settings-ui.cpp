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
#include "scene-combo-box.hpp"

#include <QCheckBox>
#include <QComboBox>
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
constexpr const char *kNoneOption = "(Ninguna)";
} // namespace

TrigglowDelayDock::TrigglowDelayDock(DelayController &controller, BufferModeController &bufferController,
				     QWidget *parent)
	: QWidget(parent),
	  controller_(controller),
	  bufferController_(bufferController)
{
	BuildUi();

	controller_.SetStatusChangedCallback([this](const DelayStatus &status) { OnStatusChanged(status); });
	bufferController_.SetStatusChangedCallback(
		[this](const BufferModeStatus &status) { OnBufferStatusChanged(status); });
	RefreshFromStatus(controller_.GetStatus());
	RefreshFromBufferStatus(bufferController_.GetStatus());
}

void TrigglowDelayDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(8);

	// --- Mode selector (issue #173 phase 2: one panel, two mechanisms) ---
	auto *modeRow = new QHBoxLayout();
	auto *modeLabel = new QLabel(QStringLiteral("Modo:"), this);
	modeRow->addWidget(modeLabel);
	modeCombo_ = new QComboBox(this);
	modeCombo_->addItem(QStringLiteral("Reconexion (simple)"));
	modeCombo_->addItem(QStringLiteral("Sin cortes (beta)"));
	modeCombo_->setToolTip(
		QStringLiteral("Reconexion: aplica el delay nativo de OBS, provoca un corte breve al cambiar en "
			       "directo. Sin cortes (beta): usa un buffer de video propio, nunca corta el stream, "
			       "pero consume mas memoria y todavia no retrasa el audio."));
	modeRow->addWidget(modeCombo_, /*stretch=*/1);
	root->addLayout(modeRow);

	// --- Reconnect mode group (existing, v0.1.0) ---
	reconnectGroup_ = new QWidget(this);
	auto *reconnectLayout = new QVBoxLayout(reconnectGroup_);
	reconnectLayout->setContentsMargins(0, 0, 0, 0);
	reconnectLayout->setSpacing(8);

	stateLabel_ = new QLabel(reconnectGroup_);
	stateLabel_->setStyleSheet("font-weight: 600; font-size: 13px;");
	reconnectLayout->addWidget(stateLabel_);

	detailLabel_ = new QLabel(reconnectGroup_);
	detailLabel_->setWordWrap(true);
	// palette(mid) is meant for borders/grooves, not body text — it renders as
	// low-contrast, hard-to-read gray on OBS's dark themes, which matters most
	// here since this label carries the Error state's explanation.
	// palette(windowText) is the theme's actual "readable text" role.
	detailLabel_->setStyleSheet("color: palette(windowText); font-size: 11px;");
	reconnectLayout->addWidget(detailLabel_);

	auto *configRow = new QHBoxLayout();
	auto *secondsLabel = new QLabel(QStringLiteral("Delay (segundos):"), reconnectGroup_);
	configRow->addWidget(secondsLabel);

	secondsSpin_ = new QSpinBox(reconnectGroup_);
	secondsSpin_->setRange(0, 1800);
	secondsSpin_->setValue(static_cast<int>(controller_.GetStatus().configuredSeconds));
	secondsSpin_->setToolTip(QStringLiteral("Segundos de delay a aplicar. Por defecto: 10s."));
	configRow->addWidget(secondsSpin_);
	reconnectLayout->addLayout(configRow);

	safeModeCheck_ = new QCheckBox(QStringLiteral("Modo seguro (recomendado)"), reconnectGroup_);
	safeModeCheck_->setChecked(controller_.GetStatus().safeMode);
	safeModeCheck_->setToolTip(
		QStringLiteral("Si la reconexion no se confirma a tiempo, el plugin no reintenta en bucle: "
			       "pasa a estado de Error y espera una accion manual tuya."));
	reconnectLayout->addWidget(safeModeCheck_);

	auto *sceneRow = new QHBoxLayout();
	auto *sceneLabel = new QLabel(QStringLiteral("Escena durante reconexion:"), reconnectGroup_);
	sceneRow->addWidget(sceneLabel);

	reconnectSceneCombo_ = new SceneComboBox(reconnectGroup_);
	reconnectSceneCombo_->setToolTip(
		QStringLiteral("Opcional: escena a mostrar mientras el stream reconecta, en vez del corte/pantalla "
			       "negra por defecto de OBS. Vuelve a la escena anterior en cuanto se confirma la "
			       "reconexion."));
	reconnectSceneCombo_->SetRefreshCallback(
		[this] { RefreshSceneCombo(reconnectSceneCombo_, controller_.GetStatus().reconnectSceneName, true); });
	sceneRow->addWidget(reconnectSceneCombo_, /*stretch=*/1);
	reconnectLayout->addLayout(sceneRow);
	RefreshSceneCombo(reconnectSceneCombo_, controller_.GetStatus().reconnectSceneName, true);

	auto *buttonRow = new QHBoxLayout();
	enableButton_ = new QPushButton(QStringLiteral("Enable Delay"), reconnectGroup_);
	disableButton_ = new QPushButton(QStringLiteral("Disable Delay"), reconnectGroup_);
	toggleButton_ = new QPushButton(QStringLiteral("Toggle Delay"), reconnectGroup_);
	buttonRow->addWidget(enableButton_);
	buttonRow->addWidget(disableButton_);
	buttonRow->addWidget(toggleButton_);
	reconnectLayout->addLayout(buttonRow);

	auto *hint = new QLabel(QStringLiteral("Si ya estas en directo, aplicar un cambio provoca una breve reconexion "
					       "del stream (ver docs/SPEC.md)."),
				reconnectGroup_);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: palette(windowText); font-size: 10px;");
	reconnectLayout->addWidget(hint);

	root->addWidget(reconnectGroup_);

	connect(secondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		[this](int value) { controller_.SetDelaySeconds(static_cast<uint32_t>(value)); });
	connect(safeModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { controller_.SetSafeMode(checked); });
	connect(reconnectSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		controller_.SetReconnectScene(index <= 0 ? std::string{}
							 : reconnectSceneCombo_->currentText().toStdString());
	});
	connect(enableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Enable pressed in dock (reconnect mode)");
		controller_.Enable();
	});
	connect(disableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Disable pressed in dock (reconnect mode)");
		controller_.Disable();
	});
	connect(toggleButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Toggle pressed in dock (reconnect mode)");
		controller_.Toggle();
	});

	applyWatchdog_ = new QTimer(this);
	applyWatchdog_->setSingleShot(true);
	applyWatchdog_->setInterval(kApplyTimeoutMs);
	connect(applyWatchdog_, &QTimer::timeout, this, [this] { controller_.OnApplyTimeout(); });

	// --- Buffer mode group (new, phase 2) ---
	bufferGroup_ = new QWidget(this);
	auto *bufferLayout = new QVBoxLayout(bufferGroup_);
	bufferLayout->setContentsMargins(0, 0, 0, 0);
	bufferLayout->setSpacing(8);

	bufferStateLabel_ = new QLabel(bufferGroup_);
	bufferStateLabel_->setStyleSheet("font-weight: 600; font-size: 13px;");
	bufferLayout->addWidget(bufferStateLabel_);

	bufferDetailLabel_ = new QLabel(bufferGroup_);
	bufferDetailLabel_->setWordWrap(true);
	bufferDetailLabel_->setStyleSheet("color: palette(windowText); font-size: 11px;");
	bufferLayout->addWidget(bufferDetailLabel_);

	auto *liveRow = new QHBoxLayout();
	liveRow->addWidget(new QLabel(QStringLiteral("Escena en directo:"), bufferGroup_));
	liveSceneCombo_ = new SceneComboBox(bufferGroup_);
	liveSceneCombo_->setToolTip(QStringLiteral("La escena con tu contenido real. Obligatoria para activar."));
	liveSceneCombo_->SetRefreshCallback(
		[this] { RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false); });
	liveRow->addWidget(liveSceneCombo_, /*stretch=*/1);
	bufferLayout->addLayout(liveRow);
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);

	auto *loadingRow = new QHBoxLayout();
	loadingRow->addWidget(new QLabel(QStringLiteral("Escena de carga:"), bufferGroup_));
	loadingSceneCombo_ = new SceneComboBox(bufferGroup_);
	loadingSceneCombo_->setToolTip(
		QStringLiteral("Opcional: que ver mientras se llena el buffer, en vez de quedarse en la escena en "
			       "directo sin delay durante ese hueco."));
	loadingSceneCombo_->SetRefreshCallback([this] {
		RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	});
	loadingRow->addWidget(loadingSceneCombo_, /*stretch=*/1);
	bufferLayout->addLayout(loadingRow);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);

	auto *bufferSecondsRow = new QHBoxLayout();
	bufferSecondsRow->addWidget(new QLabel(QStringLiteral("Delay (segundos):"), bufferGroup_));
	bufferSecondsSpin_ = new QSpinBox(bufferGroup_);
	bufferSecondsSpin_->setRange(1, 60);
	bufferSecondsSpin_->setValue(static_cast<int>(bufferController_.GetStatus().delaySeconds));
	bufferSecondsSpin_->setToolTip(
		QStringLiteral("El maximo real depende de la resolucion/FPS de la escena y de un presupuesto de "
			       "VRAM fijo (~1.5GB) - pedir mas de lo que cabe se recorta automaticamente (log de "
			       "OBS)."));
	bufferSecondsRow->addWidget(bufferSecondsSpin_);
	bufferLayout->addLayout(bufferSecondsRow);

	auto *bufferButtonRow = new QHBoxLayout();
	bufferEnableButton_ = new QPushButton(QStringLiteral("Enable"), bufferGroup_);
	bufferDisableButton_ = new QPushButton(QStringLiteral("Disable"), bufferGroup_);
	bufferButtonRow->addWidget(bufferEnableButton_);
	bufferButtonRow->addWidget(bufferDisableButton_);
	bufferLayout->addLayout(bufferButtonRow);

	auto *bufferHint = new QLabel(
		QStringLiteral("Beta: solo retrasa video, todavia no el audio. Nunca corta el stream una vez lleno "
			       "el buffer."),
		bufferGroup_);
	bufferHint->setWordWrap(true);
	bufferHint->setStyleSheet("color: palette(windowText); font-size: 10px;");
	bufferLayout->addWidget(bufferHint);

	root->addWidget(bufferGroup_);
	root->addStretch(1);

	connect(liveSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		bufferController_.SetLiveScene(index < 0 ? std::string{}
							 : liveSceneCombo_->currentText().toStdString());
	});
	connect(loadingSceneCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		bufferController_.SetLoadingScene(index <= 0 ? std::string{}
							     : loadingSceneCombo_->currentText().toStdString());
	});
	connect(bufferSecondsSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
		[this](int value) { bufferController_.SetDelaySeconds(static_cast<uint32_t>(value)); });
	connect(bufferEnableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Enable pressed in dock (buffer mode)");
		bufferController_.Enable();
	});
	connect(bufferDisableButton_, &QPushButton::clicked, this, [this] {
		TRIGGLOW_LOG_INFO(kComponent, "Disable pressed in dock (buffer mode)");
		bufferController_.Disable();
	});

	fillTimer_ = new QTimer(this);
	fillTimer_->setSingleShot(true);
	connect(fillTimer_, &QTimer::timeout, this, [this] { bufferController_.OnFillTimerElapsed(); });

	connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		reconnectGroup_->setVisible(index == 0);
		bufferGroup_->setVisible(index == 1);
	});
	// QComboBox auto-selects index 0 as soon as the first item is added
	// (above), before this connect() — so currentIndexChanged never fires
	// for the already-0 initial state and the lambda above never runs on
	// its own. Set the initial visibility explicitly instead of relying on
	// setCurrentIndex(0) to (not) trigger it.
	reconnectGroup_->setVisible(true);
	bufferGroup_->setVisible(false);
}

// ============================== Reconnect mode ==============================

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
		stateText = QStringLiteral("● Active (%1s)")
				    .arg(status.activeSeconds ? status.activeSeconds : status.configuredSeconds);
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
	// Don't let the user pick a different placeholder scene mid-reconnect —
	// the controller already captured "the scene to restore to" for this
	// specific reconnect when it started.
	reconnectSceneCombo_->setEnabled(!busy);

	const QSignalBlocker blockScene(reconnectSceneCombo_);
	int sceneIndex = status.reconnectSceneName.empty()
				 ? 0
				 : reconnectSceneCombo_->findText(QString::fromStdString(status.reconnectSceneName));
	reconnectSceneCombo_->setCurrentIndex(sceneIndex >= 0 ? sceneIndex : 0);
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

// ================================ Buffer mode ================================

void TrigglowDelayDock::OnBufferStatusChanged(const BufferModeStatus &status)
{
	RefreshFromBufferStatus(status);

	if (status.state == BufferModeState::Filling)
		ArmFillTimer(status.delaySeconds);
	else
		DisarmFillTimer();
}

void TrigglowDelayDock::RefreshFromBufferStatus(const BufferModeStatus &status)
{
	QString stateText;
	QString color;
	switch (status.state) {
	case BufferModeState::Inactive:
		stateText = QStringLiteral("● Inactive");
		color = QStringLiteral("palette(text)");
		break;
	case BufferModeState::Filling:
		stateText = QStringLiteral("● Llenando buffer...");
		color = QStringLiteral("#d8a400");
		break;
	case BufferModeState::Active:
		stateText = QStringLiteral("● Active (%1s, sin cortes)").arg(status.delaySeconds);
		color = QStringLiteral("#2e9e44");
		break;
	case BufferModeState::Error:
		stateText = QStringLiteral("● Error");
		color = QStringLiteral("#c0392b");
		break;
	}
	bufferStateLabel_->setText(stateText);
	bufferStateLabel_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(color));

	bufferDetailLabel_->setText(QString::fromStdString(status.message));
	bufferDetailLabel_->setVisible(!status.message.empty());

	bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	liveSceneCombo_->setEnabled(!busy);
	loadingSceneCombo_->setEnabled(!busy);
	bufferSecondsSpin_->setEnabled(status.state != BufferModeState::Filling);
	bufferEnableButton_->setEnabled(!busy);
	bufferDisableButton_->setEnabled(busy);

	const QSignalBlocker blockSeconds(bufferSecondsSpin_);
	bufferSecondsSpin_->setValue(static_cast<int>(status.delaySeconds));

	const QSignalBlocker blockLive(liveSceneCombo_);
	int liveIndex = liveSceneCombo_->findText(QString::fromStdString(status.liveSceneName));
	if (liveIndex >= 0)
		liveSceneCombo_->setCurrentIndex(liveIndex);

	const QSignalBlocker blockLoading(loadingSceneCombo_);
	int loadingIndex = status.loadingSceneName.empty()
				   ? 0
				   : loadingSceneCombo_->findText(QString::fromStdString(status.loadingSceneName));
	loadingSceneCombo_->setCurrentIndex(loadingIndex >= 0 ? loadingIndex : 0);
}

void TrigglowDelayDock::ArmFillTimer(uint32_t seconds)
{
	if (!fillTimer_)
		return;
	fillTimer_->setInterval(static_cast<int>(seconds) * 1000);
	fillTimer_->start();
}

void TrigglowDelayDock::DisarmFillTimer()
{
	if (fillTimer_)
		fillTimer_->stop();
}

// ================================== Shared ==================================

void TrigglowDelayDock::RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption)
{
	const QSignalBlocker block(combo);
	QString previousValue = QString::fromStdString(currentValue);
	combo->clear();
	if (includeNoneOption)
		combo->addItem(QString::fromUtf8(kNoneOption));
	for (const auto &name : controller_.ListAvailableScenes())
		combo->addItem(QString::fromStdString(name));

	if (!previousValue.isEmpty()) {
		int idx = combo->findText(previousValue);
		if (idx >= 0)
			combo->setCurrentIndex(idx);
	}
}

} // namespace trigglow
