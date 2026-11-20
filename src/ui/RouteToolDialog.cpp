#include "RouteToolDialog.h"

#include "ToolAssemblyDialog.h"
#include "ToolContourButton.h"
#include "ToolPickerPopup.h"
#include "ToolSelectionWidget.h"
#include "bending/core/ToolGeometry.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace bending::ui {
RouteToolDialog::RouteToolDialog(QWidget *parent, QString scope, core::ToolCatalog catalog,
                                 Changes displayed, std::array<bool, 2> mixed, double span,
                                 std::optional<double> thickness, double maximumGap, bool advanced,
                                 std::function<QString(const Changes &)> validate)
    : QDialog(parent), catalog_(std::move(catalog)), displayed_(std::move(displayed)), span_(span),
      maximumGap_(maximumGap), thickness_(thickness) {
    setObjectName("routeToolDialog");
    setWindowTitle(QStringLiteral("Оснастка — %1").arg(scope));
    setMinimumWidth(420);
    auto *layout = new QVBoxLayout(this);
    auto *heading = new QLabel(std::move(scope), this);
    heading->setObjectName("routeToolScope");
    auto font = heading->font();
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    layout->addWidget(new QLabel(QStringLiteral("Нажмите на профиль, чтобы заменить."), this));
    auto *cards = new QHBoxLayout;
    layout->addLayout(cards);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        auto &row = rows_[i];
        row.mixed = mixed[i];
        auto *column = new QVBoxLayout;
        cards->addLayout(column, 1);
        const auto kind = i == 0 ? core::ToolKind::Punch : core::ToolKind::Die;
        auto *title =
            new QLabel(i == 0 ? QStringLiteral("Пуансон") : QStringLiteral("Матрица"), this);
        column->addWidget(title);
        row.profile = new ToolContourButton(kind, this, true);
        row.profile->setObjectName(i == 0 ? "routeToolPunch" : "routeToolDie");
        row.profile->setCheckable(false);
        column->addWidget(row.profile);
        row.summary = new QLabel(this);
        row.summary->setTextFormat(Qt::PlainText);
        row.summary->setWordWrap(true);
        column->addWidget(row.summary);
        row.sections = new QPushButton(QStringLiteral("Секции…"), this);
        column->addWidget(row.sections);
        connect(row.profile, &QToolButton::clicked, this, [this, i] { pick(i); });
        connect(row.sections, &QPushButton::clicked, this,
                [this, i] { editSections(i, rows_[i].piece); });
    }
    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);
    auto *buttons = new QDialogButtonBox(this);
    if (advanced) {
        auto *more =
            buttons->addButton(QStringLiteral("Другие настройки…"), QDialogButtonBox::ActionRole);
        more->setObjectName("routeToolAdvanced");
        connect(more, &QPushButton::clicked, this, [this] { done(Advanced); });
    }
    auto *cancel = buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    apply_ = buttons->addButton(QStringLiteral("Применить"), QDialogButtonBox::AcceptRole);
    apply_->setObjectName("routeToolApply");
    apply_->setEnabled(false);
    cancel->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(apply_, &QPushButton::clicked, this, [this, validate = std::move(validate)] {
        const auto error = validate(changes_);
        if (error.isEmpty())
            accept();
        else {
            error_->setText(error);
            error_->show();
        }
    });
    for (std::size_t i = 0; i < rows_.size(); ++i)
        refreshRow(i);
}

void RouteToolDialog::openFor(core::ToolKind kind, std::optional<std::size_t> piece) {
    const std::size_t i = kind == core::ToolKind::Punch ? 0 : 1;
    rows_[i].piece = piece;
    QTimer::singleShot(0, this, [this, i] {
        rows_[i].profile->setFocus();
        pick(i);
    });
}

void RouteToolDialog::pick(std::size_t i) {
    const auto &current = changes_[i] ? changes_[i] : displayed_[i];
    ToolPickerPopup picker(
        catalog_, i == 0 ? core::ToolKind::Punch : core::ToolKind::Die,
        current ? std::optional{current->profile} : std::nullopt, current ? current->groove : 0,
        [this](const auto &tool, auto groove) {
            return ToolSelectionWidget::profileIssue(tool, groove, thickness_);
        },
        false, rows_[i].profile, current && current->reversed);
    if (picker.exec() != QDialog::Accepted || !picker.choice())
        return;
    const auto &[profile, groove] = *picker.choice();
    const bool same = current && current->profile == profile;
    if (same && current->groove == groove && (!rows_[i].mixed || changes_[i]) &&
        (!profile.segments || current->assembly))
        return;
    if (!rows_[i].mixed && changes_[i] && displayed_[i] && displayed_[i]->profile == profile &&
        displayed_[i]->groove == groove) {
        changes_[i].reset();
        refreshRow(i);
        return;
    }
    Tool next{profile, same ? current->assembly : std::nullopt, groove,
              i == 0 && (same ? current->reversed
                              : core::defaultPunchOrientation(profile).value_or(false))};
    if (profile.segments && (!same || !next.assembly || (rows_[i].mixed && !changes_[i]))) {
        const auto plan =
            i == 0 ? core::planPunchAssemblies(*profile.segments, span_, 200000, maximumGap_).best()
                   : core::planToolSegments(*profile.segments, span_, 200000, maximumGap_);
        next.assembly = plan.status == core::SegmentPlanStatus::Complete
                            ? std::optional{plan.assembly}
                            : std::nullopt;
    }
    changes_[i] = std::move(next);
    rows_[i].piece.reset();
    refreshRow(i);
}

void RouteToolDialog::editSections(std::size_t i, std::optional<std::size_t> piece) {
    const auto &current = changes_[i] ? changes_[i] : displayed_[i];
    if (!current || !current->profile.segments)
        return;
    ToolAssemblyDialog dialog(current->profile, current->assembly, this, span_, maximumGap_);
    if (piece)
        dialog.selectPiece(*piece);
    if (dialog.exec() == QDialog::Accepted && dialog.assembly() &&
        dialog.assembly() != current->assembly) {
        auto next = *current;
        next.assembly = dialog.assembly();
        changes_[i] = std::move(next);
        refreshRow(i);
    }
}

void RouteToolDialog::refreshRow(std::size_t i) {
    auto &row = rows_[i];
    const auto &current = changes_[i] ? changes_[i] : displayed_[i];
    row.profile->setTool(current ? std::optional{current->profile} : std::nullopt,
                         current ? current->groove : 0, current && current->reversed);
    row.profile->setAccessibleDescription(QStringLiteral("Открыть список инструментов"));
    row.sections->setEnabled(current && current->profile.segments);
    QString summary;
    if (current) {
        summary = QString::fromStdString(current->profile.code) + QStringLiteral(" · ");
        if (row.mixed && !changes_[i])
            summary += QStringLiteral("разные по шагам");
        else if (const auto length = core::toolLength(current->profile, current->assembly)) {
            summary += QStringLiteral("%1 мм").arg(QLocale().toString(*length, 'f', 2));
            if (current->assembly)
                summary += QStringLiteral(" · секций %1").arg(current->assembly->pieces.size());
            if (*length + 0.005 < span_)
                summary += QStringLiteral("\nКороче линии гиба на %1 мм")
                               .arg(QLocale().toString(span_ - *length, 'f', 2));
        } else
            summary += QStringLiteral("подберите секции");
    } else
        summary = QStringLiteral("Не выбрано");
    row.summary->setText(summary);
    error_->hide();
    apply_->setEnabled(changes_[0].has_value() || changes_[1].has_value());
}
} // namespace bending::ui
