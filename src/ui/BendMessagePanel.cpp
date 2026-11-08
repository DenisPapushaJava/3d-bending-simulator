#include "BendMessagePanel.h"
#include <QLabel>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
namespace bending::ui {
BendMessagePanel::BendMessagePanel(QWidget *parent) : QFrame(parent) {
    setObjectName("bendMessages");
    setAccessibleName(QStringLiteral("Сообщения проверки гибки"));
    setStyleSheet("QFrame#bendMessages{background:#F4F7F9;border:1px solid #708090;"
                  "border-radius:5px;}QToolButton{border:0;text-align:left;padding:2px;}"
                  "QLabel{background:transparent;color:#1C2938;}"
                  "QToolButton{color:#1C2938;}");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(4);
    heading_ = new QToolButton(this);
    heading_->setObjectName("bendMessagesToggle");
    heading_->setCheckable(true);
    heading_->setFocusPolicy(Qt::StrongFocus);
    heading_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    heading_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    heading_->setToolTip(QStringLiteral("Показать или свернуть подробности"));
    layout->addWidget(heading_);
    summary_ = new QLabel(this);
    summary_->setObjectName("bendMessagesSummary");
    summary_->setTextFormat(Qt::PlainText);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    scroll_ = new QScrollArea(this);
    scroll_->setObjectName("bendMessagesDetails");
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setMaximumHeight(150);
    details_ = new QLabel(scroll_);
    details_->setObjectName("bendMessagesText");
    details_->setTextFormat(Qt::PlainText);
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details_->setAlignment(Qt::AlignTop);
    scroll_->setWidget(details_);
    layout->addWidget(scroll_);
    connect(heading_, &QToolButton::toggled, this, [this] { refresh(); });
    hide();
}
void BendMessagePanel::setActive(bool active) {
    if (active_ != active) {
        active_ = active;
        refresh();
    }
}
void BendMessagePanel::setMessages(std::vector<BendMessage> messages) {
    std::stable_sort(messages.begin(), messages.end(),
                     [](const auto &a, const auto &b) { return a.severity > b.severity; });
    if (messages_ == messages)
        return;
    messages_ = std::move(messages);
    refresh();
}
void BendMessagePanel::refresh() {
    setVisible(active_ && !messages_.empty());
    if (messages_.empty())
        return;
    const auto &first = messages_.front();
    heading_->setIcon(style()->standardIcon(
        first.severity == BendMessageSeverity::Error     ? QStyle::SP_MessageBoxCritical
        : first.severity == BendMessageSeverity::Warning ? QStyle::SP_MessageBoxWarning
                                                         : QStyle::SP_MessageBoxInformation));
    heading_->setText(first.title +
                      (heading_->isChecked() ? QStringLiteral(" ▴") : QStringLiteral(" ▾")));
    heading_->setAccessibleDescription(heading_->isChecked()
                                           ? QStringLiteral("Подробности открыты")
                                           : QStringLiteral("Подробности свёрнуты"));
    QStringList titles, details;
    for (std::size_t i = 0; i < messages_.size(); ++i) {
        const auto &m = messages_[i];
        if (i && i < 3)
            titles << m.title;
        details << (m.detail.isEmpty() ? m.title : m.title + QLatin1Char('\n') + m.detail);
    }
    if (messages_.size() > 3)
        titles << QStringLiteral("Ещё сообщений: %1").arg(messages_.size() - 3);
    summary_->setText(titles.join(QLatin1Char('\n')));
    summary_->setVisible(!titles.empty() && !heading_->isChecked());
    details_->setText(details.join(QStringLiteral("\n\n")));
    scroll_->setVisible(heading_->isChecked());
    setAccessibleDescription(details_->text());
    updateGeometry();
}
QSize BendMessagePanel::sizeHint() const {
    return {370, heading_->isChecked() ? 205 : 43 + (summary_->isVisible() ? 44 : 0)};
}
} // namespace bending::ui
