#include "ToolAssemblyDialog.h"
#include "ToolSegmentColorPreferences.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace bending::ui {
namespace {
class AssemblyDiagram final : public QWidget {
  public:
    std::function<std::optional<std::vector<core::ToolSectionInterval>>()> intervals;
    explicit AssemblyDiagram(QWidget *parent) : QWidget(parent) {
        setMinimumHeight(84);
        setAccessibleName(QStringLiteral("Расположение секций вдоль инструмента"));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto sections = intervals ? intervals() : std::nullopt;
        if (!sections || sections->empty())
            return;
        const double scale = (width() - 24.) / sections->back().toMm;
        const double y = 16;
        const auto colors = toolSegmentColors();
        for (std::size_t i = 0; i < sections->size(); ++i) {
            const auto &s = (*sections)[i];
            const double x = 12 + s.fromMm * scale;
            const double w = (s.toMm - s.fromMm) * scale;
            p.setPen(QColor("#507781"));
            p.setBrush(viewer::toolSegmentColor(s.toMm - s.fromMm, colors));
            p.drawRect(QRectF(x, y, w, 30));
            p.setPen(palette().color(QPalette::Text));
            if (w >= 30)
                p.drawText(QRectF(x, y, w, 30), Qt::AlignCenter, QString::number(i + 1));
            if (i + 1 < sections->size() && (*sections)[i + 1].fromMm > s.toMm) {
                const double right = 12 + (*sections)[i + 1].fromMm * scale;
                p.setPen(QPen(QColor("#956a38"), 1, Qt::DashLine));
                p.drawLine(QPointF(x + w, y + 38), QPointF(right, y + 38));
            }
        }
    }
};
} // namespace
ToolAssemblyDialog::ToolAssemblyDialog(core::ToolPreset tool,
                                       std::optional<core::ToolAssembly> initial, QWidget *parent)
    : QDialog(parent), tool_(std::move(tool)), initial_(std::move(initial)) {
    setObjectName("toolAssemblyDialog");
    setWindowTitle((tool_.kind == core::ToolKind::Punch ? QStringLiteral("Секции пуансона · ")
                                                        : QStringLiteral("Секции матрицы · ")) +
                   QString::fromStdString(tool_.code));
    setWindowModality(Qt::WindowModal);
    resize(620, 520);
    auto *layout = new QVBoxLayout(this);
    auto *caption = new QLabel(tool_.kind == core::ToolKind::Punch
                                   ? QStringLiteral("Секции вдоль инструмента · обычная ориентация")
                                   : QStringLiteral("Секции вдоль инструмента"),
                               this);
    layout->addWidget(caption);
    auto *diagram = new AssemblyDiagram(this);
    diagram_ = diagram;
    diagram->intervals = [this] {
        return tool_.segments ? core::assemblyIntervals(*tool_.segments, draft_) : std::nullopt;
    };
    layout->addWidget(diagram);
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
                refresh();
            }
        });
        connect(gap, &QDoubleSpinBox::valueChanged, this, [this, i](double value) {
            if (i < draft_.jointGapsMm.size()) {
                draft_.jointGapsMm[i] = value;
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
    diagram_->update();
}
} // namespace bending::ui
