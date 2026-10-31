#include "ToolStockDialog.h"
#include "ToolSegmentColorPreferences.h"
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <iterator>
namespace bending::ui {
namespace {
struct SectionSet {
    const char *id;
    QString title;
    std::vector<core::ToolSegment> sections; // Multiplicity in one reference set, not stock.
};
const std::vector<SectionSet> &sectionSets(core::ToolKind kind) {
    static const std::vector<SectionSet> punches{
        {"whole-835", QStringLiteral("Цельный инструмент · 835 мм"), {{835, 1}}},
        {"whole-415", QStringLiteral("Цельный инструмент · 415 мм"), {{415, 1}}},
        {"punch-805",
         QStringLiteral("Пуансон 805 мм · 9 секций с рожками"),
         {{100, 1, core::SegmentShape::LeftHorn},
          {270, 1},
          {10, 1},
          {15, 1},
          {20, 1},
          {40, 1},
          {50, 1},
          {200, 1},
          {100, 1, core::SegmentShape::RightHorn}}},
        {"left-horn-100",
         QStringLiteral("Левый рог · 100 мм"),
         {{100, 1, core::SegmentShape::LeftHorn}}},
        {"right-horn-100",
         QStringLiteral("Правый рог · 100 мм"),
         {{100, 1, core::SegmentShape::RightHorn}}}};
    static const std::vector<SectionSet> dies{
        {"whole-835", QStringLiteral("Цельный инструмент · 835 мм"), {{835, 1}}},
        {"whole-415", QStringLiteral("Цельный инструмент · 415 мм"), {{415, 1}}},
        {"die-805",
         QStringLiteral("Матрица 805 мм · 8 секций"),
         {{200, 1}, {10, 1}, {15, 1}, {20, 1}, {40, 1}, {50, 1}, {370, 1}, {100, 1}}}};
    return kind == core::ToolKind::Punch ? punches : dies;
}
std::vector<core::ToolSegment> referenceSections(core::ToolKind kind, int index) {
    const auto &sets = sectionSets(kind);
    if (index > 0 && static_cast<std::size_t>(index) <= sets.size())
        return sets[static_cast<std::size_t>(index - 1)].sections;
    // A plain 100 mm section may also be entered individually; it is not a horn.
    std::vector<core::ToolSegment> rows{{100, 0}};
    for (const auto &set : sets)
        for (const auto &section : set.sections)
            if (std::none_of(rows.begin(), rows.end(), [&](const auto &s) {
                    return s.lengthMm == section.lengthMm && s.shape == section.shape;
                }))
                rows.push_back({section.lengthMm, 0, section.shape});
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.shape != b.shape ? a.shape < b.shape : a.lengthMm > b.lengthMm;
    });
    return rows;
}
} // namespace
ToolStockDialog::ToolStockDialog(core::ToolCatalog catalog,
                                 std::function<QString(core::ToolPreset)> writer, QWidget *parent,
                                 bool embedded)
    : QDialog(parent), catalog_(std::move(catalog)), writer_(std::move(writer)),
      embedded_(embedded) {
    if (embedded_)
        setWindowFlags(Qt::Widget);
    setObjectName("toolStockDialog");
    setWindowTitle(QStringLiteral("Библиотека секций инструмента"));
    resize(720, 740);
    auto *layout = new QVBoxLayout(this);
    tool_ = new QComboBox(this);
    tool_->setObjectName("stockTool");
    for (const auto &t : catalog_)
        tool_->addItem((t.kind == core::ToolKind::Punch ? QStringLiteral("Пуансон · ")
                                                        : QStringLiteral("Матрица · ")) +
                       QString::fromStdString(t.code));
    layout->addWidget(tool_);
    tool_->setVisible(!embedded_);
    auto *help = new QLabel(
        QStringLiteral(
            "Укажите доступное количество секций одного профиля. "
            "Шаблон не подтверждает наличие: новые количества равны нулю. "
            "Рожковые секции хранятся отдельно и пока не участвуют в автоматической сборке."),
        this);
    help->setWordWrap(true);
    layout->addWidget(help);
    help->setVisible(!embedded_);
    auto *setRow = new QHBoxLayout;
    set_ = new QComboBox(this);
    set_->setObjectName("stockSetPreset");
    setRow->addWidget(set_, 1);
    copies_ = new QSpinBox(this);
    copies_->setObjectName("stockSetCount");
    copies_->setRange(0, 1000);
    copies_->setPrefix(QStringLiteral("Комплектов: "));
    copies_->setAccessibleName(QStringLiteral("Количество комплектов для добавления в наличие"));
    setRow->addWidget(copies_);
    addSet_ = new QPushButton(QStringLiteral("Добавить в наличие"), this);
    addSet_->setObjectName("stockAddSet");
    setRow->addWidget(addSet_);
    layout->addLayout(setRow);
    setDescription_ = new QLabel(this);
    setDescription_->setObjectName("stockSetDescription");
    setDescription_->setWordWrap(true);
    layout->addWidget(setDescription_);
    if (embedded_) {
        auto *setsToggle = new QPushButton(QStringLiteral("Добавить комплект секций…"), this);
        setsToggle->setObjectName("stockSetsToggle");
        setsToggle->setCheckable(true);
        layout->insertWidget(layout->count() - 2, setsToggle, 0, Qt::AlignLeft);
        for (auto *w : std::array<QWidget *, 4>{set_, copies_, addSet_, setDescription_})
            w->hide();
        connect(setsToggle, &QPushButton::toggled, this, [this](bool visible) {
            for (auto *w : std::array<QWidget *, 4>{set_, copies_, addSet_, setDescription_})
                w->setVisible(visible);
        });
    }
    enabled_ = new QCheckBox(QStringLiteral("Подбирать из указанного наличия секций"), this);
    enabled_->setObjectName("stockEnabled");
    if (embedded_)
        enabled_->setText(QStringLiteral("Указать наличие секций"));
    layout->addWidget(enabled_);
    rows_ = new QTableWidget(0, 4, this);
    rows_->setObjectName("stockRows");
    rows_->setHorizontalHeaderLabels({QStringLiteral("Длина, мм"),
                                      QStringLiteral("Количество, шт."), QStringLiteral("Форма"),
                                      QStringLiteral("Цвет")});
    rows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rows_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    rows_->setColumnWidth(3, 64);
    rows_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(rows_);
    hornNotice_ = new QLabel(QStringLiteral("Сапожки пока доступны только для учёта наличия. Их "
                                            "точная форма и симуляция — следующий этап."),
                             this);
    hornNotice_->setObjectName("stockHornNotice");
    hornNotice_->setWordWrap(true);
    hornNotice_->hide();
    layout->addWidget(hornNotice_);
    auto *colorHelp = new QLabel(
        QStringLiteral("Цвета общие для одной длины у всех инструментов и сохраняются сразу. "
                       "Нажмите цвет для выбора; правой кнопкой — возврат к стандартному."),
        this);
    colorHelp->setWordWrap(true);
    layout->addWidget(colorHelp);
    colorHelp->setVisible(!embedded_);
    auto *actions = new QHBoxLayout;
    auto *addRow = new QPushButton(QStringLiteral("Добавить"), this);
    addRow->setObjectName("stockAdd");
    if (embedded_)
        addRow->setText(QStringLiteral("Добавить секцию"));
    auto *remove = new QPushButton(QStringLiteral("Удалить строку"), this);
    auto *standard = new QPushButton(QStringLiteral("Добавить размеры без наличия"), this);
    standard->setObjectName("stockTemplate");
    if (embedded_)
        standard->setText(QStringLiteral("Типовые длины"));
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
    buttons->button(QDialogButtonBox::Close)->setVisible(!embedded_);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this,
            [this] { save(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(tool_, &QComboBox::currentIndexChanged, this, [this] { load(); });
    connect(enabled_, &QCheckBox::toggled, this, [this] {
        if (!loading_)
            inventoryEdited_ = true;
        updateEditing();
    });
    connect(addRow, &QPushButton::clicked, this, [this] {
        enabled_->setChecked(true);
        add({100, 0});
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (rows_->currentRow() >= 0) {
            rows_->removeRow(rows_->currentRow());
            inventoryEdited_ = true;
            updateHornNotice();
        }
    });
    connect(standard, &QPushButton::clicked, this, [this] { addSet(false); });
    connect(set_, &QComboBox::currentIndexChanged, this, [this] { updateSet(); });
    connect(copies_, &QSpinBox::valueChanged, this, [this] { updateSet(); });
    connect(addSet_, &QPushButton::clicked, this, [this] { addSet(true); });
    if (!catalog_.empty())
        load();
}
core::ToolKind ToolStockDialog::toolKind() const {
    const auto index = static_cast<std::size_t>(tool_->currentIndex());
    return index < catalog_.size() ? catalog_[index].kind : core::ToolKind::Punch;
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
    quantity->setAccessibleName(QStringLiteral("Количество секций %1 мм").arg(s.lengthMm));
    if (embedded_)
        quantity->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    auto *shape = new QComboBox(rows_);
    shape->addItems({QStringLiteral("Прямая"), QStringLiteral("Левый сапожок"),
                     QStringLiteral("Правый сапожок")});
    shape->setToolTip(QStringLiteral(
        "Сапожки учитываются отдельно. Их точная форма и расчёт пока не поддержаны."));
    shape->setCurrentIndex(static_cast<int>(s.shape));
    rows_->setCellWidget(row, 0, length);
    rows_->setCellWidget(row, 1, quantity);
    rows_->setCellWidget(row, 2, shape);
    auto *color = new QPushButton(rows_);
    color->setObjectName("stockColor");
    color->setContextMenuPolicy(Qt::CustomContextMenu);
    rows_->setCellWidget(row, 3, color);
    connect(color, &QPushButton::clicked, this, [this, length] { chooseColor(length->value()); });
    connect(color, &QWidget::customContextMenuRequested, this, [this, color, length](QPoint p) {
        QMenu menu(color);
        auto *reset = menu.addAction(QStringLiteral("Стандартный цвет для этой длины"));
        reset->setObjectName("resetSegmentColor");
        connect(reset, &QAction::triggered, this, [this, length] { resetColor(length->value()); });
        menu.exec(color->mapToGlobal(p));
    });
    connect(length, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!loading_)
            inventoryEdited_ = true;
        updateColors();
    });
    connect(quantity, &QSpinBox::valueChanged, this, [this] {
        if (!loading_)
            inventoryEdited_ = true;
        updateHornNotice();
    });
    connect(shape, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_)
            inventoryEdited_ = true;
        updateHornNotice();
    });
    if (!loading_) {
        inventoryEdited_ = true;
        updateEditing();
        updateColors();
    }
}
void ToolStockDialog::load() {
    const auto index = static_cast<std::size_t>(tool_->currentIndex());
    if (index >= catalog_.size())
        return;
    loading_ = true;
    const auto &s = catalog_[index].segments;
    {
        const QSignalBlocker blocked(set_);
        const auto previous = set_->currentData();
        set_->clear();
        set_->addItem(QStringLiteral("Все типовые размеры"), "all");
        for (const auto &set : sectionSets(toolKind()))
            set_->addItem(set.title, QLatin1String(set.id));
        set_->setCurrentIndex(std::max(0, set_->findData(previous)));
    }
    copies_->setValue(0);
    updateSet();
    rows_->setRowCount(0);
    enabled_->setChecked(s.has_value());
    if (s)
        for (const auto &r : *s)
            add(r);
    auto references = referenceSections(toolKind(), 0);
    const double nominalLength = catalog_[index].lengthMm;
    if (nominalLength > 0 &&
        std::none_of(references.begin(), references.end(), [nominalLength](const auto &r) {
            return r.lengthMm == nominalLength && r.shape == core::SegmentShape::Straight;
        }))
        references.insert(references.begin(), {nominalLength, 0});
    if (embedded_ && !s)
        references = nominalLength > 0 ? std::vector<core::ToolSegment>{{nominalLength, 0}}
                                       : std::vector<core::ToolSegment>{};
    if (embedded_ && s)
        references.clear();
    for (const auto &r : references) {
        bool exists = false;
        for (int i = 0; i < rows_->rowCount(); ++i)
            exists |=
                static_cast<QDoubleSpinBox *>(rows_->cellWidget(i, 0))->value() == r.lengthMm &&
                static_cast<QComboBox *>(rows_->cellWidget(i, 2))->currentIndex() ==
                    static_cast<int>(r.shape);
        if (!exists)
            add(r);
    }
    status_->setText(
        embedded_
            ? (s ? QStringLiteral(
                       "Сохраняется кнопкой ниже или при переходе к другому профилю/закрытии.")
                 : QStringLiteral("Наличие не указано. Включите учёт, чтобы заполнить количество."))
        : s ? QStringLiteral("Изменения применяются к библиотеке после сохранения.")
            : QStringLiteral("Наличие ещё не указано; сохраняется номинальная цельная длина."));
    loading_ = false;
    inventoryEdited_ = false;
    updateEditing();
    updateColors();
}
void ToolStockDialog::updateEditing() {
    updateHornNotice();
    for (int row = 0; row < rows_->rowCount(); ++row)
        for (int column = 0; column < 3; ++column)
            rows_->cellWidget(row, column)->setEnabled(enabled_->isChecked());
    for (int row = 0; row < rows_->rowCount(); ++row)
        static_cast<QSpinBox *>(rows_->cellWidget(row, 1))
            ->setSpecialValueText(enabled_->isChecked() ? QString{} : QStringLiteral("—"));
}
void ToolStockDialog::updateHornNotice() {
    bool horns = false;
    for (int row = 0; row < rows_->rowCount(); ++row)
        if (auto *shape = qobject_cast<QComboBox *>(rows_->cellWidget(row, 2)))
            horns |= shape->currentIndex() > 0;
    hornNotice_->setVisible(embedded_ && horns);
}
void ToolStockDialog::selectTool(const std::string &id) {
    for (std::size_t i = 0; i < catalog_.size(); ++i)
        if (catalog_[i].id == id) {
            tool_->setCurrentIndex(static_cast<int>(i));
            return;
        }
}
void ToolStockDialog::updateColors() {
    const auto colors = toolSegmentColors();
    for (int row = 0; row < rows_->rowCount(); ++row) {
        const double length = static_cast<QDoubleSpinBox *>(rows_->cellWidget(row, 0))->value();
        const auto color = viewer::toolSegmentColor(length, colors);
        auto *button = static_cast<QPushButton *>(rows_->cellWidget(row, 3));
        button->setStyleSheet(
            QStringLiteral("background:%1;border:1px solid #596878;border-radius:3px;")
                .arg(color.name()));
        button->setProperty("segmentColor", color.name());
        button->setAccessibleName(
            QStringLiteral("Цвет секций длиной %1 мм: %2").arg(length).arg(color.name()));
        button->setToolTip(button->accessibleName() +
                           QStringLiteral(". Нажмите для выбора; ПКМ — стандартный цвет."));
    }
}
void ToolStockDialog::chooseColor(double length) {
    QColorDialog dialog(viewer::toolSegmentColor(length, toolSegmentColors()), this);
    dialog.setObjectName("toolSegmentColorDialog");
    dialog.setWindowTitle(QStringLiteral("Цвет секций длиной %1 мм").arg(length));
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (!setToolSegmentColor(length, dialog.currentColor())) {
        status_->setText(QStringLiteral("Не удалось сохранить цвет в настройках."));
        return;
    }
    updateColors();
    status_->setText(
        QStringLiteral("Цвет сохранён для этой длины у всех инструментов. Наличие не изменено."));
    if (onColorsChanged)
        onColorsChanged();
}
void ToolStockDialog::resetColor(double length) {
    if (!resetToolSegmentColor(length)) {
        status_->setText(QStringLiteral("Не удалось сбросить цвет в настройках."));
        return;
    }
    updateColors();
    status_->setText(QStringLiteral("Восстановлен стандартный цвет для этой длины."));
    if (onColorsChanged)
        onColorsChanged();
}
void ToolStockDialog::updateSet() {
    const bool specific = set_->currentIndex() > 0;
    copies_->setEnabled(specific);
    addSet_->setEnabled(specific && copies_->value() > 0);
    if (!specific) {
        setDescription_->setText(
            (toolKind() == core::ToolKind::Punch
                 ? QStringLiteral("Цельные 835 / 415 мм; набор 805 мм — 7 обычных секций "
                                  "и два рожка по 100 мм. ")
                 : QStringLiteral("Цельные 835 / 415 мм; набор 805 мм — 8 обычных секций, "
                                  "в том числе 370 мм. ")) +
            QStringLiteral("Это редактируемый типовой состав; проверьте его для вашего профиля. "
                           "Наличие вводится отдельно."));
        return;
    }
    const auto parts = referenceSections(toolKind(), set_->currentIndex());
    double length = 0;
    unsigned count = 0;
    QStringList values;
    for (const auto &p : parts) {
        length += p.lengthMm * p.quantity;
        count += p.quantity;
        auto value = p.quantity > 1 ? QStringLiteral("%1 × %2").arg(p.lengthMm).arg(p.quantity)
                                    : QString::number(p.lengthMm);
        if (p.shape == core::SegmentShape::LeftHorn)
            value += QStringLiteral(" (левый рог)");
        else if (p.shape == core::SegmentShape::RightHorn)
            value += QStringLiteral(" (правый рог)");
        values << value;
    }
    setDescription_->setText(QStringLiteral("В одном комплекте: %1 = %2 мм · секций: %3. "
                                            "При добавлении количества суммируются с уже "
                                            "указанными; сохраняются кнопкой «Сохранить».")
                                 .arg(values.join(QStringLiteral(" + ")))
                                 .arg(length)
                                 .arg(count));
}
void ToolStockDialog::addSet(bool available) {
    if (available && (set_->currentIndex() <= 0 || copies_->value() <= 0))
        return;
    std::vector<core::ToolSegment> merged;
    for (int i = 0; i < rows_->rowCount(); ++i)
        merged.push_back(
            {static_cast<QDoubleSpinBox *>(rows_->cellWidget(i, 0))->value(),
             static_cast<unsigned>(static_cast<QSpinBox *>(rows_->cellWidget(i, 1))->value()),
             static_cast<core::SegmentShape>(
                 static_cast<QComboBox *>(rows_->cellWidget(i, 2))->currentIndex())});
    for (const auto &s : referenceSections(toolKind(), set_->currentIndex())) {
        auto found = std::find_if(merged.begin(), merged.end(), [&](const auto &r) {
            return r.lengthMm == s.lengthMm && r.shape == s.shape;
        });
        if (found == merged.end()) {
            merged.push_back({s.lengthMm, 0, s.shape});
            found = std::prev(merged.end());
        }
        if (available)
            found->quantity += s.quantity * static_cast<unsigned>(copies_->value());
    }
    if (!core::validToolSegments(merged)) {
        status_->setText(QStringLiteral(
            "Комплект не добавлен: проверьте повторяющиеся строки, "
            "предел 64 размеров и количество не более 1000 шт. в строке. Данные не изменены."));
        return;
    }
    rows_->setRowCount(0);
    for (const auto &s : merged)
        add(s);
    enabled_->setChecked(true);
    status_->setText(
        available
            ? QStringLiteral("Комплект добавлен в черновик наличия. Сохраните изменения.")
            : QStringLiteral(
                  "Размеры добавлены с нулевым новым наличием; введённые количества сохранены."));
}
void ToolStockDialog::save() {
    if (!inventoryEdited_) {
        status_->setText(
            QStringLiteral("Наличие сохранено без изменений. Цвета сохраняются сразу."));
        return;
    }
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
    inventoryEdited_ = false;
    status_->setText(
        QStringLiteral("Наличие сохранено. Существующие проекты сохраняют свои снимки сборок."));
}
} // namespace bending::ui
