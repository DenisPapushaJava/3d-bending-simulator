#include "ToolEndButtons.h"
#include "ToolAssemblyDiagram.h"
#include <QAction>
#include <QHBoxLayout>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>
#include <algorithm>
namespace bending::ui {
namespace {
QString label(const core::ToolSegment &s) {
    return QStringLiteral("%1 мм · a %2 · R%3 · h %4")
        .arg(s.lengthMm)
        .arg(s.horn->projectionMm)
        .arg(s.horn->radiusMm)
        .arg(s.horn->tipHeightMm);
}
} // namespace
ToolEndButtons::ToolEndButtons(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    for (std::size_t side = 0; side < 2; ++side) {
        auto *button = new QToolButton(this);
        buttons_[side] = button;
        button->setObjectName(side ? "assemblyRightHorn" : "assemblyLeftHorn");
        button->setCheckable(true);
        button->setIcon(toolHornEndIcon(side != 0, devicePixelRatioF()));
        button->setIconSize({28, 28});
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setMinimumHeight(44);
        connect(button, &QToolButton::toggled, this, [this, side](bool on) {
            const auto snapshot = source_;
            if (snapshot && onSelected)
                onSelected(*snapshot, side != 0, on ? chosen_[side] : std::nullopt);
        });
        layout->addWidget(button, 1);
    }
}
void ToolEndButtons::setSource(const core::ToolPreset &tool,
                               const std::optional<core::ToolAssembly> &assembly) {
    const bool changed = !source_ || *source_ != tool;
    if (changed) {
        source_ = std::make_shared<const core::ToolPreset>(tool);
        chosen_ = {};
    }
    for (std::size_t side = 0; side < 2; ++side) {
        auto *button = buttons_[side];
        std::optional<std::size_t> end;
        if (tool.segments && assembly && !assembly->pieces.empty()) {
            const auto row = side ? assembly->pieces.back() : assembly->pieces.front();
            if (row < tool.segments->size() &&
                tool.segments->at(row).shape ==
                    (side ? core::SegmentShape::RightHorn : core::SegmentShape::LeftHorn))
                end = row;
        }
        std::vector<std::size_t> variants;
        if (tool.segments)
            for (std::size_t i = 0; i < tool.segments->size(); ++i) {
                const auto &s = tool.segments->at(i);
                if (s.shape ==
                        (side ? core::SegmentShape::RightHorn : core::SegmentShape::LeftHorn) &&
                    s.horn && s.quantity && core::usableToolSegment(s))
                    variants.push_back(i);
            }
        if (end)
            chosen_[side] = end;
        else if (!chosen_[side] && !variants.empty())
            chosen_[side] = variants.front();
        const QSignalBlocker blocker(button);
        button->setChecked(end.has_value());
        button->setEnabled(!variants.empty());
        button->setText(
            (side ? QStringLiteral("Правый сапожок\n") : QStringLiteral("Левый сапожок\n")) +
            (end                ? QStringLiteral("%1 мм").arg(tool.segments->at(*end).lengthMm)
             : variants.empty() ? QStringLiteral("Нет в наличии")
                                : QStringLiteral("Добавить")));
        button->setToolTip(
            end && tool.segments->at(*end).horn ? label(tool.segments->at(*end))
            : variants.empty()
                ? QStringLiteral("Настройте выемку и количество в библиотеке инструмента.")
                : QStringLiteral("Добавить крайнюю секцию из наличия."));
        button->setToolTip(button->toolTip() +
                           QStringLiteral("\nСапожки указаны в обычной ориентации пуансона."));
        if (changed) {
            if (auto *old = button->menu()) {
                button->setMenu(nullptr);
                old->deleteLater();
            }
            if (variants.size() > 1) {
                auto *menu = new QMenu(button);
                for (auto row : variants) {
                    auto *action = menu->addAction(label(tool.segments->at(row)));
                    action->setData(static_cast<qulonglong>(row));
                    connect(action, &QAction::triggered, this,
                            [this, snapshot = source_, side, row] {
                                if (onSelected)
                                    onSelected(*snapshot, side != 0, row);
                            });
                }
                button->setMenu(menu);
                button->setPopupMode(QToolButton::MenuButtonPopup);
            } else
                button->setPopupMode(QToolButton::DelayedPopup);
        }
        if (auto *menu = button->menu())
            for (auto *action : menu->actions()) {
                const auto row = static_cast<std::size_t>(action->data().toULongLong());
                const auto used =
                    assembly ? static_cast<unsigned>(std::count(assembly->pieces.begin(),
                                                                assembly->pieces.end(), row))
                             : 0;
                action->setCheckable(true);
                action->setChecked(end == row);
                action->setEnabled(end == row || used < tool.segments->at(row).quantity);
            }
    }
}
} // namespace bending::ui
