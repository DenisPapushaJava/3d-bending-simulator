#include "ToolStockDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace bending::ui {
ToolStockDialog::ToolStockDialog(core::ToolCatalog catalog,
                                 std::function<QString(core::ToolPreset)> writer, QWidget *parent)
    : QDialog(parent), catalog_(std::move(catalog)), writer_(std::move(writer)) {
    setObjectName("toolStockDialog");
    setWindowTitle(QStringLiteral("Библиотека секций инструмента"));
    resize(620, 580);
    auto *layout = new QVBoxLayout(this);
    tool_ = new QComboBox(this);
    tool_->setObjectName("stockTool");
    for (const auto &t : catalog_)
        tool_->addItem((t.kind == core::ToolKind::Punch ? QStringLiteral("Пуансон · ")
                                                        : QStringLiteral("Матрица · ")) +
                       QString::fromStdString(t.code));
    layout->addWidget(tool_);
    auto *help = new QLabel(
        QStringLiteral(
            "Укажите доступное количество секций одного профиля. "
            "Шаблон не подтверждает наличие: новые количества равны нулю. "
            "Рожковые секции хранятся отдельно и пока не участвуют в автоматической сборке."),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);
    enabled_ = new QCheckBox(QStringLiteral("Подбирать из указанного наличия секций"), this);
    enabled_->setObjectName("stockEnabled");
    layout->addWidget(enabled_);
    rows_ = new QTableWidget(0, 3, this);
    rows_->setObjectName("stockRows");
    rows_->setHorizontalHeaderLabels(
        {QStringLiteral("Длина, мм"), QStringLiteral("Количество, шт."), QStringLiteral("Форма")});
    rows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rows_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(rows_);
    auto *actions = new QHBoxLayout;
    auto *addRow = new QPushButton(QStringLiteral("Добавить"), this);
    addRow->setObjectName("stockAdd");
    auto *remove = new QPushButton(QStringLiteral("Удалить строку"), this);
    auto *standard = new QPushButton(QStringLiteral("Типовой шаблон…"), this);
    standard->setObjectName("stockTemplate");
    actions->addWidget(addRow);
    actions->addWidget(remove);
    actions->addWidget(standard);
    layout->addLayout(actions);
    status_ = new QLabel(this);
    status_->setObjectName("stockStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Save)->setObjectName("stockSave");
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить"));
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this,
            [this] { save(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(tool_, &QComboBox::currentIndexChanged, this, [this] { load(); });
    connect(enabled_, &QCheckBox::toggled, rows_, &QWidget::setEnabled);
    connect(addRow, &QPushButton::clicked, this, [this] {
        enabled_->setChecked(true);
        add({100, 0});
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (rows_->currentRow() >= 0)
            rows_->removeRow(rows_->currentRow());
    });
    connect(standard, &QPushButton::clicked, this, [this] {
        // Append missing template lengths; never discard entered quantities.
        enabled_->setChecked(true);
        for (const double length : {835., 415., 270., 200., 100., 50., 40., 20., 15., 10.}) {
            bool exists = false;
            for (int i = 0; i < rows_->rowCount(); ++i)
                exists |=
                    static_cast<QDoubleSpinBox *>(rows_->cellWidget(i, 0))->value() == length &&
                    static_cast<QComboBox *>(rows_->cellWidget(i, 2))->currentIndex() == 0;
            if (!exists)
                add({length, 0});
        }
        status_->setText(
            QStringLiteral("Шаблон добавлен. Заполните фактическое количество и сохраните."));
    });
    if (!catalog_.empty())
        load();
}
void ToolStockDialog::add(core::ToolSegment s) {
    if (rows_->rowCount() >= 64)
        return;
    const int row = rows_->rowCount();
    rows_->insertRow(row);
    auto *length = new QDoubleSpinBox(rows_);
    length->setRange(1, 100000);
    length->setDecimals(3);
    length->setValue(s.lengthMm);
    auto *quantity = new QSpinBox(rows_);
    quantity->setRange(0, 1000);
    quantity->setValue(static_cast<int>(s.quantity));
    auto *shape = new QComboBox(rows_);
    shape->addItems(
        {QStringLiteral("Обычная"), QStringLiteral("Левый рог"), QStringLiteral("Правый рог")});
    shape->setCurrentIndex(static_cast<int>(s.shape));
    rows_->setCellWidget(row, 0, length);
    rows_->setCellWidget(row, 1, quantity);
    rows_->setCellWidget(row, 2, shape);
}
void ToolStockDialog::load() {
    const auto index = static_cast<std::size_t>(tool_->currentIndex());
    if (index >= catalog_.size())
        return;
    const auto &s = catalog_[index].segments;
    rows_->setRowCount(0);
    enabled_->setChecked(s.has_value());
    rows_->setEnabled(s.has_value());
    if (s)
        for (const auto &r : *s)
            add(r);
    status_->setText(
        s ? QStringLiteral("Изменения применяются к библиотеке после сохранения.")
          : QStringLiteral(
                "Наличие секций ещё не учтено. Сохраняется прежний выбор цельного инструмента."));
}
void ToolStockDialog::save() {
    const auto index = static_cast<std::size_t>(tool_->currentIndex());
    if (index >= catalog_.size())
        return;
    auto tool = catalog_[index];
    tool.segments.reset();
    if (enabled_->isChecked()) {
        tool.segments.emplace();
        for (int i = 0; i < rows_->rowCount(); ++i)
            tool.segments->push_back(
                {static_cast<QDoubleSpinBox *>(rows_->cellWidget(i, 0))->value(),
                 static_cast<unsigned>(static_cast<QSpinBox *>(rows_->cellWidget(i, 1))->value()),
                 static_cast<core::SegmentShape>(
                     static_cast<QComboBox *>(rows_->cellWidget(i, 2))->currentIndex())});
    }
    if (!core::validToolPreset(tool)) {
        status_->setText(QStringLiteral("Проверьте длины и повторяющиеся строки одной формы."));
        return;
    }
    if (const auto error = writer_(tool); !error.isEmpty()) {
        status_->setText(error);
        return;
    }
    catalog_[index] = std::move(tool);
    status_->setText(
        QStringLiteral("Наличие сохранено. Существующие проекты сохраняют свои снимки сборок."));
}
} // namespace bending::ui
