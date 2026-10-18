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

#include "trigi-page.hpp"
#include "api-client.hpp"
#include "chat-text.hpp"
#include "trigi-widgets.hpp"

#include <algorithm>

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace trigglow {

using supportui::T;
using trigiui::Problem;

namespace {

constexpr int kAvatarSize = 28;
constexpr int kAvatarGap = 8;
constexpr int kMarkerSize = 5;
// Button icons: the glyph, and its gap to the text.
constexpr int kIconSize = 14;
constexpr int kButtonIconGap = 6;
constexpr int kLinkIconGap = 5;
// Below this width the technical-data row stacks.
constexpr int kNarrowWidth = 420;
// "Trigi is thinking" becomes "taking longer than usual" after this.
constexpr int kSlowAfterMs = 15000;
// The user's messages take at most this share of the transcript's width.
constexpr double kUserBubbleShare = 0.82;

QString Q(const std::string &text)
{
	return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

void TrackEmpty(QWidget *field, bool empty)
{
	ui::SetStyleProperty(field, "tgEmpty", empty ? QStringLiteral("true") : QStringLiteral("false"));
}

// A small accent square, the bullet of the notice and of Trigi's lists, set
// down to the middle of the first text line.
QWidget *MakeMarker(QWidget *parent, int lineHeight)
{
	auto *box = new QWidget(parent);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(0, std::max(0, (lineHeight - kMarkerSize) / 2), 0, 0);
	auto *marker = new QFrame(box);
	marker->setProperty("tgMarker", QStringLiteral("true"));
	marker->setFixedSize(kMarkerSize, kMarkerSize);
	layout->addWidget(marker);
	layout->addStretch(1);
	box->setFixedWidth(kMarkerSize + 2);
	return box;
}

// Hidden by the page (a removed row), as opposed to just created and not
// shown yet.
bool HiddenOnPurpose(QWidget *widget, QWidget *stop)
{
	for (; widget && widget != stop; widget = widget->parentWidget()) {
		if (widget->testAttribute(Qt::WA_WState_ExplicitShowHide) &&
		    widget->testAttribute(Qt::WA_WState_Hidden))
			return true;
	}
	return false;
}

void RemoveRow(QVBoxLayout *layout, QWidget *row)
{
	if (!row)
		return;
	row->hide();
	if (layout)
		layout->removeWidget(row);
	row->deleteLater();
}

} // namespace

TrigiPage::TrigiPage(SupportService &service, QWidget *parent) : QWidget(parent), service_(service)
{
	slowTimer_ = new QTimer(this);
	slowTimer_->setSingleShot(true);
	slowTimer_->setInterval(kSlowAfterMs);
	connect(slowTimer_, &QTimer::timeout, this, [this] {
		if (!thinkingText_ || !thinkingRow_)
			return;
		thinkingText_->setText(T("Trigi.ThinkingLong"));
		thinkingRow_->setAccessibleName(T("Trigi.ThinkingLong"));
		Announce(T("Trigi.ThinkingLong"), false);
	});
	rateTimer_ = new QTimer(this);
	rateTimer_->setInterval(1000);
	connect(rateTimer_, &QTimer::timeout, this, [this] { UpdateRateLimit(); });

	BuildUi();
	ShowNotice(!service_.NoticeAccepted());
}

TrigiPage::~TrigiPage() = default;

// ---- Layout ---------------------------------------------------------------------------

void TrigiPage::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(18, 14, 18, 12);
	root->setSpacing(8);

	views_ = new QStackedWidget(this);

	noticeScroll_ = new QScrollArea(this);
	noticeScroll_->setProperty("tgPlain", QStringLiteral("true"));
	noticeScroll_->setFrameShape(QFrame::NoFrame);
	noticeScroll_->setWidgetResizable(true);
	noticeScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	noticeScroll_->setFocusPolicy(Qt::NoFocus);
	noticeScroll_->setWidget(BuildNotice());
	views_->addWidget(noticeScroll_);

	// The transcript: a tab stop of its own so it scrolls from the keyboard.
	transcript_ = new QScrollArea(this);
	transcript_->setProperty("tgTranscript", QStringLiteral("true"));
	transcript_->setWidgetResizable(true);
	transcript_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	transcript_->setFocusPolicy(Qt::StrongFocus);
	transcript_->setAccessibleName(T("Trigi.TranscriptName"));
	transcriptContent_ = new QWidget(transcript_);
	transcriptContent_->setObjectName(QStringLiteral("tgTranscriptContent"));
	messages_ = new QVBoxLayout(transcriptContent_);
	messages_->setContentsMargins(12, 12, 12, 12);
	messages_->setSpacing(12);
	messages_->addStretch(1); // Messages stack from the top.
	transcript_->setWidget(transcriptContent_);
	transcript_->viewport()->installEventFilter(this);
	QScrollBar *bar = transcript_->verticalScrollBar();
	connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int maximum) {
		if (stickToEnd_)
			bar->setValue(maximum);
	});
	// The user scrolled: stop following new messages until the next one.
	connect(bar, &QAbstractSlider::actionTriggered, this, [this](int) { stickToEnd_ = false; });
	views_->addWidget(transcript_);
	root->addWidget(views_, 1);

	// State banner (contract A3.9): icon, text, countdown, up to two actions.
	banner_ = new QFrame(this);
	banner_->setProperty("tgBanner", QStringLiteral("warning"));
	{
		auto *layout = new QVBoxLayout(banner_);
		layout->setContentsMargins(10, 8, 10, 8);
		layout->setSpacing(8);
		auto *textRow = new QHBoxLayout();
		textRow->setSpacing(8);
		bannerIcon_ = new QLabel(banner_);
		bannerIcon_->setFixedSize(16, 16);
		bannerText_ = new QLabel(banner_);
		bannerText_->setWordWrap(true);
		bannerText_->setTextFormat(Qt::PlainText);
		bannerCountdown_ = new QLabel(banner_);
		bannerCountdown_->setProperty("tgRole", QStringLiteral("data"));
		bannerCountdown_->setProperty("tgTone", QStringLiteral("warning"));
		// A fixed name: the ticking text must not be read out every second.
		bannerCountdown_->setAccessibleName(T("Trigi.CountdownName"));
		bannerCountdown_->setVisible(false);
		textRow->addWidget(bannerIcon_, 0, Qt::AlignTop);
		textRow->addWidget(bannerText_, 1);
		textRow->addWidget(bannerCountdown_, 0, Qt::AlignTop);
		layout->addLayout(textRow);
		auto *buttons = new QBoxLayout(QBoxLayout::LeftToRight);
		bannerButtons_ = buttons;
		buttons->setContentsMargins(24, 0, 0, 0);
		buttons->setSpacing(8);
		bannerPrimary_ = MakeButton(QString(), "secondary", banner_);
		bannerSecondary_ = MakeButton(QString(), "secondary", banner_);
		buttons->addWidget(bannerPrimary_, 0, Qt::AlignLeft);
		buttons->addWidget(bannerSecondary_, 0, Qt::AlignLeft);
		buttons->addStretch(1);
		layout->addLayout(buttons);
		connect(bannerPrimary_, &QPushButton::clicked, this, [this] {
			if (bannerPrimaryAction_)
				bannerPrimaryAction_();
		});
		connect(bannerSecondary_, &QPushButton::clicked, this, [this] {
			if (bannerSecondaryAction_)
				bannerSecondaryAction_();
		});
	}
	banner_->setVisible(false);
	root->addWidget(banner_);

	composer_ = BuildComposer();
	root->addWidget(composer_);

	// "Include OBS technical data" + "See what is sent" (contract A3.4): off
	// until the user ticks it, remembered, sent once with a new conversation.
	diagnosticsRow_ = new QWidget(this);
	{
		auto *layout = new QVBoxLayout(diagnosticsRow_);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(3);
		auto *row = new QBoxLayout(QBoxLayout::LeftToRight);
		diagnosticsLayout_ = row;
		row->setSpacing(12);
		diagnosticsCheck_ = new QCheckBox(T("Trigi.Diagnostics.Include"), diagnosticsRow_);
		diagnosticsCheck_->setChecked(service_.IncludeDiagnostics());
		diagnosticsCheck_->setAccessibleDescription(T("Trigi.Diagnostics.Hint"));
		diagnosticsCheck_->setFocusPolicy(Qt::TabFocus);
		diagnosticsSee_ = MakeButton(T("Trigi.Diagnostics.See"), "link", diagnosticsRow_);
		SetButtonIcon(diagnosticsSee_, ui::IconId::Eye, Tone::Accent);
		diagnosticsSee_->setAccessibleName(T("Trigi.Diagnostics.SeeName"));
		row->addWidget(diagnosticsCheck_, 0, Qt::AlignLeft);
		row->addWidget(diagnosticsSee_, 0, Qt::AlignLeft);
		row->addStretch(1);
		layout->addLayout(row);
		diagnosticsHint_ = new QLabel(T("Trigi.Diagnostics.Hint"), diagnosticsRow_);
		diagnosticsHint_->setProperty("tgRole", QStringLiteral("hint"));
		diagnosticsHint_->setWordWrap(true);
		diagnosticsHint_->setTextFormat(Qt::PlainText);
		diagnosticsHint_->setContentsMargins(22, 0, 0, 0);
		layout->addWidget(diagnosticsHint_);
		connect(diagnosticsCheck_, &QCheckBox::toggled, this,
			[this](bool on) { service_.SetIncludeDiagnostics(on); });
		connect(diagnosticsSee_, &QPushButton::clicked, this, [this] { ShowDiagnosticsPreview(); });
	}
	diagnosticsRow_->setVisible(service_.DiagnosticsSupported());
	root->addWidget(diagnosticsRow_);

	// The notice's two answers (contract A5.5).
	noticeActions_ = new QWidget(this);
	{
		auto *layout = new QHBoxLayout(noticeActions_);
		layout->setContentsMargins(0, 4, 0, 0);
		layout->setSpacing(8);
		acceptButton_ = MakeButton(T("Trigi.Notice.Accept"), "primary", noticeActions_);
		declineButton_ = MakeButton(T("Trigi.Notice.Decline"), "secondary", noticeActions_);
		declineButton_->setProperty("tgSize", QStringLiteral("large"));
		layout->addWidget(acceptButton_);
		layout->addWidget(declineButton_);
		layout->addStretch(1);
		connect(acceptButton_, &QPushButton::clicked, this, [this] {
			service_.AcceptNotice();
			ShowNotice(false);
			question_->setFocus(Qt::OtherFocusReason);
		});
		connect(declineButton_, &QPushButton::clicked, this, [this] {
			if (onOpenForm)
				onOpenForm(TicketDraft{}, Handover::UserChoice);
		});
	}
	root->addWidget(noticeActions_);

	AddTrigiText(T("Trigi.Greeting"));

	UpdateTabOrder();
}

