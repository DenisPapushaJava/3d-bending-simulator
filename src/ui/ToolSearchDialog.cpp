#include "ToolSearchDialog.h"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
namespace bending::ui {
ToolSearchDialog::ToolSearchDialog(QWidget *parent) : QDialog(parent) {
    setObjectName("checkedToolProgress");
    setWindowTitle(QStringLiteral("Подбор инструмента"));
    setWindowModality(Qt::NonModal);
    setMinimumWidth(430);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    title_ = new QLabel(this);
    auto font = title_->font();
    font.setPointSize(font.pointSize() + 3);
    font.setBold(true);
    title_->setFont(font);
    layout->addWidget(title_);
    tool_ = new QLabel(this);
    tool_->setTextFormat(Qt::PlainText);
    tool_->setStyleSheet("color:#526171");
    layout->addWidget(tool_);
    for (auto &step : steps_) {
        step = new QLabel(this);
        step->setMargin(8);
        layout->addWidget(step);
    }
    busyIndicator_ = new QProgressBar(this);
    busyIndicator_->setObjectName("checkedToolBusy");
    busyIndicator_->setRange(0, 0);
    busyIndicator_->setTextVisible(false);
    busyIndicator_->setFixedHeight(6);
    busyIndicator_->setStyleSheet(
        "QProgressBar{border:0;background:#E7EDF2;border-radius:3px;}"
        "QProgressBar::chunk{background:#087F85;width:40px;border-radius:3px;}");
    layout->addWidget(busyIndicator_);
    detail_ = new QLabel(this);
    detail_->setTextFormat(Qt::PlainText);
    detail_->setWordWrap(true);
    detail_->setStyleSheet("color:#526171");
    layout->addWidget(detail_);
    auto *buttons = new QHBoxLayout;
    auto *ways = new QToolButton(this);
    ways->setText(QStringLiteral("Другие способы"));
    ways->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(ways);
    auto *quick = menu->addAction(QStringLiteral("Быстро по параметрам"));
    quick->setObjectName("checkedToolQuick");
    connect(quick, &QAction::triggered, this, [this] {
        if (onQuick)
            onQuick();
    });
    auto *manual = menu->addAction(QStringLiteral("Выбрать вручную"));
    manual->setObjectName("checkedToolManual");
    connect(manual, &QAction::triggered, this, [this] {
        if (onManual)
            onManual();
    });
    ways->setMenu(menu);
    buttons->addWidget(ways);
    buttons->addStretch();
    action_ = new QPushButton(this);
    action_->setObjectName("checkedToolProgressAction");
    connect(action_, &QPushButton::clicked, this, [this] {
        if (!busy_ && resumable_) {
            if (onContinue)
                onContinue();
        } else
            reject();
    });
    buttons->addWidget(action_);
    layout->addLayout(buttons);
}
void ToolSearchDialog::begin() {
    busy_ = true;
    resumable_ = false;
    action_->setText(QStringLiteral("Отменить"));
    busyIndicator_->show();
    progress({}, {});
    show();
    raise();
}
void ToolSearchDialog::progress(const application::BendRouteProgress &p, const QString &tool) {
    static const std::array<QString, 3> names{QStringLiteral("Готовим деталь"),
                                              QStringLiteral("Подбираем инструмент"),
                                              QStringLiteral("Проверяем гибку")};
    const auto active = static_cast<std::size_t>(p.phase);
    if (active >= steps_.size())
        return;
    title_->setText(names[active]);
    tool_->setText(tool);
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        steps_[i]->setText(
            (i < active ? QStringLiteral("✓  ") : QString::number(i + 1) + QStringLiteral("   ")) +
            names[i]);
        steps_[i]->setStyleSheet(i == active ? "background:#E7F4F4;color:#087F85;border-radius:5px;"
                                 : i < active ? "color:#087F85;"
                                              : "color:#83909F;");
    }
    detail_->setText(p.checkedOperations
                         ? QStringLiteral("Проверено установок: %1").arg(p.checkedOperations)
                         : QString{});
}
void ToolSearchDialog::finish(bool resumable) {
    busy_ = false;
    resumable_ = resumable;
    busyIndicator_->hide();
    title_->setText(resumable ? QStringLiteral("Проверка не завершена")
                              : QStringLiteral("Проверенный вариант не найден"));
    detail_->setText(
        resumable ? QStringLiteral("Можно продолжить с готовыми проверками или выбрать "
                                   "инструмент другим способом.")
                  : QStringLiteral("Можно попробовать быстрый или ручной выбор инструмента."));
    action_->setText(resumable ? QStringLiteral("Продолжить") : QStringLiteral("Закрыть"));
}
void ToolSearchDialog::reject() {
    if (onCancel)
        onCancel();
    QDialog::reject();
}
} // namespace bending::ui
