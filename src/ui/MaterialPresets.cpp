#include "MaterialPresets.h"
#include "bending/ui/Preferences.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <array>

namespace bending::ui {
namespace {
using core::SheetMaterial;
constexpr auto settingsKey = "materials/presets";
constexpr int builtInCount = 3;
constexpr double defaultK = .5;
constexpr int maxTensileStrengthMpa = 5000;
const std::vector<MaterialPreset> &builtIns() {
    static const std::vector<MaterialPreset> presets{
        {QStringLiteral("Низкоуглеродистая сталь"),
         SheetMaterial::MildSteel,
         defaultK,
         {QStringLiteral("Ст-3"), QStringLiteral("09Г2С")},
         400},
        {QStringLiteral("Нержавеющая сталь"),
         SheetMaterial::StainlessSteel,
         defaultK,
         {QStringLiteral("AISI-304"), QStringLiteral("AISI-430")},
         600},
        {QStringLiteral("Алюминий"), SheetMaterial::Aluminium, defaultK, {}, 100}};
    return presets;
}
void showReferenceTable(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Справочная таблица K"));
    dialog.resize(680, 270);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint =
        new QLabel(QStringLiteral("Воздушная гибка. Ri — внутренний радиус, t — толщина листа."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *table = new QTableWidget(builtInCount, 3);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Ri/t ≤ 1"), QStringLiteral("1 < Ri/t < 3"), QStringLiteral("Ri/t ≥ 3")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    constexpr std::array ratios{1., 2., 3.};
    QStringList names;
    for (int row = 0; row < builtInCount; ++row) {
        const auto &preset = builtIns()[static_cast<std::size_t>(row)];
        names.append(preset.name);
        const core::UnfoldSettings reference{preset.material, {}};
        for (int col = 0; col < table->columnCount(); ++col) {
            const auto k = core::unfoldK(reference, ratios[static_cast<std::size_t>(col)], 1.);
            auto *cell = new QTableWidgetItem(k ? QLocale().toString(*k, 'f', 2) : QString{});
            cell->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, col, cell);
        }
    }
    table->setVerticalHeaderLabels(names);
    layout->addWidget(table);
    auto *close = new QPushButton(QStringLiteral("Закрыть"));
    layout->addWidget(close, 0, Qt::AlignRight);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

void editGrades(QPushButton *button) {
    QDialog dialog(button);
    dialog.setWindowTitle(QStringLiteral("Марки для чертежа"));
    dialog.resize(450, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral("Одна марка на строку."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *editor = new QPlainTextEdit;
    editor->setObjectName(QStringLiteral("materialGradeList"));
    editor->setPlainText(button->property("grades").toStringList().join(QLatin1Char('\n')));
    layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Готово"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QStringList grades;
        for (const auto &line : editor->toPlainText().split(QLatin1Char('\n'))) {
            const auto grade = line.trimmed();
            if (grade.size() > 60) {
                hint->setText(
                    QStringLiteral("Обозначение марки должно быть не длиннее 60 знаков."));
                return;
            }
            if (!grade.isEmpty() && !grades.contains(grade, Qt::CaseInsensitive))
                grades.append(grade);
        }
        button->setProperty("grades", grades);
        button->setText(QStringLiteral("Изменить (%1)…").arg(grades.size()));
        dialog.accept();
    });
    editor->setFocus();
    dialog.exec();
}
} // namespace

std::vector<MaterialPreset> materialPresets() {
    auto result = builtIns();
    const auto rows = preferences()->value(settingsKey).toList();
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const auto row = rows[i].toMap();
        const auto grades = row.value("grades").toStringList();
        bool strengthOk = false;
        const auto strength = row.value("tensileStrengthMpa").toInt(&strengthOk);
        std::optional<int> tensileStrength;
        if (strengthOk && strength > 0 && strength <= maxTensileStrengthMpa)
            tensileStrength = strength;
        if (i < builtInCount) {
            auto &preset = result[static_cast<std::size_t>(i)];
            const auto name = row.value("name").toString().trimmed();
            if (!name.isEmpty() && name.size() <= 60)
                preset.name = name;
            preset.grades = grades;
            if (row.contains("tensileStrengthMpa"))
                preset.tensileStrengthMpa = tensileStrength;
            if (!row.contains("k"))
                preset.k.reset(); // A saved row without K uses the reference rule.
            bool ok = false;
            const auto k = row.value("k").toDouble(&ok);
            if (ok && core::validUnfoldSettings({preset.material, k}))
                preset.k = k;
            continue;
        }
        const auto name = row.value("name").toString();
        bool ok = false;
        const auto k = row.value("k").toDouble(&ok);
        if (ok && !name.isEmpty() &&
            core::validUnfoldSettings({SheetMaterial::Unknown, k, 1, name.toStdString()}))
            result.push_back({name, SheetMaterial::Unknown, k, grades, tensileStrength});
    }
    return result;
}

QString materialName(const core::UnfoldSettings &settings) {
    if (!settings.customMaterialName.empty())
        return QString::fromStdString(settings.customMaterialName);
    if (settings.material == SheetMaterial::Unknown)
        return {};
    for (const auto &preset : materialPresets())
        if (preset.material == settings.material)
            return preset.name;
    return {};
}

void editMaterialPresets(QWidget *parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Материалы"));
    dialog.resize(1020, 390);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint =
        new QLabel(QStringLiteral("Дважды щёлкните по названию, чтобы изменить его. "
                                  "Стартовые значения Rm ориентировочные. "
                                  "Укажите предел прочности из паспорта листа, если он известен."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *reference = new QPushButton(QStringLiteral("Справочная таблица K…"));
    layout->addWidget(reference, 0, Qt::AlignLeft);
    QObject::connect(reference, &QPushButton::clicked, &dialog,
                     [&dialog] { showReferenceTable(&dialog); });
    auto *table = new QTableWidget(0, 4);
    table->setObjectName(QStringLiteral("materialPresets"));
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Категория"), QStringLiteral("Правило развёртки"),
         QStringLiteral("Марки для чертежа"), QStringLiteral("Rm, МПа")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(table);
    const auto addRow = [table](const QString &name, std::optional<double> k,
                                const QStringList &grades, bool fixed,
                                std::optional<int> tensileStrength) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(name));
        auto *rule = new QWidget(table);
        auto *ruleLayout = new QHBoxLayout(rule);
        ruleLayout->setContentsMargins(2, 0, 2, 0);
        auto *mode = new QComboBox(rule);
        mode->setObjectName(QStringLiteral("materialUnfoldMode"));
        if (fixed)
            mode->addItem(QStringLiteral("По справочной таблице"), false);
        mode->addItem(QStringLiteral("Постоянный K"), true);
        mode->setCurrentIndex(mode->findData(k.has_value() || !fixed));
        auto *factor = new QDoubleSpinBox(rule);
        factor->setObjectName(QStringLiteral("materialUnfoldK"));
        factor->setAccessibleName(QStringLiteral("Коэффициент K"));
        factor->setRange(0., 1.);
        factor->setDecimals(4);
        factor->setSingleStep(.01);
        factor->setValue(k.value_or(defaultK));
        factor->setVisible(mode->currentData().toBool());
        QObject::connect(mode, &QComboBox::currentIndexChanged, factor,
                         [mode, factor] { factor->setVisible(mode->currentData().toBool()); });
        ruleLayout->addWidget(mode, 1);
        ruleLayout->addWidget(factor);
        table->setCellWidget(row, 1, rule);
        auto *edit = new QPushButton(QStringLiteral("Изменить (%1)…").arg(grades.size()));
        edit->setObjectName(QStringLiteral("editMaterialGrades"));
        edit->setProperty("grades", grades);
        table->setCellWidget(row, 2, edit);
        QObject::connect(edit, &QPushButton::clicked, edit, [edit] { editGrades(edit); });
        auto *strength = new QSpinBox(table);
        strength->setAccessibleName(QStringLiteral("Предел прочности Rm, МПа"));
        strength->setToolTip(QStringLiteral("Предел прочности при растяжении, МПа. "
                                            "Введите 0, чтобы оставить значение незаданным."));
        strength->setRange(0, maxTensileStrengthMpa);
        strength->setSpecialValueText(QStringLiteral("Не задан"));
        strength->setValue(tensileStrength.value_or(0));
        table->setCellWidget(row, 3, strength);
    };
    for (const auto &preset : materialPresets())
        addRow(preset.name, preset.k, preset.grades, preset.material != SheetMaterial::Unknown,
               preset.tensileStrengthMpa);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    auto *add =
        buttons->addButton(QStringLiteral("Добавить категорию"), QDialogButtonBox::ActionRole);
    auto *remove = buttons->addButton(QStringLiteral("Удалить"), QDialogButtonBox::ActionRole);
    remove->setEnabled(false);
    layout->addWidget(buttons);
    QObject::connect(table, &QTableWidget::currentCellChanged, &dialog,
                     [remove](int row) { remove->setEnabled(row >= builtInCount); });
    QObject::connect(add, &QPushButton::clicked, &dialog, [&] {
        addRow({}, defaultK, {}, false, {});
        table->setCurrentCell(table->rowCount() - 1, 0);
        table->editItem(table->currentItem());
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [table] {
        if (table->currentRow() >= builtInCount)
            table->removeRow(table->currentRow());
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QVariantList rows;
        QStringList names;
        for (int row = 0; row < table->rowCount(); ++row) {
            const auto name = table->item(row, 0)->text().trimmed();
            const auto grades = table->cellWidget(row, 2)->property("grades").toStringList();
            if (name.isEmpty() || name.size() > 60 || names.contains(name, Qt::CaseInsensitive)) {
                hint->setText(QStringLiteral("Строка %1. Укажите уникальное название до 60 знаков.")
                                  .arg(row + 1));
                return;
            }
            const auto *strength = qobject_cast<QSpinBox *>(table->cellWidget(row, 3));
            QVariantMap data{
                {"name", name}, {"grades", grades}, {"tensileStrengthMpa", strength->value()}};
            const auto *rule = table->cellWidget(row, 1);
            const auto *mode = rule->findChild<QComboBox *>("materialUnfoldMode");
            const auto *factor = rule->findChild<QDoubleSpinBox *>("materialUnfoldK");
            if (mode->currentData().toBool())
                data.insert("k", factor->value());
            names.append(name);
            rows.append(data);
        }
        auto settings = preferences();
        settings->setValue(settingsKey, rows);
        settings->sync();
        if (settings->status() == QSettings::NoError)
            dialog.accept();
        else
            hint->setText(QStringLiteral("Не удалось сохранить список материалов."));
    });
    dialog.exec();
}
} // namespace bending::ui