QWidget *TrigiPage::BuildNotice()
{
	auto *content = new QWidget();
	content->setObjectName(QStringLiteral("tgScrollContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto *card = new QFrame(content);
	card->setProperty("tgCard", QStringLiteral("true"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(14, 12, 14, 14);
	cardLayout->setSpacing(9);

	auto *titleRow = new QHBoxLayout();
	titleRow->setSpacing(8);
	auto *title = new QLabel(T("Trigi.Notice.Title"), card);
	title->setProperty("tgRole", QStringLiteral("strong"));
	title->setWordWrap(true);
	titleRow->addWidget(MakeIconLabel(ui::IconId::Shield, Tone::Accent, 16, card), 0, Qt::AlignTop);
	titleRow->addWidget(title, 1);
	cardLayout->addLayout(titleRow);

	const int lineHeight = QFontMetrics(font()).lineSpacing();
	static const char *const kItems[] = {"Trigi.Notice.Item1", "Trigi.Notice.Item2", "Trigi.Notice.Item3",
					     "Trigi.Notice.Item4", "Trigi.Notice.Item5", "Trigi.Notice.Item6"};
	for (const char *key : kItems) {
		auto *row = new QHBoxLayout();
		row->setSpacing(9);
		auto *text = new QLabel(T(key), card);
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		row->addWidget(MakeMarker(card, lineHeight), 0, Qt::AlignTop);
		row->addWidget(text, 1);
		cardLayout->addLayout(row);
		if (key == kItems[1]) {
			// The policy the second point names: a fixed page, opened in the
			// browser (never a link inside the text).
			privacyButton_ = MakeButton(T("Trigi.Notice.Privacy"), "link", card);
			SetButtonIcon(privacyButton_, ui::IconId::ExternalLink, Tone::Accent);
			privacyButton_->setAccessibleName(T("Trigi.Notice.PrivacyName"));
			auto *linkRow = new QHBoxLayout();
			linkRow->setContentsMargins(kMarkerSize + 2 + 9, 0, 0, 0);
			linkRow->addWidget(privacyButton_);
			linkRow->addStretch(1);
			cardLayout->addLayout(linkRow);
			connect(privacyButton_, &QPushButton::clicked, this,
				[] { supportui::OpenWebPage(api::web::kPrivacy); });
		}
	}
	card->setAccessibleName(T("Trigi.Notice.Title"));
	layout->addWidget(card);
	layout->addStretch(1);
	return content;
}

QWidget *TrigiPage::BuildComposer()
{
	auto *box = new QWidget(this);
	auto *layout = new QVBoxLayout(box);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(5);

	auto *row = new QHBoxLayout();
	row->setSpacing(8);
	question_ = new QuestionEdit(box);
	question_->setPlaceholderText(T("Trigi.Placeholder"));
	question_->setAccessibleName(T("Trigi.QuestionLabel"));
	question_->setAccessibleDescription(T("Trigi.QuestionHelp"));
	question_->onSubmit = [this] {
		Submit();
	};
	sendButton_ = MakeButton(T("Trigi.Send"), "primary", box);
	SetButtonIcon(sendButton_, ui::IconId::Send, Tone::OnPrimary);
	sendButton_->setAccessibleDescription(T("Trigi.QuestionHelp"));
	row->addWidget(question_, 1);
	row->addWidget(sendButton_, 0, Qt::AlignBottom);
	layout->addLayout(row);

	// The fixed line under the question box (contract A5.5) | the counter.
	auto *meta = new QHBoxLayout();
	meta->setSpacing(8);
	finePrint_ = new QWidget(box);
	{
		auto *fine = new QHBoxLayout(finePrint_);
		fine->setContentsMargins(0, 0, 0, 0);
		fine->setSpacing(6);
		auto *text = new QLabel(T("Trigi.FinePrint"), finePrint_);
		text->setProperty("tgRole", QStringLiteral("hint"));
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		fine->addWidget(MakeIconLabel(ui::IconId::Info, Tone::Muted, 12, finePrint_), 0, Qt::AlignTop);
		fine->addWidget(text, 1);
	}
	counter_ = new QLabel(box);
	counter_->setProperty("tgRole", QStringLiteral("dataHint"));
	counter_->setAlignment(Qt::AlignRight | Qt::AlignTop);
	meta->addWidget(finePrint_, 1);
	meta->addWidget(counter_, 0, Qt::AlignTop);
	layout->addLayout(meta);

	connect(question_, &QPlainTextEdit::textChanged, this, [this] { UpdateCounter(); });
	connect(sendButton_, &QPushButton::clicked, this, [this] { Submit(); });
	UpdateCounter();
	return box;
}

void TrigiPage::ShowNotice(bool show)
{
	views_->setCurrentWidget(show ? static_cast<QWidget *>(noticeScroll_) : static_cast<QWidget *>(transcript_));
	composer_->setVisible(!show);
	noticeActions_->setVisible(show);
	diagnosticsHint_->setVisible(show);
	UpdateControls();
}

bool TrigiPage::NoticeShown() const
{
	return views_->currentWidget() == noticeScroll_;
}

void TrigiPage::FocusFirst()
{
	if (NoticeShown())
		acceptButton_->setFocus(Qt::OtherFocusReason);
	else
		question_->setFocus(Qt::OtherFocusReason);
}

bool TrigiPage::Busy() const
{
	return service_.Busy() || summarizing_ || !pending_.text.isEmpty();
}

bool TrigiPage::HasMessages() const
{
	return userMessages_;
}

// ---- Theme and building blocks --------------------------------------------------------

QColor TrigiPage::ToneColor(Tone tone) const
{
	const uitokens::Tokens &t = theme_.tokens;
	switch (tone) {
	case Tone::Accent:
		return ui::ToQColor(t.accentText);
	case Tone::Text:
		return ui::ToQColor(t.text);
	case Tone::Muted:
		return ui::ToQColor(t.textMuted);
	case Tone::Warning:
		return ui::ToQColor(t.warningText);
	case Tone::Danger:
		return ui::ToQColor(t.dangerText);
	case Tone::OnPrimary:
		break;
	}
	return ui::ToQColor(t.onGradient);
}

void TrigiPage::ApplyIcons()
{
	if (!themed_)
		return;
	const qreal dpr = devicePixelRatioF();
	const QColor disabled = ui::ToQColor(theme_.tokens.disabledFg);
	icons_.erase(std::remove_if(icons_.begin(), icons_.end(), [](const IconSlot &s) { return !s.widget; }),
		     icons_.end());
	for (const IconSlot &slot : icons_) {
		if (auto *button = qobject_cast<QPushButton *>(slot.widget.data()))
			button->setIcon(ui::Icon(slot.id, ToneColor(slot.tone), disabled, Qt::AlignLeft));
		else if (auto *label = qobject_cast<QLabel *>(slot.widget.data()))
			label->setPixmap(ui::IconPixmap(slot.id, ToneColor(slot.tone), slot.size, dpr));
	}
}

void TrigiPage::ApplyTheme(const ui::Theme &theme)
{
	theme_ = theme;
	themed_ = true;
	const ChatTextView::Style style{ui::ToQColor(theme_.tokens.text), ui::ToQColor(theme_.tokens.codeTint),
					ui::Fonts().mono};
	textViews_.erase(std::remove_if(textViews_.begin(), textViews_.end(), [](const auto &v) { return !v; }),
			 textViews_.end());
	for (const QPointer<ChatTextView> &view : textViews_)
		view->SetStyle(style);
	ApplyIcons();
	if (spinner_)
		spinner_->SetColor(ToneColor(Tone::Accent));
	UpdateCounter();
}

QLabel *TrigiPage::MakeIconLabel(ui::IconId id, Tone tone, int size, QWidget *parent)
{
	auto *label = new QLabel(parent);
	label->setFixedSize(size, size);
	icons_.push_back({label, id, tone, size});
	if (themed_)
		label->setPixmap(ui::IconPixmap(id, ToneColor(tone), size, devicePixelRatioF()));
	return label;
}

QPushButton *TrigiPage::MakeButton(const QString &text, const char *variant, QWidget *parent)
{
	auto *button = new QPushButton(text, parent);
	const QString kind = QString::fromLatin1(variant);
	button->setProperty("tgVariant", kind);
	button->setAutoDefault(false);
	button->setFocusPolicy(Qt::TabFocus);
	if (kind == QLatin1String("link"))
		button->setCursor(Qt::PointingHandCursor);
	if (themed_)
		ui::ApplyFont(button, kind == QLatin1String("link") ? ui::FontRole::Strong : ui::FontRole::Button,
			      theme_.basePt);
	return button;
}

void TrigiPage::SetButtonIcon(QPushButton *button, ui::IconId id, Tone tone)
{
	// The glyph at the left of a wider box: the box's extra width is the gap
	// to the text (like the dock's buttons).
	const bool link = button->property("tgVariant").toString() == QLatin1String("link");
	button->setIconSize(QSize(kIconSize + (link ? kLinkIconGap : kButtonIconGap), kIconSize));
	icons_.push_back({button, id, tone, kIconSize});
	if (themed_)
		button->setIcon(ui::Icon(id, ToneColor(tone), ui::ToQColor(theme_.tokens.disabledFg), Qt::AlignLeft));
}

QLabel *TrigiPage::MakeAvatar(QWidget *parent)
{
	QLabel *avatar = MakeIconLabel(ui::IconId::Bolt, Tone::Accent, 16, parent);
	avatar->setFixedSize(kAvatarSize, kAvatarSize);
	avatar->setAlignment(Qt::AlignCenter);
	avatar->setProperty("tgAvatar", QStringLiteral("true"));
	return avatar;
}

ChatTextView *TrigiPage::MakeTextView(QWidget *parent)
{
	auto *view = new ChatTextView(parent);
	if (themed_)
		view->SetStyle(
			{ui::ToQColor(theme_.tokens.text), ui::ToQColor(theme_.tokens.codeTint), ui::Fonts().mono});
	textViews_.push_back(view);
	return view;
}

bool TrigiPage::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == transcript_->viewport() && event->type() == QEvent::Resize)
		FitBubbles();
	return QWidget::eventFilter(watched, event);
}

void TrigiPage::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	// Narrow dialogs (large OBS fonts, small screens): "See what is sent"
	// goes under the box instead of cutting its label.
	if (diagnosticsLayout_)
		diagnosticsLayout_->setDirection(width() >= kNarrowWidth ? QBoxLayout::LeftToRight
									 : QBoxLayout::TopToBottom);
	FitBannerButtons();
}

void TrigiPage::FitBubbles()
{
	const int available = transcript_->viewport()->width() - messages_->contentsMargins().left() -
			      messages_->contentsMargins().right();
	bubbles_.erase(std::remove_if(bubbles_.begin(), bubbles_.end(), [](const auto &b) { return !b; }),
		       bubbles_.end());
	for (const QPointer<QWidget> &bubble : bubbles_) {
		const bool user = bubble->property("tgBubble").toString() == QLatin1String("user");
		const int width = user ? static_cast<int>(available * kUserBubbleShare)
				       : available - kAvatarSize - kAvatarGap;
		bubble->setMaximumWidth(std::max(120, width));
	}
}

void TrigiPage::ScrollToEnd()
{
	stickToEnd_ = true;
	QScrollBar *bar = transcript_->verticalScrollBar();
	bar->setValue(bar->maximum());
}

void TrigiPage::Announce(const QString &message, bool urgent)
{
	supportui::Announce(transcript_, message, urgent);
}

// ---- Messages -------------------------------------------------------------------------

void TrigiPage::InsertMessage(QWidget *row)
{
	// Before the stretch that keeps messages at the top, and before
	// "thinking", which always stays last.
	int index = messages_->count() - 1;
	if (thinkingRow_) {
		const int thinking = messages_->indexOf(thinkingRow_.data());
		if (thinking >= 0)
			index = thinking;
	}
	messages_->insertWidget(index, row);
	FitBubbles();
	ScrollToEnd();
}

QWidget *TrigiPage::AddUserMessage(const QString &text)
{
	auto *row = new QWidget(transcriptContent_);
	auto *layout = new QVBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);
	auto *line = new QHBoxLayout();
	line->setSpacing(0);
	line->addStretch(1);
	auto *bubble = new QFrame(row);
	bubble->setObjectName(QStringLiteral("tgUserBubble"));
	bubble->setProperty("tgBubble", QStringLiteral("user"));
	auto *bubbleLayout = new QVBoxLayout(bubble);
	bubbleLayout->setContentsMargins(12, 8, 12, 8);
	ChatTextView *view = MakeTextView(bubble);
	view->SetPlainText(text);
	bubbleLayout->addWidget(view);
	bubble->setAccessibleName(T("Trigi.YouSaid").arg(text));
	auto *copy = new QAction(T("Trigi.Copy"), bubble);
	connect(copy, &QAction::triggered, this, [text] { QGuiApplication::clipboard()->setText(text); });
	bubble->addAction(copy);
	bubble->setContextMenuPolicy(Qt::ActionsContextMenu);
	line->addWidget(bubble);
	layout->addLayout(line);

	// "Not sent", under the message, when it didn't go through.
	auto *notSent = new QWidget(row);
	notSent->setObjectName(QStringLiteral("tgNotSent"));
	{
		auto *notSentLayout = new QHBoxLayout(notSent);
		notSentLayout->setContentsMargins(0, 0, 0, 0);
		notSentLayout->setSpacing(4);
		auto *label = new QLabel(T("Trigi.NotSent"), notSent);
		label->setProperty("tgRole", QStringLiteral("hint"));
		label->setProperty("tgTone", QStringLiteral("danger"));
		notSentLayout->addStretch(1);
		notSentLayout->addWidget(MakeIconLabel(ui::IconId::AlertTriangle, Tone::Danger, 12, notSent));
		notSentLayout->addWidget(label);
	}
	notSent->setVisible(false);
	layout->addWidget(notSent);

	bubbles_.push_back(bubble);
	userMessages_ = true;
	InsertMessage(row);
	return row;
}

