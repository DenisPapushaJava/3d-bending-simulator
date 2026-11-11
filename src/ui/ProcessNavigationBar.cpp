#include "ProcessNavigationBar.h"
#include "ProcessStepButton.h"
#include <QAction>
#include <QButtonGroup>
#include <QHBoxLayout>

namespace bending::ui {
ProcessNavigationBar::ProcessNavigationBar(QAction &drawing, QAction &dxf, QWidget *parent)
    : QToolBar(QStringLiteral("Этапы работы"), parent), group_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("processNavigation"));
    setMovable(false);
    setFloatable(false);
    setAllowedAreas(Qt::BottomToolBarArea);
    setContextMenuPolicy(Qt::PreventContextMenu);
    auto *panel = new QWidget(this);
    auto *layout = new QHBoxLayout(panel);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(8);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    panel->setStyleSheet(QStringLiteral(
        "QToolButton{background:#FFFFFF;color:#526171;border:1px solid "
        "#DCE3E9;border-radius:4px;padding:8px 16px;font-size:14px;}"
        "QToolButton:checked{background:#E5F3F3;color:#087F83;border:2px solid #087F83;}"
        "QToolButton:hover{background:#F0F3F6;}QToolButton:focus{border:2px solid #087F83;}"));
    group_->setExclusive(true);
    const std::array<QString, 3> names{QStringLiteral("Деталь"), QStringLiteral("Инструмент"),
                                       QStringLiteral("Гибка")};
    for (int i = 0; i < 3; ++i) {
        auto *button = new ProcessStepButton(i, names[static_cast<std::size_t>(i)], panel);
        steps_[static_cast<std::size_t>(i)] = button;
        button->setObjectName(QStringLiteral("processStage%1").arg(i));
        button->setShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(i + 1)));
        button->setToolTip(names[static_cast<std::size_t>(i)] +
                           QStringLiteral(" · Alt+%1").arg(i + 1));
        group_->addButton(button, i);
        layout->addWidget(button, 1);
    }
    setCurrentStage(0);
    connect(group_, &QButtonGroup::idClicked, this, &ProcessNavigationBar::stageRequested);
    const auto addExport = [&](QAction &action, int icon, const QString &name,
                               const char *objectName) {
        auto *button = new ProcessStepButton(icon, name, panel);
        button->setObjectName(QString::fromLatin1(objectName));
        const auto refresh = [button, &action] {
            button->setEnabled(action.isEnabled());
            button->setStatus(action.statusTip());
        };
        refresh();
        layout->addWidget(button, 1);
        connect(button, &QToolButton::clicked, &action, &QAction::trigger);
        connect(&action, &QAction::changed, button, refresh);
    };
    addExport(drawing, 3, QStringLiteral("Чертёж / PDF"), "drawingStageButton");
    addExport(dxf, 4, QStringLiteral("Развёртка / DXF"), "flatDxfStageButton");
    addWidget(panel);
}
void ProcessNavigationBar::setCurrentStage(int stage) {
    if (auto *button = group_->button(stage))
        button->setChecked(true);
}
void ProcessNavigationBar::setStageStatus(int stage, const QString &status) {
    if (stage >= 0 && static_cast<std::size_t>(stage) < steps_.size())
        steps_[static_cast<std::size_t>(stage)]->setStatus(status);
}
} // namespace bending::ui
