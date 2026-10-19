#include "ToolSelectionWidget.h"
#include "ToolProfilesWidget.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <numbers>

namespace bending::ui {
namespace {
QString text(const std::string &s) {
    return QString::fromStdString(s);
}
QString title(const core::ToolPreset &t) {
    return text(t.manufacturer + " · " + t.code) +
           (t.teaching ? QStringLiteral(" · учебный") : QString{});
}
QLabel *wrapped(QWidget *parent) {
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace
ToolSelectionWidget::ToolSelectionWidget(core::ToolCatalog catalog, QWidget *parent)
    : QWidget(parent), catalog_(std::move(catalog)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignTop);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("toolSourceMode"));
    mode_->setAccessibleName(QStringLiteral("Источник параметров инструмента"));
    mode_->addItems({QStringLiteral("Каталог инструмента"), QStringLiteral("Ручные параметры")});
    layout->addWidget(mode_);
    catalogPanel_ = new QWidget(this);
    auto *catalogLayout = new QVBoxLayout(catalogPanel_);
    catalogLayout->setContentsMargins(0, 0, 0, 0);
    auto *form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto combo = [&](const char *name, const QString &caption) {
        auto *c = new QComboBox(catalogPanel_);
        c->setObjectName(QLatin1String(name));
        c->setAccessibleName(caption);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(16);
        form->addRow(caption, c);
        return c;
    };
    punch_ = combo("punchPreset", QStringLiteral("Пуансон:"));
    die_ = combo("diePreset", QStringLiteral("Матрица:"));
    groove_ = combo("dieGroove", QStringLiteral("Рабочий ручей:"));
    punch_->addItem(QStringLiteral("Выберите пуансон…"), -1);
    die_->addItem(QStringLiteral("Выберите матрицу…"), -1);
    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        auto *c = catalog_[i].kind == core::ToolKind::Punch ? punch_ : die_;
        c->addItem(title(catalog_[i]), static_cast<int>(i));
    }
    catalogLayout->addLayout(form);
    catalogDetails_ = new QWidget(this);
    auto *detailsLayout = new QVBoxLayout(catalogDetails_);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    favorites_ = new QComboBox(catalogDetails_);
    favorites_->setObjectName(QStringLiteral("favoriteToolPairs"));
    favorites_->setAccessibleName(QStringLiteral("Избранные пары инструмента"));
    favorites_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    favorites_->setMinimumContentsLength(16);
    detailsLayout->addWidget(favorites_);
    favorite_ = new QPushButton(QStringLiteral("В избранное"), catalogDetails_);
    favorite_->setObjectName(QStringLiteral("favoriteToolPair"));
    detailsLayout->addWidget(favorite_);
    details_ = wrapped(catalogDetails_);
    details_->setObjectName(QStringLiteral("toolPresetDetails"));
    details_->setStyleSheet(QStringLiteral("background:#E5F3F3;color:#1C2938;border:1px solid "
                                           "#087F83;border-radius:4px;padding:12px;"));
    detailsLayout->addWidget(details_);
    auto *sourcesGroup = new QToolButton(catalogDetails_);
    sourcesGroup->setObjectName(QStringLiteral("toolSourcesToggle"));
    sourcesGroup->setText(QStringLiteral("Источники и ограничения ▸"));
    sourcesGroup->setCheckable(true);
    sourcesGroup->setMinimumHeight(32);
    sources_ = wrapped(catalogDetails_);
    sources_->setObjectName(QStringLiteral("toolPresetSources"));
    sources_->hide();
    connect(sourcesGroup, &QToolButton::toggled, sources_, &QWidget::setVisible);
    detailsLayout->addWidget(sourcesGroup);
    detailsLayout->addWidget(sources_);
    layout->addWidget(catalogPanel_);
    manualPanel_ = new QWidget(this);
    auto *manualForm = new QFormLayout(manualPanel_);
    manualForm->setContentsMargins(0, 0, 0, 0);
    manualForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    manualForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const std::array<QString, 7> labels{
        QStringLiteral("Раскрытие V, мм:"),   QStringLiteral("Угол паза, °:"),
        QStringLiteral("Угол пуансона, °:"),  QStringLiteral("Радиус носика, мм:"),
        QStringLiteral("Длина матрицы, мм:"), QStringLiteral("Начальный зазор, мм:"),
        QStringLiteral("Длина пуансона, мм:")};
    const std::array<const char *, 7> names{"toolOpening",    "toolDieAngle", "toolPunchAngle",
                                            "toolNoseRadius", "toolLength",   "toolClearance",
                                            "toolPunchLength"};
    const std::array<double, 7> minimum{1, 30, 10, .1, 1, .1, 1};
    const std::array<double, 7> maximum{1000, 120, 120, 250, 100000, 1000, 100000};
    const std::array<double, 7> initial{48, 90, 85, 2, 100, 10, 100};
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        fields_[i] = new QDoubleSpinBox(manualPanel_);
        fields_[i]->setObjectName(QLatin1String(names[i]));
        fields_[i]->setAccessibleName(labels[i]);
        fields_[i]->setDecimals(6);
        fields_[i]->setRange(minimum[i], maximum[i]);
        fields_[i]->setValue(initial[i]);
        fields_[i]->setKeyboardTracking(false);
        if (i != 5)
            manualForm->addRow(labels[i], fields_[i]);
        connect(fields_[i], &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    }
    // Clearance is common to both modes; other dimensions come directly from a preset.
    auto *clearanceForm = new QFormLayout;
    clearanceForm->addRow(labels[5], fields_[5]);
    layout->addWidget(manualPanel_);
    profiles_ = new ToolProfilesWidget(this);
    profiles_->setObjectName(QStringLiteral("toolProfilesPreview"));
    layout->addWidget(profiles_);
    enlargeProfiles_ = new QPushButton(QStringLiteral("Открыть сечения крупно…"), this);
    enlargeProfiles_->setObjectName(QStringLiteral("enlargeToolProfiles"));
    layout->addWidget(enlargeProfiles_);
    auto *profileScope = wrapped(this);
    profileScope->setText(QStringLiteral("Упрощённые рабочие сечения · общий масштаб"));
    profileScope->setMinimumHeight(profileScope->heightForWidth(240));
    layout->insertWidget(layout->indexOf(profiles_), profileScope);
    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setObjectName(QStringLiteral("toolDetailsToggle"));
    detailsToggle_->setText(QStringLiteral("Избранное и данные инструмента ▸"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setMinimumHeight(30);
    layout->addWidget(detailsToggle_);
    layout->addWidget(catalogDetails_);
    connect(detailsToggle_, &QToolButton::toggled, this, [this] { updateDetails(); });
    connect(enlargeProfiles_, &QPushButton::clicked, this, [this] {
        const auto tools = parameters();
        if (!tools)
            return;
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("toolProfilesDialog"));
        dialog.setWindowTitle(QStringLiteral("Рабочие сечения инструмента"));
        dialog.resize(720, 520);
        dialog.setMinimumSize(440, 360);
        auto *dialogLayout = new QVBoxLayout(&dialog);
        auto *view = new ToolProfilesWidget(&dialog, true);
        view->setObjectName(QStringLiteral("enlargedToolProfiles"));
        view->setTools(tools);
        dialogLayout->addWidget(view, 1);
        auto *note = wrapped(&dialog);
        note->setText(QStringLiteral(
            "Сечения идеализированных тел, используемых в 3D-проверке. Масштаб одинаковый, размеры "
            "в мм. Высота клина условная и зависит от V. Корпуса, поднутрения и крепления не "
            "показаны. Это просмотр геометрии, не результат проверки совместимости."));
        note->setMinimumHeight(note->heightForWidth(400));
        dialogLayout->addWidget(note);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialogLayout->addWidget(buttons);
        dialog.exec();
    });
    auto *setup = new QWidget(this);
    auto *setupLayout = new QVBoxLayout(setup);
    setupLayout->setContentsMargins(0, 0, 0, 0);
    setupLayout->addLayout(clearanceForm);
    auto *setupToggle = new QToolButton(this);
    setupToggle->setText(QStringLiteral("Параметры установки ▸"));
    setupToggle->setObjectName(QStringLiteral("toolSetupOptionsToggle"));
    setupToggle->setCheckable(true);
    setupToggle->setMinimumHeight(30);
    layout->addWidget(setupToggle);
    layout->addWidget(setup);
    setup->hide();
    connect(setupToggle, &QToolButton::toggled, setup, &QWidget::setVisible);
    machine_ = new QLineEdit(setup);
    machine_->setObjectName(QStringLiteral("workshopMachine"));
    machine_->setAccessibleName(QStringLiteral("Название станка мастерской"));
    machine_->setPlaceholderText(QStringLiteral("Название станка (необязательно)"));
    machine_->setMaxLength(256);
    machine_->setToolTip(
        QStringLiteral("Название для проекта. Геометрия и крепления станка пока не проверяются."));
    setupLayout->addWidget(machine_);
    auto *scope = wrapped(this);
    scope->setText(
        QStringLiteral("Проверяется идеализированная рабочая зона: клин и V-паз. Корпуса, "
                       "поднутрения, держатели, станок и допустимая нагрузка не проверяются."));
    setupLayout->addWidget(scope);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(punch_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(die_, &QComboBox::currentIndexChanged, this, [this] {
        populateGrooves();
        changed();
    });
    connect(groove_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(machine_, &QLineEdit::textEdited, this, [this] { changed(); });
    connect(favorites_, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && static_cast<std::size_t>(i - 1) < favoritePairs_.size()) {
            selectPair(favoritePairs_[static_cast<std::size_t>(i - 1)]);
            changed();
        }
    });
    connect(favorite_, &QPushButton::clicked, this, [this] {
        if (!assignment())
            return;
        const auto pair = currentPair();
        const auto it = std::find(favoritePairs_.begin(), favoritePairs_.end(), pair);
        if (it != favoritePairs_.end())
            favoritePairs_.erase(it);
        else if (favoritePairs_.size() < 32)
            favoritePairs_.push_back(pair);
        QSettings settings;
        settings.beginWriteArray(QStringLiteral("tools/favorites"),
                                 static_cast<int>(favoritePairs_.size()));
        for (std::size_t i = 0; i < favoritePairs_.size(); ++i) {
            settings.setArrayIndex(static_cast<int>(i));
            settings.setValue(QStringLiteral("punch"), favoritePairs_[i].punchId);
            settings.setValue(QStringLiteral("die"), favoritePairs_[i].dieId);
            settings.setValue(QStringLiteral("groove"), favoritePairs_[i].groove);
        }
        settings.endArray();
        refreshFavorites();
        updateDetails();
    });
    QSettings settings;
    machine_->setText(settings.value(QStringLiteral("tools/machine")).toString().left(256));
    const auto count = std::min(settings.beginReadArray(QStringLiteral("tools/favorites")), 32);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        favoritePairs_.push_back({settings.value(QStringLiteral("punch")).toString(),
                                  settings.value(QStringLiteral("die")).toString(),
                                  settings.value(QStringLiteral("groove")).toInt()});
    }
    settings.endArray();
    refreshFavorites();
    selectPair({settings.value(QStringLiteral("tools/lastPunch")).toString(),
                settings.value(QStringLiteral("tools/lastDie")).toString(),
                settings.value(QStringLiteral("tools/lastGroove")).toInt()});
    if (catalog_.empty())
        mode_->setCurrentIndex(1);
    updateDetails();
}
const core::ToolPreset *ToolSelectionWidget::preset(const QComboBox *combo) const {
    const auto i = combo->currentData().toInt();
    return i >= 0 && static_cast<std::size_t>(i) < catalog_.size()
               ? &catalog_[static_cast<std::size_t>(i)]
               : nullptr;
}
void ToolSelectionWidget::populateGrooves(std::size_t selected) {
    const QSignalBlocker block(groove_);
    groove_->clear();
    if (const auto *p = preset(die_)) {
        for (const auto v : p->openingsMm)
            groove_->addItem(QStringLiteral("V %1 мм").arg(v));
        groove_->setCurrentIndex(selected < p->openingsMm.size() ? static_cast<int>(selected) : 0);
    }
}
bool ToolSelectionWidget::usesCatalog() const {
    return mode_->currentIndex() == 0;
}
std::optional<core::ToolAssignment> ToolSelectionWidget::assignment() const {
    const auto *p = preset(punch_);
    const auto *d = preset(die_);
    if (!usesCatalog() || !p || !d || groove_->currentIndex() < 0)
        return {};
    core::ToolAssignment a{*p, *d, static_cast<std::size_t>(groove_->currentIndex()),
                           fields_[5]->value(), machine_->text().toStdString()};
    return core::toolParameters(a) ? std::optional{a} : std::nullopt;
}
std::optional<core::PressTools> ToolSelectionWidget::parameters() const {
    if (usesCatalog()) {
        const auto a = assignment();
        return a ? core::toolParameters(*a) : std::nullopt;
    }
    const auto &f = fields_;
    core::PressTools t{f[0]->value(),
                       f[1]->value() * std::numbers::pi / 180,
                       f[2]->value() * std::numbers::pi / 180,
                       f[3]->value(),
                       f[4]->value(),
                       f[5]->value()};
    if (f[6]->value() != t.lengthMm)
        t.punchLengthMm = f[6]->value();
    return core::validPressTools(t) ? std::optional{t} : std::nullopt;
}
void ToolSelectionWidget::selectPreset(QComboBox *combo, const core::ToolPreset &p) {
    const auto it = std::find(catalog_.begin(), catalog_.end(), p);
    std::size_t i = 0;
    if (it == catalog_.end()) {
        i = catalog_.size();
        catalog_.push_back(p);
        combo->addItem(title(p) + QStringLiteral(" · из проекта"), static_cast<int>(i));
    } else
        i = static_cast<std::size_t>(it - catalog_.begin());
    combo->setCurrentIndex(combo->findData(static_cast<int>(i)));
}
void ToolSelectionWidget::setChoice(const std::optional<core::PressTools> &tools,
                                    const std::optional<core::ToolAssignment> &a) {
    updating_ = true;
    if (a) {
        selectPreset(punch_, a->punch);
        selectPreset(die_, a->die);
        populateGrooves(a->groove);
        fields_[5]->setValue(a->clearanceMm);
        machine_->setText(text(a->machineName));
        mode_->setCurrentIndex(0);
    } else if (tools) {
        const auto &t = *tools;
        const std::array<double, 7> values{t.openingMm,
                                           t.dieAngleRadians * 180 / std::numbers::pi,
                                           t.punchAngleRadians * 180 / std::numbers::pi,
                                           t.noseRadiusMm,
                                           t.lengthMm,
                                           t.clearanceMm,
                                           core::punchLengthMm(t)};
        for (std::size_t i = 0; i < fields_.size(); ++i)
            fields_[i]->setValue(values[i]);
        mode_->setCurrentIndex(1);
    }
    updating_ = false;
    updateDetails();
}
ToolSelectionWidget::Pair ToolSelectionWidget::currentPair() const {
    const auto *p = preset(punch_);
    const auto *d = preset(die_);
    return {p ? text(p->id) : QString{}, d ? text(d->id) : QString{}, groove_->currentIndex()};
}
void ToolSelectionWidget::selectPair(const Pair &pair) {
    const bool wasUpdating = updating_;
    updating_ = true;
    for (auto *combo : {punch_, die_}) {
        const auto &id = combo == punch_ ? pair.punchId : pair.dieId;
        for (int i = 1; i < combo->count(); ++i) {
            const auto index = combo->itemData(i).toInt();
            if (text(catalog_[static_cast<std::size_t>(index)].id) == id) {
                combo->setCurrentIndex(i);
                break;
            }
        }
    }
    populateGrooves(pair.groove < 0 ? 0 : static_cast<std::size_t>(pair.groove));
    updating_ = wasUpdating;
}
void ToolSelectionWidget::refreshFavorites() {
    const QSignalBlocker block(favorites_);
    favorites_->clear();
    favorites_->addItem(QStringLiteral("Избранные пары…"));
    for (const auto &pair : favoritePairs_) {
        const auto find = [&](const QString &id) {
            return std::find_if(catalog_.begin(), catalog_.end(),
                                [&](const auto &t) { return text(t.id) == id; });
        };
        const auto p = find(pair.punchId), d = find(pair.dieId);
        favorites_->addItem(p != catalog_.end() && d != catalog_.end() && pair.groove >= 0 &&
                                    static_cast<std::size_t>(pair.groove) < d->openingsMm.size()
                                ? text(p->code + " / " + d->code) +
                                      QStringLiteral(" · V %1 мм")
                                          .arg(d->openingsMm[static_cast<std::size_t>(pair.groove)])
                                : QStringLiteral("Пара недоступна в этом каталоге"));
    }
}
void ToolSelectionWidget::updateDetails() {
    catalogPanel_->setVisible(usesCatalog());
    manualPanel_->setVisible(!usesCatalog());
    detailsToggle_->setVisible(usesCatalog());
    catalogDetails_->setVisible(usesCatalog() && detailsToggle_->isChecked());
    const auto tools = parameters();
    profiles_->setTools(tools);
    enlargeProfiles_->setEnabled(tools.has_value());
    const auto a = assignment();
    favorite_->setEnabled(a.has_value());
    favorite_->setText(std::find(favoritePairs_.begin(), favoritePairs_.end(), currentPair()) ==
                               favoritePairs_.end()
                           ? QStringLiteral("В избранное")
                           : QStringLiteral("Убрать из избранного"));
    if (!a) {
        details_->setText(QStringLiteral("Выберите пуансон, матрицу и ручей. Совместимость пары "
                                         "проверяется при подготовке гибки."));
        details_->setMinimumHeight(details_->heightForWidth(240));
        sources_->clear();
        sources_->setMinimumHeight(0);
        return;
    }
    const auto &p = a->punch;
    const auto &d = a->die;
    details_->setText(QStringLiteral("%1\n%2° · R %3 мм · L %4 мм\n\n%5\nV %6 мм · %7° · L %8 "
                                     "мм\n\nСхема рабочей зоны, полный профиль не подтверждён.")
                          .arg(title(p))
                          .arg(p.angleRadians * 180 / std::numbers::pi)
                          .arg(*p.noseRadiusMm)
                          .arg(p.lengthMm)
                          .arg(title(d))
                          .arg(d.openingsMm[a->groove])
                          .arg(d.angleRadians * 180 / std::numbers::pi)
                          .arg(d.lengthMm));
    details_->setMinimumHeight(details_->heightForWidth(240));
    sources_->setText(text(p.source + "\n" + p.notes + "\n\n" + d.source + "\n" + d.notes));
    sources_->setMinimumHeight(sources_->heightForWidth(240));
}
void ToolSelectionWidget::changed() {
    if (updating_)
        return;
    updateDetails();
    if (onChanged)
        onChanged();
}
QString ToolSelectionWidget::summary() const {
    const auto tools = parameters();
    if (!tools)
        return QStringLiteral("Пара не выбрана");
    if (const auto a = assignment()) {
        return text(a->punch.code + " / " + a->die.code) +
               QStringLiteral(" · V %1 мм%2")
                   .arg(tools->openingMm)
                   .arg(a->punch.teaching || a->die.teaching ? QStringLiteral(" · учебная пара")
                                                             : QString{});
    }
    return QStringLiteral("Вручную · V %1 мм · R %2 мм")
        .arg(tools->openingMm)
        .arg(tools->noseRadiusMm);
}
QString ToolSelectionWidget::shortSummary() const {
    const auto tools = parameters();
    if (!tools)
        return QStringLiteral("Выбрать пару");
    const auto a = assignment();
    return (a && (a->punch.teaching || a->die.teaching) ? QStringLiteral("Учебная пара")
            : a                                         ? text(a->die.code)
                                                        : QStringLiteral("Вручную")) +
           QStringLiteral(" · V %1").arg(tools->openingMm);
}
void ToolSelectionWidget::rememberChoice() {
    const auto a = assignment();
    if (!a)
        return;
    const auto pair = currentPair();
    QSettings settings;
    settings.setValue(QStringLiteral("tools/lastPunch"), pair.punchId);
    settings.setValue(QStringLiteral("tools/lastDie"), pair.dieId);
    settings.setValue(QStringLiteral("tools/lastGroove"), pair.groove);
    settings.setValue(QStringLiteral("tools/machine"), machine_->text());
}
} // namespace bending::ui