void TrigiPage::SetNotSent(QWidget *row, bool notSent)
{
	if (!row)
		return;
	if (auto *mark = row->findChild<QWidget *>(QStringLiteral("tgNotSent")))
		mark->setVisible(notSent);
	if (auto *bubble = row->findChild<QFrame *>(QStringLiteral("tgUserBubble")))
		bubble->setAccessibleDescription(notSent ? T("Trigi.NotSent") : QString());
}

QWidget *TrigiPage::AddTrigiBubble(const std::function<void(QFrame *, QVBoxLayout *)> &fill, const QString &plain)
{
	auto *row = new QWidget(transcriptContent_);
	auto *layout = new QVBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);
	auto *line = new QHBoxLayout();
	line->setSpacing(kAvatarGap);
	line->addWidget(MakeAvatar(row), 0, Qt::AlignTop);
	auto *bubble = new QFrame(row);
	bubble->setProperty("tgBubble", QStringLiteral("trigi"));
	auto *bubbleLayout = new QVBoxLayout(bubble);
	bubbleLayout->setContentsMargins(12, 9, 12, 9);
	bubbleLayout->setSpacing(5);
	fill(bubble, bubbleLayout);
	bubble->setAccessibleName(T("Trigi.Says").arg(plain));
	auto *copy = new QAction(T("Trigi.Copy"), bubble);
	connect(copy, &QAction::triggered, this, [plain] { QGuiApplication::clipboard()->setText(plain); });
	bubble->addAction(copy);
	bubble->setContextMenuPolicy(Qt::ActionsContextMenu);
	line->addWidget(bubble);
	line->addStretch(1);
	layout->addLayout(line);
	bubbles_.push_back(bubble);
	InsertMessage(row);
	return row;
}

