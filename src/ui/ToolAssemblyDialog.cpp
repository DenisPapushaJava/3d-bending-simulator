#include "ToolAssemblyDialog.h"
#include "ToolAssemblyDiagram.h"
#include "ToolEndButtons.h"
#include "bending/core/ToolAssemblyEdits.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace bending::ui {
ToolAssemblyDialog::ToolAssemblyDialog(core::ToolPreset tool,
                                       std::optional<core::ToolAssembly> initial, QWidget *parent,
                                       std::optional<double> requiredMm, double maximumGapMm)
    : QDialog(parent), tool_(std::move(tool)), initial_(std::move(initial)),
      maximumGapMm_(maximumGapMm) {
    setObjectName("toolAssemblyDialog");
    setWindowTitle((tool_.kind == core::ToolKind::Punch ? QStringLiteral("Секции пуансона · ")
                                                        : QStringLiteral("Секции матрицы · ")) +
                   QString::fromStdString(tool_.code));
    setWindowModality(Qt::WindowModal);
    resize(680, tool_.kind == core::ToolKind::Punch ? 660 : 520);
    auto *layout = new QVBoxLayout(this);
    auto *caption = new QLabel(tool_.kind == core::ToolKind::Punch
                                   ? QStringLiteral("Секции вдоль инструмента · обычная ориентация")
                                   : QStringLiteral("Секции вдоль инструмента"),
                               this);
    layout->addWidget(caption);
    auto *diagram = new ToolAssemblyDiagram(tool_, this);
    diagram_ = diagram;
    diagram->intervals = [this] {
        return tool_.segments ? core::assemblyIntervals(*tool_.segments, draft_) : std::nullopt;
    };
    diagram->required = [this] { return target_ ? target_->value() : 0; };
    diagram->selected = [this] { return rows_->currentRow(); };
    diagram->onSelected = [this](int row) {
        rows_->selectRow(row);
        rows_->scrollTo(rows_->model()->index(row, 0));
    };
    layout->addWidget(diagram);
    if (tool_.kind == core::ToolKind::Punch) {
        ends_ = new ToolEndButtons(this);
        ends_->onSelected = [this](const core::ToolPreset &snapshot, bool right,
                                   std::optional<std::size_t> row) {
            if (snapshot == tool_)
                setEnd(right ? 1 : 0, row);
        };
        layout->addWidget(ends_);
        auto *fit = new QHBoxLayout;
        fit->addWidget(new QLabel(QStringLiteral("Длина подбора, мм"), this));
        target_ = new QDoubleSpinBox(this);
        target_->setObjectName("assemblyRequiredLength");
        target_->setAccessibleName(QStringLiteral("Длина подбора, мм"));
        target_->setRange(0, 100000);
        target_->setDecimals(3);
        target_->setSpecialValueText(QStringLiteral("Укажите длину"));
        target_->setValue(requiredMm.value_or(0));
        fit->addWidget(target_, 1);
        fitMiddle_ = new QPushButton(QStringLiteral("Подобрать середину"), this);
        fitMiddle_->setObjectName("assemblyFitMiddle");
        fitMiddle_->setToolTip(
            QStringLiteral("Сохранить крайние сапожки и подобрать прямые секции между ними."));
        fit->addWidget(fitMiddle_);
        layout->addLayout(fit);
        connect(target_, &QDoubleSpinBox::valueChanged, this, [this] {
            notice_.clear();
            refresh();
        });
        connect(fitMiddle_, &QPushButton::clicked, this, [this] { fitMiddle(); });
    }
    rows_ = new QTableWidget(0, 2, this);
    rows_->setObjectName("assemblyRows");
    rows_->setHorizontalHeaderLabels(
        {QStringLiteral("Секция из наличия"), QStringLiteral("Зазор после, мм")});
    rows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rows_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rows_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(rows_, 1);
    auto *actions = new QHBoxLayout;
    const auto button = [&](const QString &title, const char *name) {
        auto *b = new QPushButton(title, this);
        b->setObjectName(name);
        actions->addWidget(b);
        return b;
    };
    add_ = button(QStringLiteral("Добавить"), "assemblyAdd");
    remove_ = button(QStringLiteral("Удалить"), "assemblyRemove");
    up_ = button(QStringLiteral("←"), "assemblyMoveLeft");
    down_ = button(QStringLiteral("→"), "assemblyMoveRight");
    up_->setAccessibleName(QStringLiteral("Переместить секцию влево"));
    down_->setAccessibleName(QStringLiteral("Переместить секцию вправо"));
    up_->setToolTip(up_->accessibleName());
    down_->setToolTip(down_->accessibleName());
    layout->addLayout(actions);
    status_ = new QLabel(this);
    status_->setObjectName("assemblyStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *hint = new QLabel(
        QStringLiteral(
            "Зазоры учитываются в геометрии. Влияние на качество гиба не рассчитывается. "
            "Проверка установки выполняется на этапе «Гибка»."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(this);
    apply_ = buttons->addButton(QStringLiteral("Применить"), QDialogButtonBox::AcceptRole);
    apply_->setObjectName("assemblyApply");
    buttons->addButton(QStringLiteral("Отмена"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    if (initial_) {
        draft_.pieces = initial_->pieces;
        for (std::size_t i = 1; i < draft_.pieces.size(); ++i)
            draft_.jointGapsMm.push_back(core::assemblyGapAfter(*initial_, i - 1));
    }
    connect(rows_, &QTableWidget::itemSelectionChanged, this, [this] { refresh(); });
    connect(add_, &QPushButton::clicked, this, [this] {
        if (!tool_.segments || draft_.pieces.size() >= 512)
            return;
        for (std::size_t i = 0; i < tool_.segments->size(); ++i) {
            const auto &s = (*tool_.segments)[i];
            if (!core::usableToolSegment(s) ||
                static_cast<unsigned>(std::count(draft_.pieces.begin(), draft_.pieces.end(), i)) >=
                    s.quantity)
                continue;
            if (!draft_.pieces.empty())
                draft_.jointGapsMm.push_back(0);
            draft_.pieces.push_back(i);
            rebuild(static_cast<int>(draft_.pieces.size() - 1));
            break;
        }
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const int row = rows_->currentRow();
        if (row < 0)
            return;
        draft_.pieces.erase(draft_.pieces.begin() + row);
        if (!draft_.jointGapsMm.empty())
            draft_.jointGapsMm.erase(draft_.jointGapsMm.begin() +
                                     std::min<std::size_t>(static_cast<std::size_t>(row),
                                                           draft_.jointGapsMm.size() - 1));
        rebuild(std::min(row, static_cast<int>(draft_.pieces.size()) - 1));
    });
    const auto move = [this](int delta) {
        const int row = rows_->currentRow(), to = row + delta;
        if (row < 0 || to < 0 || to >= static_cast<int>(draft_.pieces.size()))
            return;
        // Gaps belong to physical joints; swapping pieces does not move the window.
        std::swap(draft_.pieces[static_cast<std::size_t>(row)],
                  draft_.pieces[static_cast<std::size_t>(to)]);
        rebuild(to);
    };
    connect(up_, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down_, &QPushButton::clicked, this, [move] { move(1); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!tool_.segments || !core::assemblyLength(*tool_.segments, draft_))
            return;
        result_ = draft_;
        bool unchanged = initial_ && initial_->pieces == draft_.pieces;
        for (std::size_t i = 1; unchanged && i < draft_.pieces.size(); ++i)
            unchanged = core::assemblyGapAfter(*initial_, i - 1) == draft_.jointGapsMm[i - 1];
        if (unchanged)
            result_ = initial_;
        else if (!draft_.jointGapsMm.empty() &&
                 std::all_of(draft_.jointGapsMm.begin(), draft_.jointGapsMm.end(),
                             [&](double gap) { return gap == draft_.jointGapsMm.front(); })) {
            result_->gapMm = draft_.jointGapsMm.front();
            result_->jointGapsMm.clear();
        }
        accept();
    });
    rebuild(0);
}
void ToolAssemblyDialog::rebuild(int selected) {
    notice_.clear();
    rows_->setRowCount(0);
    if (!tool_.segments)
        return;
    for (std::size_t i = 0; i < draft_.pieces.size(); ++i) {
        rows_->insertRow(static_cast<int>(i));
        auto *section = new QComboBox(rows_);
        for (std::size_t row = 0; row < tool_.segments->size(); ++row) {
            const auto &s = (*tool_.segments)[row];
            if (core::usableToolSegment(s) && s.quantity) {
                QString label =
                    QStringLiteral("%1 мм · в наличии %2 шт.").arg(s.lengthMm).arg(s.quantity);
                if (s.horn)
                    label =
                        QStringLiteral("%1 мм · %2 · %3/R%4/%5 · %6 шт.")
                            .arg(s.lengthMm)
                            .arg(s.shape == core::SegmentShape::LeftHorn ? QStringLiteral("Левый")
                                                                         : QStringLiteral("Правый"))
                            .arg(s.horn->projectionMm)
                            .arg(s.horn->radiusMm)
                            .arg(s.horn->tipHeightMm)
                            .arg(s.quantity);
                section->addItem(label, static_cast<int>(row));
                section->setItemData(section->count() - 1, label, Qt::ToolTipRole);
            }
        }
        section->setCurrentIndex(section->findData(static_cast<int>(draft_.pieces[i])));
        rows_->setCellWidget(static_cast<int>(i), 0, section);
        auto *gap = new QDoubleSpinBox(rows_);
        gap->setDecimals(3);
        gap->setRange(0, 1000);
        gap->setValue(core::assemblyGapAfter(draft_, i));
        gap->setEnabled(i + 1 < draft_.pieces.size());
        gap->setAccessibleName(QStringLiteral("Зазор после секции %1, мм").arg(i + 1));
        rows_->setCellWidget(static_cast<int>(i), 1, gap);
        connect(section, &QComboBox::currentIndexChanged, this, [this, section, i] {
            if (section->currentIndex() >= 0) {
                draft_.pieces[i] = static_cast<std::size_t>(section->currentData().toInt());
                notice_.clear();
                refresh();
            }
        });
        connect(gap, &QDoubleSpinBox::valueChanged, this, [this, i](double value) {
            if (i < draft_.jointGapsMm.size()) {
                draft_.jointGapsMm[i] = value;
                notice_.clear();
                refresh();
            }
        });
    }
    if (selected >= 0 && selected < rows_->rowCount())
        rows_->selectRow(selected);
    refresh();
}
void ToolAssemblyDialog::refresh() {
    const auto length =
        tool_.segments ? core::assemblyLength(*tool_.segments, draft_) : std::nullopt;
    apply_->setEnabled(length.has_value());
    status_->setText(
        length
            ? QStringLiteral("%1 секц. · общая длина %2 мм").arg(draft_.pieces.size()).arg(*length)
            : QStringLiteral(
                  "Проверьте наличие и порядок: левый сапожок — слева, правый — справа."));
    if (length && target_ && target_->value() >= 1 && *length + 1e-5 < target_->value())
        status_->setText(status_->text() +
                         QStringLiteral(" · не хватает %1 мм").arg(target_->value() - *length));
    if (!notice_.isEmpty())
        status_->setText(status_->text() + QLatin1Char('\n') + notice_);
    add_->setEnabled(
        tool_.segments && draft_.pieces.size() < 512 &&
        std::any_of(tool_.segments->begin(), tool_.segments->end(), [this](const auto &s) {
            const auto row = static_cast<std::size_t>(&s - tool_.segments->data());
            return core::usableToolSegment(s) &&
                   static_cast<unsigned>(
                       std::count(draft_.pieces.begin(), draft_.pieces.end(), row)) < s.quantity;
        }));
    const int row = rows_->currentRow();
    remove_->setEnabled(row >= 0);
    up_->setEnabled(row > 0);
    down_->setEnabled(row >= 0 && row + 1 < rows_->rowCount());
    if (fitMiddle_)
        fitMiddle_->setEnabled(tool_.segments && target_->value() >= 1);
    if (ends_)
        ends_->setSource(tool_, draft_);
    diagram_->update();
}
void ToolAssemblyDialog::setEnd(std::size_t side, std::optional<std::size_t> row) {
    if (!tool_.segments || side > 1)
        return;
    const auto next = core::withPunchEnd(*tool_.segments, draft_, side != 0, row);
    if (!next) {
        notice_ = QStringLiteral("Проверьте наличие и порядок секций. Прежний состав сохранён.");
        refresh();
        return;
    }
    draft_ = *next;
    rebuild(side ? static_cast<int>(draft_.pieces.size()) - 1 : 0);
}
void ToolAssemblyDialog::selectPiece(std::size_t piece) {
    if (piece < static_cast<std::size_t>(rows_->rowCount()))
        rows_->selectRow(static_cast<int>(piece));
}
void ToolAssemblyDialog::fitMiddle() {
    if (!tool_.segments || !target_ || target_->value() < 1)
        return;
    const auto ends = draft_.pieces.empty() ? std::optional{core::PunchAssemblyEnds{}}
                                            : core::punchAssemblyEnds(*tool_.segments, draft_);
    if (!ends) {
        notice_ = QStringLiteral("Уточните порядок и наличие секций перед подбором середины.");
        refresh();
        return;
    }
    const auto result = core::planPunchAssemblies(*tool_.segments, target_->value(), 200000,
                                                  maximumGapMm_, {}, 1, ends);
    if (result.status != core::SegmentPlanStatus::Complete || result.plans.empty()) {
        notice_ = result.status == core::SegmentPlanStatus::SearchLimit
                      ? QStringLiteral("Достигнут предел подбора. Прежний состав сохранён.")
                      : QStringLiteral(
                            "Не хватает прямых секций для этой длины. Прежний состав сохранён.");
        refresh();
        return;
    }
    draft_ = result.best().assembly;
    for (std::size_t i = 1; i < draft_.pieces.size(); ++i)
        draft_.jointGapsMm.push_back(draft_.gapMm);
    draft_.gapMm = 0;
    rebuild(0);
}
} // namespace bending::ui
