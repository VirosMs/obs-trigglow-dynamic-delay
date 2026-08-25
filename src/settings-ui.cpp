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
constexpr const char *kNoneOption = "(Ninguna)";
} // namespace

TrigglowDelayDock::TrigglowDelayDock(BufferModeController &bufferController, QWidget *parent)
	: QWidget(parent),
	  bufferController_(bufferController)
{
	BuildUi();

	bufferController_.SetStatusChangedCallback([this](const BufferModeStatus &status) { OnStatusChanged(status); });
	RefreshFromStatus(bufferController_.GetStatus());
}

void TrigglowDelayDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(10, 10, 10, 10);
	root->setSpacing(8);

	stateLabel_ = new QLabel(this);
	stateLabel_->setStyleSheet("font-weight: 600; font-size: 13px;");
	root->addWidget(stateLabel_);

	detailLabel_ = new QLabel(this);
	detailLabel_->setWordWrap(true);
	detailLabel_->setStyleSheet("color: palette(windowText); font-size: 11px;");
	root->addWidget(detailLabel_);

	auto *liveRow = new QHBoxLayout();
	liveRow->addWidget(new QLabel(QStringLiteral("Escena en directo:"), this));
	liveSceneCombo_ = new SceneComboBox(this);
	liveSceneCombo_->setToolTip(QStringLiteral("La escena con tu contenido real. Obligatoria para activar."));
	liveSceneCombo_->SetRefreshCallback(
		[this] { RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false); });
	liveRow->addWidget(liveSceneCombo_, /*stretch=*/1);
	root->addLayout(liveRow);
	RefreshSceneCombo(liveSceneCombo_, bufferController_.GetStatus().liveSceneName, false);

	auto *loadingRow = new QHBoxLayout();
	loadingRow->addWidget(new QLabel(QStringLiteral("Escena de carga:"), this));
	loadingSceneCombo_ = new SceneComboBox(this);
	loadingSceneCombo_->setToolTip(
		QStringLiteral("Opcional: que ver mientras se llena el buffer, en vez de quedarse en la escena en "
			       "directo sin delay durante ese hueco."));
	loadingSceneCombo_->SetRefreshCallback([this] {
		RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);
	});
	loadingRow->addWidget(loadingSceneCombo_, /*stretch=*/1);
	root->addLayout(loadingRow);
	RefreshSceneCombo(loadingSceneCombo_, bufferController_.GetStatus().loadingSceneName, true);

	auto *secondsRow = new QHBoxLayout();
	secondsRow->addWidget(new QLabel(QStringLiteral("Delay (segundos):"), this));
	secondsSpin_ = new QSpinBox(this);
	secondsSpin_->setRange(1, 60);
	secondsSpin_->setValue(static_cast<int>(bufferController_.GetStatus().delaySeconds));
	secondsSpin_->setToolTip(
		QStringLiteral("Segundos de retraso pedidos. Se respeta la calidad minima elegida abajo por "
			       "encima del tiempo: si no caben enteros en el presupuesto de VRAM a esa calidad, "
			       "el tiempo real de buffer se acorta en su lugar."));
	secondsRow->addWidget(secondsSpin_);
	root->addLayout(secondsRow);

	auto *qualityRow = new QHBoxLayout();
	qualityRow->addWidget(new QLabel(QStringLiteral("Calidad minima:"), this));
	minResolutionCombo_ = new QComboBox(this);
	minResolutionCombo_->addItem(QStringLiteral("480p"), 480);
	minResolutionCombo_->addItem(QStringLiteral("720p"), 720);
	minResolutionCombo_->addItem(QStringLiteral("1080p"), 1080);
	minResolutionCombo_->setToolTip(
		QStringLiteral("El tramo delayed nunca baja de esta resolucion, aunque eso signifique guardar "
			       "menos segundos de los pedidos. Mas calidad = menos tiempo real de buffer con el "
			       "mismo presupuesto de VRAM."));
	qualityRow->addWidget(minResolutionCombo_);
	root->addLayout(qualityRow);

	// Live-updated by RefreshFitEstimate() whenever secondsSpin_/
	// minResolutionCombo_ change -- see that method and
	// BufferModeController::EstimateBufferFit's comment.
	fitLabel_ = new QLabel(this);
	fitLabel_->setWordWrap(true);
	fitLabel_->setStyleSheet("font-size: 10px;");
	root->addWidget(fitLabel_);

	// Informational only, computed once from real hardware where possible
	// (VideoDelayFilter::GetBufferBudgetBytes, see src/gpu-info.hpp) --
	// "aconsejar segun el hardware, pero a su eleccion": this never
	// restricts delaySeconds/minResolutionHeight above, just shows the
	// user what their choices are actually working with.
	uint64_t budgetMb = bufferController_.GetBufferBudgetBytes() / (1024 * 1024);
	auto *budgetLabel = new QLabel(
		QStringLiteral("Presupuesto de buffer detectado: ~%1 MB (segun tu GPU).").arg(budgetMb), this);
	budgetLabel->setWordWrap(true);
	budgetLabel->setStyleSheet("color: palette(windowText); font-size: 10px;");
	root->addWidget(budgetLabel);

	auto *buttonRow = new QHBoxLayout();
	enableButton_ = new QPushButton(QStringLiteral("Enable"), this);
	disableButton_ = new QPushButton(QStringLiteral("Disable"), this);
	buttonRow->addWidget(enableButton_);
	buttonRow->addWidget(disableButton_);
	root->addLayout(buttonRow);

	auto *hint = new QLabel(
		QStringLiteral("Beta: retrasa video y audio juntos. Nunca corta el stream una vez lleno el buffer."),
		this);
	hint->setWordWrap(true);
	hint->setStyleSheet("color: palette(windowText); font-size: 10px;");
	root->addWidget(hint);

	root->addStretch(1);

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

	fillTimer_ = new QTimer(this);
	fillTimer_->setSingleShot(true);
	connect(fillTimer_, &QTimer::timeout, this, [this] { bufferController_.OnFillTimerElapsed(); });

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
		fitLabel_->setText(QStringLiteral("Elige una escena en directo para ver una estimacion."));
		fitLabel_->setStyleSheet("color: palette(windowText); font-size: 10px;");
		return;
	}

	if (estimate.fitsFullDuration) {
		fitLabel_->setText(QStringLiteral("✓ Con estos ajustes caben los %1s pedidos enteros, a %2x%3.")
					   .arg(secondsSpin_->value())
					   .arg(estimate.width)
					   .arg(estimate.height));
		fitLabel_->setStyleSheet("color: #2e9e44; font-size: 10px;");
	} else {
		fitLabel_->setText(
			QStringLiteral("⚠ Con estos ajustes solo se guardaran ~%1s reales de los %2s pedidos "
				       "(a %3x%4) -- no es un error, pero el delay real sera mas corto de lo "
				       "pedido. Baja los segundos o la calidad minima si quieres los %2s enteros.")
				.arg(estimate.actualSeconds, 0, 'f', 1)
				.arg(secondsSpin_->value())
				.arg(estimate.width)
				.arg(estimate.height));
		fitLabel_->setStyleSheet("color: #d8a400; font-size: 10px;");
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
	stateLabel_->setText(stateText);
	stateLabel_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(color));

	detailLabel_->setText(QString::fromStdString(status.message));
	detailLabel_->setVisible(!status.message.empty());

	bool busy = status.state == BufferModeState::Filling || status.state == BufferModeState::Active;
	liveSceneCombo_->setEnabled(!busy);
	loadingSceneCombo_->setEnabled(!busy);
	secondsSpin_->setEnabled(status.state != BufferModeState::Filling);
	minResolutionCombo_->setEnabled(!busy);
	enableButton_->setEnabled(!busy);
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

void TrigglowDelayDock::RefreshSceneCombo(SceneComboBox *combo, const std::string &currentValue, bool includeNoneOption)
{
	const QSignalBlocker block(combo);
	QString previousValue = QString::fromStdString(currentValue);
	combo->clear();
	if (includeNoneOption)
		combo->addItem(QString::fromUtf8(kNoneOption));
	for (const auto &name : bufferController_.ListAvailableScenes())
		combo->addItem(QString::fromStdString(name));

	if (!previousValue.isEmpty()) {
		int idx = combo->findText(previousValue);
		if (idx >= 0)
			combo->setCurrentIndex(idx);
	}
}

} // namespace trigglow