QWidget *TrigiPage::AddTrigiMessage(const std::vector<chat::Line> &lines, const QString &plain)
{
	return AddTrigiBubble(
		[this, &lines](QFrame *bubble, QVBoxLayout *layout) {
			const int lineHeight = QFontMetrics(bubble->font()).lineSpacing();
			for (const chat::Line &line : lines) {
				if (line.kind == chat::LineKind::Break) {
					layout->addSpacing(4);
					continue;
				}
				// Escaped segments in <b> / <span class="code"> only (chat-text.hpp).
				ChatTextView *view = MakeTextView(bubble);
				view->SetLineHtml(Q(chat::LineHtml(line)));
				if (line.kind == chat::LineKind::Paragraph) {
					layout->addWidget(view);
					continue;
				}
				auto *item = new QHBoxLayout();
				item->setSpacing(8);
				if (line.kind == chat::LineKind::Bullet) {
					item->addWidget(MakeMarker(bubble, lineHeight), 0, Qt::AlignTop);
				} else {
					// "N." from LinePrefix(), as plain text.
					auto *number = new QLabel(Q(chat::LinePrefix(line)).trimmed(), bubble);
					number->setTextFormat(Qt::PlainText);
					number->setProperty("tgRole", QStringLiteral("data"));
					number->setProperty("tgTone", QStringLiteral("accent"));
					item->addWidget(number, 0, Qt::AlignTop);
				}
				item->addWidget(view, 1);
				layout->addLayout(item);
			}
		},
		plain);
}

QWidget *TrigiPage::AddTrigiText(const QString &text)
{
	return AddTrigiBubble(
		[this, &text](QFrame *bubble, QVBoxLayout *layout) {
			ChatTextView *view = MakeTextView(bubble);
			view->SetPlainText(text);
			layout->addWidget(view);
		},
		text);
}

void TrigiPage::AddReplyActions(QWidget *message, trigiui::ReplyAction action, bool feedback)
{
	auto *layout = qobject_cast<QVBoxLayout *>(message->layout());
	if (!layout)
		return;
	const int indent = kAvatarSize + kAvatarGap;
	if (action != trigiui::ReplyAction::None) {
		auto *row = new QHBoxLayout();
		row->setContentsMargins(indent, 0, 0, 0);
		QPushButton *button = nullptr;
		switch (action) {
		case trigiui::ReplyAction::CreateTicket:
			button = MakeButton(T("Trigi.CreateTicket"), "secondary", message);
			SetButtonIcon(button, ui::IconId::Ticket, Tone::Text);
			connect(button, &QPushButton::clicked, this, [this] { StartTicket(false); });
			break;
		case trigiui::ReplyAction::WriteToTeam:
			button = MakeButton(T("Trigi.WriteToTeam"), "secondary", message);
			SetButtonIcon(button, ui::IconId::MessageCircle, Tone::Text);
			connect(button, &QPushButton::clicked, this, [this] { StartTicket(true); });
			break;
		case trigiui::ReplyAction::OpenWebSupport:
			button = MakeButton(T("Trigi.OpenWebSupport"), "secondary", message);
			SetButtonIcon(button, ui::IconId::ExternalLink, Tone::Text);
			button->setAccessibleName(T("Trigi.OpenWebSupportName"));
			connect(button, &QPushButton::clicked, this, [this] {
				service_.NotifyHandoff();
				supportui::OpenWebPage(api::web::kAppSupport);
			});
			break;
		case trigiui::ReplyAction::None:
			break;
		}
		if (button) {
			replyButtons_.push_back(button);
			row->addWidget(button);
		}
		row->addStretch(1);
		layout->addLayout(row);
	}
	if (feedback) {
		auto *row = new QWidget(message);
		row->setObjectName(QStringLiteral("tgFeedback"));
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(indent, 0, 0, 0);
		rowLayout->setSpacing(6);
		auto *prompt = new QLabel(T("Trigi.Helpful"), row);
		prompt->setProperty("tgRole", QStringLiteral("hint"));
		auto *yes = MakeButton(T("Trigi.HelpfulYes"), "secondary", row);
		SetButtonIcon(yes, ui::IconId::ThumbsUp, Tone::Text);
		yes->setAccessibleName(T("Trigi.HelpfulYesName"));
		auto *no = MakeButton(T("Trigi.HelpfulNo"), "secondary", row);
		SetButtonIcon(no, ui::IconId::ThumbsDown, Tone::Text);
		no->setAccessibleName(T("Trigi.HelpfulNoName"));
		rowLayout->addWidget(prompt);
		rowLayout->addSpacing(4);
		rowLayout->addWidget(yes);
		rowLayout->addWidget(no);
		rowLayout->addStretch(1);
		layout->addWidget(row);
		connect(yes, &QPushButton::clicked, this, [this, row] { OnHelpful(row, true); });
		connect(no, &QPushButton::clicked, this, [this, row] { OnHelpful(row, false); });
		feedbackRow_ = row;
	}
	ApplyIcons();
	UpdateTabOrder();
}

void TrigiPage::DropUnansweredFeedback()
{
	if (!feedbackRow_)
		return;
	QWidget *row = feedbackRow_;
	feedbackRow_ = nullptr;
	if (auto *layout = qobject_cast<QVBoxLayout *>(row->parentWidget()->layout()))
		RemoveRow(layout, row);
}

void TrigiPage::ShowThinking(const QString &text)
{
	HideThinking();
	auto *row = new QWidget(transcriptContent_);
	auto *line = new QHBoxLayout(row);
	line->setContentsMargins(0, 0, 0, 0);
	line->setSpacing(kAvatarGap);
	line->addWidget(MakeAvatar(row), 0, Qt::AlignTop);
	auto *bubble = new QFrame(row);
	bubble->setProperty("tgBubble", QStringLiteral("status"));
	auto *bubbleLayout = new QHBoxLayout(bubble);
	bubbleLayout->setContentsMargins(12, 8, 12, 8);
	bubbleLayout->setSpacing(8);
	auto *spinner = new SpinnerIcon(14, bubble);
	spinner->SetColor(ToneColor(Tone::Accent));
	auto *label = new QLabel(text, bubble);
	label->setProperty("tgRole", QStringLiteral("hint"));
	label->setTextFormat(Qt::PlainText);
	bubbleLayout->addWidget(spinner, 0, Qt::AlignVCenter);
	bubbleLayout->addWidget(label, 1);
	line->addWidget(bubble);
	line->addStretch(1);
	row->setAccessibleName(text);
	messages_->insertWidget(messages_->count() - 1, row);
	thinkingRow_ = row;
	thinkingText_ = label;
	spinner_ = spinner;
	spinner->Start();
	slowTimer_->start();
	ScrollToEnd();
}

void TrigiPage::HideThinking()
{
	slowTimer_->stop();
	if (spinner_)
		spinner_->Stop();
	RemoveRow(messages_, thinkingRow_);
	thinkingRow_ = nullptr;
	thinkingText_ = nullptr;
	spinner_ = nullptr;
}

void TrigiPage::ClearConversation()
{
	if (Busy())
		return;
	DropUnansweredFeedback();
	for (int i = messages_->count() - 1; i >= 0; --i) {
		QWidget *row = messages_->itemAt(i)->widget();
		if (row)
			RemoveRow(messages_, row);
	}
	bubbles_.clear();
	replyButtons_.clear();
	failed_ = {};
	userMessages_ = false;
	if (problem_ != Problem::RateLimited)
		HideProblem();
	AddTrigiText(T("Trigi.Greeting"));
	Announce(T("Trigi.Cleared"), false);
	question_->setFocus(Qt::OtherFocusReason);
	UpdateControls();
}

// ---- Asking ---------------------------------------------------------------------------

void TrigiPage::Submit()
{
	if (Busy() || NoticeShown())
		return;
	const QString text = question_->toPlainText();
	const trigiui::QuestionCount count = trigiui::CountQuestion(text.toStdString());
	if (count.empty)
		return;
	if (count.tooLong) {
		Announce(T("Trigi.Error.TooLong").arg(support::kMaxQuestionChars), true);
		return;
	}
	Send(text.trimmed(), nullptr, true);
}

void TrigiPage::Send(const QString &text, QWidget *bubble, bool fromComposer)
{
	QPointer<TrigiPage> self(this);
	using Refusal = SupportService::AskRefusal;
	const Refusal refusal = service_.Ask(text, [self](const SupportService::AskResult &result) {
		if (self)
			self->OnReply(result, self->elapsed_.isValid() ? self->elapsed_.elapsed() : 0);
	});
	switch (refusal) {
	case Refusal::None:
		break;
	case Refusal::Busy:
	case Refusal::Empty:
		return;
	case Refusal::RateLimited:
		ShowProblem(Problem::RateLimited);
		return;
	case Refusal::TooLong:
		Announce(T("Trigi.Error.TooLong").arg(support::kMaxQuestionChars), true);
		return;
	case Refusal::NoticeNotAccepted:
		ShowNotice(true);
		return;
	case Refusal::NotAvailable: {
		using Gate = SupportService::TrigiGate;
		const Gate gate = service_.Gate();
		if (gate == Gate::SignedOut) {
			failed_ = {text, bubble};
			ShowProblem(Problem::SessionEnded);
		} else {
			HandOver(gate == Gate::Outdated          ? Problem::Outdated
				 : gate == Gate::AccountInactive ? Problem::AccountInactive
								 : Problem::Disabled,
				 text);
		}
		return;
	}
	}

	HideProblem();
	DropUnansweredFeedback();
	if (fromComposer) {
		bubble = AddUserMessage(text);
		asked_ << text;
		question_->clear();
	} else if (bubble) {
		SetNotSent(bubble, false);
	} else {
		bubble = AddUserMessage(text); // Its message was cleared from the view.
	}
	pending_ = {text, bubble};
	failed_ = {};
	elapsed_.start();
	ShowThinking(T("Trigi.Thinking"));
	Announce(T("Trigi.Thinking"), false);
	UpdateControls();
}

void TrigiPage::OnReply(const SupportService::AskResult &result, long long elapsedMs)
{
	HideThinking();
	const Pending sent = pending_;
	pending_ = {};
	const support::ReplyResult &reply = result.reply;
	if (!reply.Ok()) {
		const Problem problem = trigiui::ProblemFor(reply.status, reply.httpStatus, elapsedMs);
		failed_ = sent;
		SetNotSent(sent.bubble, true);
		if (trigiui::OpensForm(problem)) {
			UpdateControls();
			HandOver(problem, sent.text);
			return;
		}
		ShowProblem(problem);
		UpdateControls();
		return;
	}

	QWidget *message = nullptr;
	QString spoken;
	switch (reply.kind) {
	case support::ReplyKind::Answer:
		if (reply.lines.empty()) {
			// Nothing left after the safe render (an answer of emojis only).
			spoken = T("Trigi.EmptyReply");
			message = AddTrigiText(spoken);
			AddReplyActions(message, trigiui::ReplyAction::CreateTicket, false);
		} else {
			spoken = Q(chat::PlainText(reply.lines));
			message = AddTrigiMessage(reply.lines, spoken);
			AddReplyActions(message, trigiui::ActionFor(reply.next), trigiui::OffersFeedback(reply.kind));
		}
		break;
	case support::ReplyKind::Refused:
		spoken = T("Trigi.Refused");
		message = AddTrigiText(spoken);
		AddReplyActions(message,
				reply.next == support::NextStep::WriteToTeam ? trigiui::ReplyAction::WriteToTeam
									     : trigiui::ReplyAction::None,
				false);
		break;
	case support::ReplyKind::Unavailable:
		spoken = T("Trigi.Unavailable");
		message = AddTrigiText(spoken);
		AddReplyActions(message, trigiui::ReplyAction::CreateTicket, false);
		break;
	}
	// One announcement per reply.
	Announce(T("Trigi.Says").arg(spoken), false);
	UpdateControls();
}

// ---- "Was this helpful?" and tickets --------------------------------------------------

void TrigiPage::OnHelpful(QWidget *row, bool helpful)
{
	if (!row || Busy())
		return;
	service_.SendFeedback(helpful);
	QWidget *message = row->parentWidget();
	auto *layout = message ? qobject_cast<QVBoxLayout *>(message->layout()) : nullptr;
	if (!layout)
		return;
	const int index = layout->indexOf(row);
	if (feedbackRow_ == row)
		feedbackRow_ = nullptr;

	auto *outcome = new QWidget(message);
	auto *outcomeLayout = new QHBoxLayout(outcome);
	outcomeLayout->setContentsMargins(kAvatarSize + kAvatarGap, 0, 0, 0);
	outcomeLayout->setSpacing(6);
	QWidget *focusNext = question_;
	QString said;
	if (helpful) {
		// Resolved: the server closes the conversation; a new question
		// starts another one.
		service_.MarkResolved();
		service_.ForgetConversation();
		said = T("Trigi.Resolved");
		auto *text = new QLabel(said, outcome);
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		outcomeLayout->addWidget(MakeIconLabel(ui::IconId::Check, Tone::Accent, 14, outcome), 0, Qt::AlignTop);
		outcomeLayout->addWidget(text, 1);
	} else {
		said = T("Trigi.NotHelpful");
		auto *column = new QVBoxLayout();
		column->setSpacing(6);
		auto *text = new QLabel(said, outcome);
		text->setWordWrap(true);
		text->setTextFormat(Qt::PlainText);
		text->setProperty("tgRole", QStringLiteral("muted"));
		auto *ticket = MakeButton(T("Trigi.CreateTicket"), "secondary", outcome);
		SetButtonIcon(ticket, ui::IconId::Ticket, Tone::Text);
		connect(ticket, &QPushButton::clicked, this, [this] { StartTicket(false); });
		replyButtons_.push_back(ticket);
		auto *buttonRow = new QHBoxLayout();
		buttonRow->addWidget(ticket);
		buttonRow->addStretch(1);
		column->addWidget(text);
		column->addLayout(buttonRow);
		outcomeLayout->addLayout(column, 1);
		focusNext = ticket;
	}
	RemoveRow(layout, row);
	layout->insertWidget(index, outcome);
	ApplyIcons();
	UpdateTabOrder();
	Announce(said, false);
	focusNext->setFocus(Qt::OtherFocusReason);
	UpdateControls();
}

TrigiPage::TicketDraft TrigiPage::OwnWordsDraft() const
{
	TicketDraft draft;
	draft.message = asked_.join(QStringLiteral("\n\n"));
	draft.origin = asked_.isEmpty() ? TicketDraft::Origin::None : TicketDraft::Origin::OwnWords;
	return draft;
}

void TrigiPage::WriteToTeam()
{
	StartTicket(service_.HasConversation());
}

void TrigiPage::StartTicket(bool handoff)
{
	if (Busy())
		return;
	if (handoff)
		service_.NotifyHandoff();
	if (!service_.HasConversation()) {
		if (onOpenForm)
			onOpenForm(OwnWordsDraft(), Handover::UserChoice);
		return;
	}
	QPointer<TrigiPage> self(this);
	const bool started = service_.RequestSummary([self](const support::SummaryResult &result) {
		if (self)
			self->OnSummary(result, self->elapsed_.isValid() ? self->elapsed_.elapsed() : 0);
	});
	if (!started) {
		if (onOpenForm)
			onOpenForm(OwnWordsDraft(), Handover::UserChoice);
		return;
	}
	summarizing_ = true;
	elapsed_.start();
	HideProblem();
	DropUnansweredFeedback();
	ShowThinking(T("Trigi.Summarizing"));
	Announce(T("Trigi.Summarizing"), false);
	UpdateControls();
}

void TrigiPage::OnSummary(const support::SummaryResult &result, long long elapsedMs)
{
	summarizing_ = false;
	HideThinking();
	UpdateControls();
	if (result.Ok()) {
		TicketDraft draft;
		draft.subject = Q(result.subject);
		draft.message = Q(result.message);
		draft.origin = result.ai ? TicketDraft::Origin::SummaryAi : TicketDraft::Origin::Summary;
		if (draft.message.trimmed().isEmpty()) {
			// An empty draft helps nobody: the user's own words instead.
			draft.message = OwnWordsDraft().message;
			draft.origin = TicketDraft::Origin::OwnWords;
		}
		if (onOpenForm)
			onOpenForm(draft, Handover::UserChoice);
		return;
	}
	const Problem problem = trigiui::ProblemFor(result.status, result.httpStatus, elapsedMs);
	if (trigiui::OpensForm(problem)) {
		HandOver(problem, QString());
		return;
	}
	if (problem == Problem::SessionEnded) {
		ShowProblem(problem);
		return;
	}
	// The summary is a convenience: the form opens with the user's own words.
	if (onOpenForm)
		onOpenForm(OwnWordsDraft(), Handover::UserChoice);
}

void TrigiPage::HandOver(Problem problem, const QString &text)
{
	TicketDraft draft = OwnWordsDraft();
	if (!text.isEmpty() && !asked_.contains(text)) {
		draft.message = draft.message.isEmpty() ? text : draft.message + QStringLiteral("\n\n") + text;
		draft.origin = TicketDraft::Origin::OwnWords;
	}
	Handover reason = Handover::Disabled;
	switch (problem) {
	case Problem::Outdated:
		reason = Handover::Outdated;
		break;
	case Problem::AccountInactive:
		reason = Handover::AccountInactive;
		break;
	case Problem::SessionEnded:
		reason = Handover::SessionEnded;
		break;
	default:
		break;
	}
	if (onOpenForm)
		onOpenForm(draft, reason);
}

// ---- States ---------------------------------------------------------------------------

void TrigiPage::SetBanner(const char *tone, ui::IconId icon, const QString &text)
{
	ui::SetStyleProperty(banner_, "tgBanner", QString::fromLatin1(tone));
	const QString kind = QString::fromLatin1(tone);
	const Tone iconTone = kind == QLatin1String("danger")    ? Tone::Danger
			      : kind == QLatin1String("warning") ? Tone::Warning
								 : Tone::Accent;
	ui::SetStyleProperty(bannerText_, "tgTone", kind == QLatin1String("danger") ? kind : QString());
	icons_.erase(std::remove_if(icons_.begin(), icons_.end(),
				    [this](const IconSlot &s) { return s.widget.data() == bannerIcon_; }),
		     icons_.end());
	icons_.push_back({bannerIcon_, icon, iconTone, 16});
	bannerText_->setText(text);
	bannerCountdown_->setVisible(false);
	banner_->setVisible(true);
	ApplyIcons();
}

void TrigiPage::SetBannerButton(QPushButton *button, std::function<void()> &slot, const QString &text, ui::IconId icon,
				std::function<void()> action)
{
	slot = std::move(action);
	button->setVisible(static_cast<bool>(slot));
	if (!slot)
		return;
	button->setText(text);
	icons_.erase(std::remove_if(icons_.begin(), icons_.end(),
				    [button](const IconSlot &s) { return s.widget.data() == button; }),
		     icons_.end());
	SetButtonIcon(button, icon, Tone::Text);
}

void TrigiPage::FitBannerButtons()
{
	// The page's and the banner's margins.
	supportui::FitButtonRow(bannerButtons_, width() - 36 - 20);
}

void TrigiPage::ShowProblem(Problem problem)
{
	problem_ = problem;
	rateTimer_->stop();
	std::function<void()> retry;
	if (!failed_.text.isEmpty()) {
		retry = [this] {
			if (!failed_.text.isEmpty())
				Send(failed_.text, failed_.bubble, false);
		};
	}
	const QString retryText = T("Trigi.Retry");
	QString text;
	switch (problem) {
	case Problem::Offline:
		text = T("Trigi.Error.Offline");
		SetBanner("warning", ui::IconId::WifiOff, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, retryText, ui::IconId::Refresh, retry);
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, QString(), ui::IconId::Ticket, nullptr);
		break;
	case Problem::Timeout:
		text = T("Trigi.Error.Timeout");
		SetBanner("warning", ui::IconId::Clock, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, retryText, ui::IconId::Refresh, retry);
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, QString(), ui::IconId::Ticket, nullptr);
		break;
	case Problem::ServerBusy:
		text = T("Trigi.Error.Server");
		SetBanner("warning", ui::IconId::AlertTriangle, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, retryText, ui::IconId::Refresh, retry);
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, T("Trigi.WriteToTeam"),
				ui::IconId::MessageCircle, [this] { StartTicket(true); });
		break;
	case Problem::RateLimited:
		text = T("Trigi.Error.RateLimited");
		SetBanner("warning", ui::IconId::Clock, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, T("Trigi.CreateTicketShort"), ui::IconId::Ticket,
				[this] { StartTicket(false); });
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, QString(), ui::IconId::Ticket, nullptr);
		bannerCountdown_->setVisible(true);
		UpdateRateLimit();
		rateTimer_->start();
		break;
	case Problem::SessionEnded:
		text = T("Trigi.Error.SessionEnded");
		SetBanner("danger", ui::IconId::LogIn, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, T("Trigi.SignIn"), ui::IconId::LogIn, [this] {
			if (onSignIn)
				onSignIn();
		});
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, T("Trigi.ReportWithoutTrigi"),
				ui::IconId::MessageCircle, [this] { HandOver(Problem::SessionEnded, failed_.text); });
		break;
	case Problem::ConversationClosed:
		text = T("Trigi.Error.ConversationClosed");
		SetBanner("info", ui::IconId::Info, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, retryText, ui::IconId::Refresh, retry);
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, QString(), ui::IconId::Ticket, nullptr);
		break;
	case Problem::Generic:
	case Problem::Disabled:
	case Problem::Outdated:
	case Problem::AccountInactive:
		text = T("Trigi.Error.Generic");
		SetBanner("danger", ui::IconId::AlertTriangle, text);
		SetBannerButton(bannerPrimary_, bannerPrimaryAction_, retryText, ui::IconId::Refresh, retry);
		SetBannerButton(bannerSecondary_, bannerSecondaryAction_, T("Trigi.WriteToTeam"),
				ui::IconId::MessageCircle, [this] { StartTicket(true); });
		break;
	case Problem::None:
		HideProblem();
		return;
	}
	banner_->setAccessibleName(text);
	FitBannerButtons();
	Announce(text, trigiui::IsUrgent(problem));
	UpdateControls();
}

void TrigiPage::HideProblem()
{
	problem_ = Problem::None;
	rateTimer_->stop();
	banner_->setVisible(false);
	bannerPrimaryAction_ = nullptr;
	bannerSecondaryAction_ = nullptr;
}

void TrigiPage::UpdateRateLimit()
{
	if (problem_ != Problem::RateLimited) {
		rateTimer_->stop();
		return;
	}
	const int left = service_.RetryAfterSecondsLeft();
	if (left > 0) {
		bannerCountdown_->setText(Q(trigiui::FormatCountdown(left)));
		return;
	}
	// Over: say it once, and offer the question that didn't go through.
	rateTimer_->stop();
	problem_ = Problem::None;
	const QString text = T("Trigi.Error.RateLimitedOver");
	SetBanner("info", ui::IconId::Check, text);
	std::function<void()> retry;
	if (!failed_.text.isEmpty()) {
		retry = [this] {
			if (!failed_.text.isEmpty())
				Send(failed_.text, failed_.bubble, false);
		};
	}
	SetBannerButton(bannerPrimary_, bannerPrimaryAction_, T("Trigi.Retry"), ui::IconId::Refresh, retry);
	SetBannerButton(bannerSecondary_, bannerSecondaryAction_, QString(), ui::IconId::Ticket, nullptr);
	banner_->setAccessibleName(text);
	Announce(text, false);
	UpdateControls();
}

// ---- Controls -------------------------------------------------------------------------

void TrigiPage::UpdateCounter()
{
	const trigiui::QuestionCount count = trigiui::CountQuestion(question_->toPlainText().toStdString());
	counter_->setText(QStringLiteral("%1/%2").arg(count.chars).arg(support::kMaxQuestionChars));
	ui::SetStyleProperty(counter_, "tgTone", count.tooLong ? QStringLiteral("danger") : QString());
	ui::SetStyleProperty(question_, "tgInvalid", count.tooLong ? QStringLiteral("true") : QStringLiteral("false"));
	counter_->setAccessibleName(count.tooLong
					    ? T("Trigi.Error.TooLong").arg(support::kMaxQuestionChars)
					    : T("Trigi.CounterName").arg(count.chars).arg(support::kMaxQuestionChars));
	TrackEmpty(question_, question_->document()->isEmpty());
	if (count.tooLong && !overLimit_)
		Announce(T("Trigi.Error.TooLong").arg(support::kMaxQuestionChars), true);
	overLimit_ = count.tooLong;
	UpdateControls();
}

void TrigiPage::UpdateTabOrder()
{
	// Reading order: the notice's link or the transcript and the buttons in
	// it, the state banner, the question and Send, the technical data, and
	// the notice's answers.
	setTabOrder(privacyButton_, transcript_);
	QWidget *previous = transcript_;
	for (QPushButton *button : transcriptContent_->findChildren<QPushButton *>()) {
		if (HiddenOnPurpose(button, transcriptContent_))
			continue;
		setTabOrder(previous, button);
		previous = button;
	}
	for (QWidget *next :
	     std::initializer_list<QWidget *>{bannerPrimary_, bannerSecondary_, question_, sendButton_,
					      diagnosticsCheck_, diagnosticsSee_, acceptButton_, declineButton_}) {
		setTabOrder(previous, next);
		previous = next;
	}
}

void TrigiPage::UpdateControls()
{
	if (!sendButton_ || !question_ || !bannerPrimary_ || !diagnosticsCheck_)
		return; // Still building.
	const bool busy = Busy();
	const trigiui::QuestionCount count = trigiui::CountQuestion(question_->toPlainText().toStdString());
	const bool waiting = service_.RetryAfterSecondsLeft() > 0;
	sendButton_->setEnabled(!busy && !NoticeShown() && !count.empty && !count.tooLong && !waiting);
	replyButtons_.erase(std::remove_if(replyButtons_.begin(), replyButtons_.end(),
					   [](const auto &b) { return !b; }),
			    replyButtons_.end());
	for (const QPointer<QPushButton> &button : replyButtons_)
		button->setEnabled(!busy);
	if (feedbackRow_) {
		for (QPushButton *button : feedbackRow_->findChildren<QPushButton *>())
			button->setEnabled(!busy);
	}
	bannerPrimary_->setEnabled(!busy);
	bannerSecondary_->setEnabled(!busy);
	// The technical data goes once, with a new conversation: it can't change
	// for the one in progress.
	const bool locked = service_.HasConversation();
	diagnosticsCheck_->setEnabled(!busy && !locked);
	diagnosticsCheck_->setToolTip(locked ? T("Trigi.Diagnostics.Locked") : T("Trigi.Diagnostics.Hint"));
	if (onStateChanged)
		onStateChanged();
}

void TrigiPage::ShowDiagnosticsPreview()
{
	supportui::OpenTextPreview(window(), theme_, T("Trigi.Diagnostics.PreviewTitle"),
				   T("Trigi.Diagnostics.PreviewNote"), service_.DiagnosticsPreview());
}

} // namespace trigglow
