#include "ToolSelectionWidget.h"
#include "ToolLibraryDialog.h"
#include "ToolPickerPopup.h"
#include "bending/application/BendRoute.h"
#include "bending/core/ToolGeometry.h"
#include <QAction>
#include <QCheckBox>
#include <QMenu>

#include "bending/ui/Preferences.h"
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
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
           (t.teaching ? QStringLiteral(" · учебный") : QString{}) +
           (!t.parametersConfirmed ? QStringLiteral(" · нужны данные") : QString{});
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
    : QWidget(parent), catalog_(std::move(catalog)), workshopSize_(catalog_.size()) {
    std::erase_if(catalog_, [](const auto &tool) { return !core::hasLibraryProfile(tool); });
    workshopSize_ = catalog_.size();
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignTop);
    lengthWarning_ = wrapped(this);
    lengthWarning_->setObjectName("toolLengthWarning");
    lengthWarning_->setStyleSheet(
        "background:#FFF2CC;color:#704500;border-radius:4px;padding:8px;");
    lengthWarning_->hide();
    layout->addWidget(lengthWarning_);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("toolSourceMode"));
    mode_->setAccessibleName(QStringLiteral("Источник параметров инструмента"));
    mode_->addItem(QStringLiteral("Библиотека инструмента"));
    layout->addWidget(mode_);
    mode_->hide();
    recommend_ = new QPushButton(QStringLiteral("Подобрать инструмент"), this);
    recommend_->setObjectName(QStringLiteral("recommendTools"));
    recommend_->setToolTip(QStringLiteral(
        "Ищет пару и проверенный порядок в фоне. Стрелка рядом — быстрый подбор без поиска. "
        "V около 8t — ориентир подбора, а не минимальный размер. Можно вручную выбрать другой "
        "ручей в пределах текущего режима V/t ≥ 5. Материал, угол, паспорт и нагрузка могут "
        "задавать более строгие ограничения; нагрузка пока не проверяется."));
    auto *recommendRow = new QHBoxLayout;
    recommendRow->addWidget(recommend_, 1);
    auto *recommendMore = new QToolButton(this);
    recommendMore->setObjectName("recommendToolsOptions");
    recommendMore->setArrowType(Qt::DownArrow);
    recommendMore->setToolTip(QStringLiteral("Быстрый подбор без поиска порядка"));
    recommendMore->setPopupMode(QToolButton::InstantPopup);
    auto *recommendMenu = new QMenu(recommendMore);
    auto *quick = quickRecommend_ =
        recommendMenu->addAction(QStringLiteral("Быстро по параметрам"));
    quick->setObjectName("recommendToolsQuick");
    connect(quick, &QAction::triggered, this, [this] {
        if (onCancelChecked)
            onCancelChecked();
        recommend();
    });
    recommendMore->setMenu(recommendMenu);
    recommendRow->addWidget(recommendMore);
    layout->addLayout(recommendRow);
    fitSections_ = new QPushButton(QStringLiteral("Подобрать секции"), this);
    fitSections_->setObjectName(QStringLiteral("fitSelectedToolSections"));
    fitSections_->setToolTip(QStringLiteral(
        "Рассчитать состав выбранных пуансона и матрицы по длине гиба, наличию и пределу зазора. "
        "Артикулы, ручей и ориентация сохраняются. В общей установке — по самому длинному "
        "включённому гибу; в индивидуальной — по выбранному. Проверка столкновений выполняется "
        "при переходе к гибке."));
    layout->addWidget(fitSections_);
    connect(fitSections_, &QPushButton::clicked, this, [this] { fitSelectedAssemblies(); });
    stock_ = new QPushButton(QStringLiteral("Библиотека инструмента…"), this);
    stock_->setObjectName("toolStock");
    stock_->setEnabled(false);
    layout->addWidget(stock_);
    segmentGap_ = new QDoubleSpinBox(this);
    segmentGap_->setObjectName("segmentGapLimit");
    segmentGap_->setRange(0, 1000);
    segmentGap_->setDecimals(3);
    segmentGap_->setPrefix(QStringLiteral("Макс. зазор: "));
    segmentGap_->setSuffix(QStringLiteral(" мм"));
    segmentGap_->setValue(preferences()->value("tools/segmentGap", 3).toDouble());
    segmentGap_->setToolTip(
        QStringLiteral("Сначала сборка без зазоров. Увеличьте предел, если секций не хватает. "
                       "Качество гиба в промежутках не рассчитывается."));
    layout->addWidget(segmentGap_);
    assemblySummary_ = wrapped(this);
    assemblySummary_->setObjectName("toolAssemblySummary");
    layout->addWidget(assemblySummary_);
    connect(segmentGap_, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!updating_) {
            preferences()->setValue("tools/segmentGap", segmentGap_->value());
            fitAssemblies();
            changed();
        }
    });
    connect(stock_, &QPushButton::clicked, this, [this] { showLibrary(); });
    recommendation_ = wrapped(this);
    recommendation_->setObjectName(QStringLiteral("toolRecommendation"));
    recommendation_->hide();
    layout->addWidget(recommendation_);
    connect(recommend_, &QPushButton::clicked, this, [this] {
        if (onCheckedRecommend)
            onCheckedRecommend();
        else
            recommend();
    });
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
        c->hide(); // Binding model shared with the graphical picker; no duplicate input.
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
    clearance_ = new QDoubleSpinBox(this);
    clearance_->setObjectName(QStringLiteral("toolClearance"));
    clearance_->setAccessibleName(QStringLiteral("Начальный зазор, мм"));
    clearance_->setDecimals(6);
    clearance_->setRange(.1, 1000);
    clearance_->setValue(10);
    clearance_->setKeyboardTracking(false);
    connect(clearance_, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    auto *clearanceForm = new QFormLayout;
    clearanceForm->addRow(QStringLiteral("Начальный зазор, мм:"), clearance_);
    endSupportLimit_ = new QDoubleSpinBox(this);
    endSupportLimit_->setObjectName("maximumEndSupportLoss");
    endSupportLimit_->setAccessibleName(QStringLiteral("Допуск нехватки опоры у каждого торца"));
    endSupportLimit_->setRange(0, 1000);
    endSupportLimit_->setDecimals(3);
    endSupportLimit_->setSuffix(QStringLiteral(" мм"));
    endSupportLimit_->setKeyboardTracking(false);
    const auto storedLimit = preferences()->value("checks/maximumEndSupportLossMm", 3.).toDouble();
    endSupportLimit_->setValue(application::validEndSupportLimitMm(storedLimit) ? storedLimit : 3.);
    endSupportLimit_->setToolTip(QStringLiteral(
        "Нехватка материала только у каждого торца линии гиба. 0 — полная опора. "
        "В пределах допуска доступен просмотр с предупреждением; нагрузка и устойчивость "
        "не подтверждаются. Внутренние разрывы и столкновения не разрешаются."));
    clearanceForm->addRow(QStringLiteral("Допуск у торца:"), endSupportLimit_);
    connect(endSupportLimit_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        preferences()->setValue("checks/maximumEndSupportLossMm", value);
        changed();
    });
    realGeometry_ = new QCheckBox(QStringLiteral("Использовать известные формы DXF"), this);
    realGeometry_->setObjectName(QStringLiteral("useDrawingToolGeometry"));
    realGeometry_->setToolTip(QStringLiteral(
        "Для каждого инструмента используется его известный контур. Недостающая "
        "форма отдельно отмечена как условная. При новом выборе формы DXF "
        "включаются автоматически. Узлы станка включаются отдельным выбором учебной сборки."));
    layout->addWidget(realGeometry_);
    realGeometry_->setChecked(true);
    realGeometry_->hide();
    demoMachine_ = new QCheckBox(QStringLiteral("Демонстрационный станок"), this);
    demoMachine_->setObjectName(QStringLiteral("demoMachine"));
    demoMachine_->setChecked(true);
    demoMachine_->setToolTip(QStringLiteral(
        "Проверять приближённый станок и пол. Высота стола от пола 900 мм; "
        "учитывается посадка адаптера и матрицы. "
        "Это учебная сборка, не точная модель SPEED BEND. Проверка по кадрам; ход и перенос не "
        "проверены."));
    layout->insertWidget(0, demoMachine_);
    connect(demoMachine_, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && !updating_)
            machineModel_ = core::MachineModel::DemoPressV3;
        changed();
    });
    upgradeFloor_ = new QPushButton(QStringLiteral("Добавить пол"), this);
    upgradeFloor_->setObjectName(QStringLiteral("upgradeFloorCheck"));
    upgradeFloor_->setToolTip(
        QStringLiteral("Обновить учебную установку и заново проверить её с полом."));
    upgradeFloor_->hide();
    layout->insertWidget(1, upgradeFloor_);
    connect(upgradeFloor_, &QPushButton::clicked, this, [this] {
        machineModel_ = core::MachineModel::DemoPressV3;
        changed();
    });
    geometryScope_ = wrapped(this);
    geometryScope_->setObjectName(QStringLiteral("toolGeometryScope"));
    layout->addWidget(geometryScope_);
    connect(realGeometry_, &QCheckBox::toggled, this, [this] { changed(); });
    detailsToggle_ = new QToolButton(this);
    detailsToggle_->setObjectName(QStringLiteral("toolDetailsToggle"));
    detailsToggle_->setText(QStringLiteral("Избранное и данные инструмента ▸"));
    detailsToggle_->setCheckable(true);
    detailsToggle_->setMinimumHeight(30);
    layout->addWidget(detailsToggle_);
    layout->addWidget(catalogDetails_);
    connect(detailsToggle_, &QToolButton::toggled, this, [this] { updateDetails(); });
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
        QStringLiteral("Название для проекта; оно не выбирает геометрическую модель станка."));
    setupLayout->addWidget(machine_);
    auto *scope = wrapped(this);
    scope->setText(QStringLiteral(
        "Источник формы каждого инструмента указан возле сечений. "
        "Учебная сборка станка выбирается отдельно; допустимая нагрузка не проверяется."));
    setupLayout->addWidget(scope);
    const auto selected = [this](std::optional<core::ToolKind> only = {}) {
        if (!updating_) {
            fitAssemblies(only);
            chooseDrawingGeometry();
        }
        changed(!only                            ? Change::Parameters
                : *only == core::ToolKind::Punch ? Change::Punch
                                                 : Change::Die);
    };
    connect(mode_, &QComboBox::currentIndexChanged, this, [selected] { selected(); });
    connect(punch_, &QComboBox::currentIndexChanged, this, [this, selected] {
        if (!updating_)
            resetPunchOrientation();
        selected(core::ToolKind::Punch);
    });
    connect(die_, &QComboBox::currentIndexChanged, this, [this] {
        populateGrooves();
        if (!updating_) {
            fitAssemblies(core::ToolKind::Die);
            chooseDrawingGeometry();
        }
        changed(Change::Die);
    });
    connect(groove_, &QComboBox::currentIndexChanged, this,
            [selected] { selected(core::ToolKind::Die); });
    for (auto *selector : {punch_, die_, groove_})
        connect(selector, &QComboBox::activated, this, [this] {
            if (!updating_ && chooseDrawingGeometry())
                changed();
        });
    connect(machine_, &QLineEdit::textEdited, this, [this] { changed(); });
    connect(favorites_, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && static_cast<std::size_t>(i - 1) < favoritePairs_.size()) {
            const auto previous = selectedTool(core::ToolKind::Punch);
            selectPair(favoritePairs_[static_cast<std::size_t>(i - 1)]);
            if (previous != selectedTool(core::ToolKind::Punch))
                resetPunchOrientation();
            chooseDrawingGeometry();
            changed(Change::Pair);
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
        auto settings = preferences();
        settings->beginWriteArray(QStringLiteral("tools/favorites"),
                                  static_cast<int>(favoritePairs_.size()));
        for (std::size_t i = 0; i < favoritePairs_.size(); ++i) {
            settings->setArrayIndex(static_cast<int>(i));
            settings->setValue(QStringLiteral("punch"), favoritePairs_[i].punchId);
            settings->setValue(QStringLiteral("die"), favoritePairs_[i].dieId);
            settings->setValue(QStringLiteral("groove"), favoritePairs_[i].groove);
        }
        settings->endArray();
        refreshFavorites();
        updateDetails();
    });
    auto settings = preferences();
    machine_->setText(settings->value(QStringLiteral("tools/machine")).toString().left(256));
    const auto count = std::min(settings->beginReadArray(QStringLiteral("tools/favorites")), 32);
    for (int i = 0; i < count; ++i) {
        settings->setArrayIndex(i);
        favoritePairs_.push_back({settings->value(QStringLiteral("punch")).toString(),
                                  settings->value(QStringLiteral("die")).toString(),
                                  settings->value(QStringLiteral("groove")).toInt()});
    }
    settings->endArray();
    refreshFavorites();
    selectPair({settings->value(QStringLiteral("tools/lastPunch")).toString(),
                settings->value(QStringLiteral("tools/lastDie")).toString(),
                settings->value(QStringLiteral("tools/lastGroove")).toInt()});
    resetPunchOrientation();
    realGeometry_->setChecked(true);
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
        for (std::size_t i = 0; i < p->openingsMm.size(); ++i) {
            const auto g = core::dieGroove(*p, i);
            groove_->addItem(p->grooveSettings.empty()
                                 ? QStringLiteral("V %1 мм").arg(p->openingsMm[i])
                                 : QStringLiteral("V %1 мм · %2°")
                                       .arg(p->openingsMm[i])
                                       .arg(g->angleRadians * 180 / std::numbers::pi));
        }
        groove_->setCurrentIndex(selected < p->openingsMm.size() ? static_cast<int>(selected) : 0);
    }
}
bool ToolSelectionWidget::usesCatalog() const {
    return mode_->currentIndex() == 0;
}
double ToolSelectionWidget::maximumEndSupportLossMm() const {
    return endSupportLimit_->value();
}
std::optional<core::ToolAssignment> ToolSelectionWidget::assignment() const {
    const auto *p = preset(punch_);
    const auto *d = preset(die_);
    if (!usesCatalog() || !p || !d || p->teaching || d->teaching || groove_->currentIndex() < 0)
        return {};
    core::ToolAssignment a{*p,
                           *d,
                           static_cast<std::size_t>(groove_->currentIndex()),
                           clearance_->value(),
                           machine_->text().toStdString(),
                           true};
    a.punchAssembly = punchAssembly_;
    a.dieAssembly = dieAssembly_;
    a.machineModel = demoMachine_->isChecked() ? machineModel_ : core::MachineModel::None;
    a.punchReversed = punchReversed_;
    return core::toolParameters(a) ? std::optional{a} : std::nullopt;
}
std::optional<core::PressTools> ToolSelectionWidget::parameters() const {
    if (!geometryIssue().isEmpty())
        return {};
    const auto t = rawParameters();
    return t && (!selectionRequest_ ||
                 core::standardAirBendOpening(selectionRequest_->thicknessMm, t->openingMm))
               ? t
               : std::nullopt;
}
std::optional<core::PressTools> ToolSelectionWidget::rawParameters() const {
    if (usesCatalog()) {
        const auto a = assignment();
        return a ? core::toolParameters(*a) : std::nullopt;
    }
    return {}; // Saved manual parameters may be read, but cannot create a new tool body.
}
QString ToolSelectionWidget::lengthIssue(const application::BendRouteToolLengthIssue &lengths) {
    const auto number = [](double value) { return QLocale().toString(value, 'g', 6); };
    QString result = QStringLiteral("Длина гиба: %1 мм.").arg(number(lengths.bendLengthMm));
    const auto append = [&](const QString &name, double length) {
        result += QStringLiteral("\n%1: %2 мм").arg(name, number(length));
        if (length < lengths.bendLengthMm)
            result +=
                QStringLiteral(" — не хватает %1 мм").arg(number(lengths.bendLengthMm - length));
        result += QLatin1Char('.');
    };
    append(QStringLiteral("Пуансон"), lengths.punchLengthMm);
    append(QStringLiteral("Матрица"), lengths.dieLengthMm);
    return result + QStringLiteral("\nТолько просмотр: гибка не подтверждена. "
                                   "Подберите секции или другой инструмент.");
}
QString ToolSelectionWidget::lengthIssue() const {
    const auto tools = rawParameters();
    if (!tools || !selectionRequest_)
        return {};
    const application::BendRouteToolLengthIssue lengths{
        selectionRequest_->bendLengthMm, core::punchLengthMm(*tools), tools->lengthMm};
    return lengths.coversBend(lengthToleranceMm_) ? QString{} : lengthIssue(lengths);
}
QString ToolSelectionWidget::openingIssue() const {
    if (!usesCatalog() || !selectionRequest_)
        return {};
    double v = 0;
    if (usesCatalog()) {
        const auto *d = preset(die_);
        const int g = groove_->currentIndex();
        if (!d || g < 0 || static_cast<std::size_t>(g) >= d->openingsMm.size())
            return {};
        v = d->openingsMm[static_cast<std::size_t>(g)];
    }
    return core::standardAirBendOpening(selectionRequest_->thicknessMm, v)
               ? QString{}
               : QStringLiteral("V %1 мм слишком мало для t %2 мм. Граница текущего режима: "
                                "%3 мм (5t). Проверка заблокирована. Ориентир подбора: "
                                "%4 мм (8t), не минимальный размер.")
                     .arg(v)
                     .arg(selectionRequest_->thicknessMm)
                     .arg(core::minimumStandardDieOpeningMm(selectionRequest_->thicknessMm))
                     .arg(core::referenceDieOpeningMm(selectionRequest_->thicknessMm));
}
void ToolSelectionWidget::selectPreset(QComboBox *combo, const core::ToolPreset &p) {
    if (p.teaching) {
        combo->setCurrentIndex(0);
        return;
    }
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
bool ToolSelectionWidget::chooseDrawingGeometry() {
    const auto *p = preset(punch_), *d = preset(die_);
    if (!usesCatalog() || !p || !d)
        return false;
    const bool known = true;
    if (realGeometry_->isChecked() == known)
        return false;
    const QSignalBlocker block(realGeometry_);
    realGeometry_->setChecked(known);
    return true;
}
std::optional<core::ToolPreset> ToolSelectionWidget::selectedTool(core::ToolKind kind) const {
    if (!usesCatalog())
        return {};
    const auto *tool = preset(kind == core::ToolKind::Punch ? punch_ : die_);
    return tool && !tool->teaching ? std::optional{*tool} : std::nullopt;
}
std::size_t ToolSelectionWidget::selectedGroove() const {
    return static_cast<std::size_t>(std::max(0, groove_->currentIndex()));
}
QString ToolSelectionWidget::optionIssue(const core::ToolPreset &tool, std::size_t groove) const {
    if (tool.teaching)
        return QStringLiteral("Учебный инструмент исключён");
    if (!tool.drawing || !tool.drawing->millimeters || !core::validToolDrawing(*tool.drawing))
        return QStringLiteral("Добавьте контур DXF в мм");
    if (!tool.parametersConfirmed) {
        if (tool.lengthMm < 1)
            return QStringLiteral(
                "Укажите длину инструмента или сохраните доступные прямые секции в библиотеке.");
        return QStringLiteral("Инструмент сохранён для просмотра. Проверьте рабочий профиль и "
                              "сохраните данные в библиотеке.");
    }
    if (tool.kind == core::ToolKind::Die) {
        if (groove >= tool.openingsMm.size())
            return QStringLiteral("Нужны параметры ручья");
        if (selectionRequest_ &&
            !core::standardAirBendOpening(selectionRequest_->thicknessMm, tool.openingsMm[groove]))
            return QStringLiteral("V %1: мало для t %2 мм")
                .arg(tool.openingsMm[groove])
                .arg(selectionRequest_->thicknessMm);
    }
    return {};
}
QString ToolSelectionWidget::assignmentIssue(const std::optional<core::ToolAssignment> &a) {
    if (!a)
        return QStringLiteral("Выберите пуансон и матрицу с контурами DXF.");
    if (a->punch.teaching || a->die.teaching)
        return QStringLiteral("Учебные инструменты исключены. Назначьте реальные профили.");
    if (!a->useDrawingGeometry || !a->punch.drawing || !a->die.drawing)
        return QStringLiteral("Назначьте реальные профили пуансона и матрицы из DXF.");
    if (!core::hasCompleteDrawingToolGeometry(*a))
        return QStringLiteral(
            "Рабочие участки DXF этой пары не поддержаны. Условная замена не применяется.");
    return {};
}
QString ToolSelectionWidget::geometryIssue() const {
    if (!usesCatalog())
        return QStringLiteral(
            "Сохранённый условный инструмент: выберите реальные профили из библиотеки.");
    const auto *p = preset(punch_), *d = preset(die_);
    if (!p || !d)
        return QStringLiteral("Выберите пуансон и матрицу на изображениях.");
    for (const auto *tool : {p, d}) {
        const auto issue =
            optionIssue(*tool, tool->kind == core::ToolKind::Die ? selectedGroove() : 0);
        if (!issue.isEmpty())
            return QString::fromStdString(tool->code) + QStringLiteral(": ") + issue;
    }
    if (const auto a = assignment())
        return assignmentIssue(a);
    core::ToolAssignment proposed{
        *p, *d, selectedGroove(), clearance_->value(), machine_->text().toStdString(), true};
    if (!core::hasCompleteDrawingToolGeometry(proposed))
        return assignmentIssue(proposed);
    return QStringLiteral("Подберите секции из наличия или уточните длину инструмента.");
}
bool ToolSelectionWidget::chooseTool(const core::ToolPreset &tool, std::size_t groove) {
    if (!isEnabled() || !optionIssue(tool, groove).isEmpty())
        return false;
    const auto found = std::find(catalog_.begin(), catalog_.end(), tool);
    if (found == catalog_.end())
        return false;
    if (selectedTool(tool.kind) == tool && usesCatalog() && realGeometry_->isChecked() &&
        (tool.kind == core::ToolKind::Punch || selectedGroove() == groove)) {
        // Selecting the common profile explicitly may replace another bend's exception.
        if (allBends_)
            changed(tool.kind == core::ToolKind::Punch ? Change::Punch : Change::Die);
        return true;
    }
    const auto previous = selectedTool(tool.kind);
    const bool sameProfile = previous && previous->id == tool.id && previous->drawing &&
                             tool.drawing &&
                             previous->drawing->millimeters == tool.drawing->millimeters &&
                             previous->drawing->edges == tool.drawing->edges;
    const bool newPunch = tool.kind == core::ToolKind::Punch && !sameProfile;
    updating_ = true;
    mode_->setCurrentIndex(0);
    auto *combo = tool.kind == core::ToolKind::Punch ? punch_ : die_;
    combo->setCurrentIndex(combo->findData(static_cast<int>(found - catalog_.begin())));
    if (tool.kind == core::ToolKind::Die)
        populateGrooves(groove);
    realGeometry_->setChecked(true);
    fitAssemblies(tool.kind);
    if (newPunch)
        resetPunchOrientation();
    updating_ = false;
    changed(tool.kind == core::ToolKind::Punch ? Change::Punch : Change::Die);
    return true;
}
void ToolSelectionWidget::showPicker(core::ToolKind kind, QWidget *anchor) {
    if (!isEnabled() || picker_)
        return;
    // The tail holds immutable project/previous-choice snapshots, not workshop stock.
    // Showing it alongside the workshop makes each edited profile look duplicated.
    const core::ToolCatalog workshop(catalog_.begin(), catalog_.begin() + workshopSize_);
    ToolPickerPopup dialog(
        workshop, kind, selectedTool(kind), selectedGroove(),
        [this](const auto &tool, auto groove) { return optionIssue(tool, groove); },
        profileImporter_ && static_cast<bool>(libraryStorage_), anchor, punchReversed_);
    picker_ = &dialog;
    const auto result = dialog.exec();
    picker_.clear();
    if (result != ToolPickerPopup::ImportDxf)
        QTimer::singleShot(0, anchor, [anchor] { anchor->setFocus(Qt::PopupFocusReason); });
    if (!isEnabled())
        return;
    if (result == ToolPickerPopup::ImportDxf)
        importDxf(kind);
    else if (result == QDialog::Accepted && dialog.choice())
        chooseTool(dialog.choice()->first, dialog.choice()->second);
}
void ToolSelectionWidget::setChoice(const std::optional<core::PressTools> &tools,
                                    const std::optional<core::ToolAssignment> &a) {
    if (picker_)
        picker_->reject();
    updating_ = true;
    if (a) {
        machineModel_ = a->machineModel;
        punchReversed_ = a->punchReversed;
        demoMachine_->setChecked(a->machineModel != core::MachineModel::None);
        punchAssembly_ = a->punchAssembly;
        dieAssembly_ = a->dieAssembly;
        selectPreset(punch_, a->punch);
        selectPreset(die_, a->die);
        populateGrooves(a->groove);
        clearance_->setValue(a->clearanceMm);
        machine_->setText(text(a->machineName));
        realGeometry_->setChecked(a->useDrawingGeometry);
        mode_->setCurrentIndex(0);
    } else if (tools) {
        machineModel_ = tools->machineModel;
        punchReversed_ = tools->punchReversed;
        demoMachine_->setChecked(tools->machineModel != core::MachineModel::None);
        punchAssembly_.reset();
        dieAssembly_.reset();
        punch_->setCurrentIndex(0);
        die_->setCurrentIndex(0);
        populateGrooves();
        clearance_->setValue(tools->clearanceMm);
        realGeometry_->setChecked(false);
        mode_->setCurrentIndex(1);
    } else {
        // New/unassigned bend: use workshop stock, never a previous project's captured stock.
        machineModel_ = core::MachineModel::DemoPressV3;
        punchReversed_ = false;
        demoMachine_->setChecked(true);
        for (auto *combo : {punch_, die_}) {
            const auto *selected = preset(combo);
            const auto id = selected ? selected->id : std::string{};
            const auto end = catalog_.begin() + workshopSize_;
            const auto found =
                std::find_if(catalog_.begin(), end, [&](const auto &t) { return t.id == id; });
            combo->setCurrentIndex(
                found == end ? 0 : combo->findData(static_cast<int>(found - catalog_.begin())));
        }
        fitAssemblies();
        chooseDrawingGeometry();
        resetPunchOrientation();
    }
    updating_ = false;
    updateDetails();
}
ToolSelectionWidget::Pair ToolSelectionWidget::currentPair() const {
    const auto *p = preset(punch_);
    const auto *d = preset(die_);
    return {p ? text(p->id) : QString{}, d ? text(d->id) : QString{}, groove_->currentIndex()};
}
void ToolSelectionWidget::resetPunchOrientation() {
    const auto *punch = preset(punch_);
    punchReversed_ = punch && core::defaultPunchOrientation(*punch).value_or(false);
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
    if (!updating_)
        fitAssemblies();
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
    const auto lengthIssueText = lengthIssue();
    lengthWarning_->setText(lengthIssueText);
    lengthWarning_->setVisible(!lengthIssueText.isEmpty());
    const bool legacyMachine =
        demoMachine_->isChecked() && machineModel_ != core::MachineModel::DemoPressV3;
    upgradeFloor_->setVisible(legacyMachine);
    demoMachine_->setText(legacyMachine ? QStringLiteral("Демонстрационный станок · без пола")
                                        : QStringLiteral("Демонстрационный станок и пол"));
    QStringList assemblyLines;
    if (allBends_ && selectionRequest_)
        assemblyLines << QStringLiteral("Для всех гибов: длина инструмента не менее %1 мм.")
                             .arg(selectionRequest_->bendLengthMm, 0, 'f', 2);
    const auto describe = [&](const core::ToolPreset *tool,
                              const std::optional<core::ToolAssembly> &a, const QString &name) {
        if (!tool)
            return;
        if (!tool->segments) {
            assemblyLines << name + QStringLiteral(": цельный · %1 мм (наличие не учтено)")
                                        .arg(tool->lengthMm);
            return;
        }
        if (!a) {
            assemblyLines << name + QStringLiteral(": нет сборки из указанного наличия. Заполните "
                                                   "секции или измените предел зазора.");
            return;
        }
        QStringList pieces;
        for (const auto row : a->pieces)
            pieces << QString::number((*tool->segments)[row].lengthMm);
        assemblyLines << name + QStringLiteral(": %1 · общая длина %2 мм · %3 секц. · зазор %4 мм")
                                    .arg(pieces.join(QStringLiteral(" + ")))
                                    .arg(*core::toolLength(*tool, a))
                                    .arg(a->pieces.size())
                                    .arg(a->gapMm);
    };
    describe(preset(punch_), punchAssembly_, QStringLiteral("Пуансон"));
    describe(preset(die_), dieAssembly_, QStringLiteral("Матрица"));
    assemblySummary_->setText(assemblyLines.join(QLatin1Char('\n')));
    assemblySummary_->setMinimumHeight(std::max(0, assemblySummary_->heightForWidth(240)));
    const auto *stockPunch = preset(punch_), *stockDie = preset(die_);
    const bool stockConfigured =
        (stockPunch && stockPunch->segments) || (stockDie && stockDie->segments);
    assemblySummary_->setVisible(usesCatalog() && (stockConfigured || allBends_));
    segmentGap_->setVisible(usesCatalog() && stockConfigured);
    recommend_->setEnabled(checkedBusy_ || (usesCatalog() && selectionRequest_.has_value()));
    quickRecommend_->setEnabled(usesCatalog() && selectionRequest_.has_value());
    fitSections_->setEnabled(usesCatalog() && selectionRequest_ && stockPunch && stockDie);
    realGeometry_->hide();
    geometryScope_->setText(geometryIssue());
    geometryScope_->setVisible(!geometryScope_->text().isEmpty());
    catalogPanel_->show();
    detailsToggle_->setVisible(usesCatalog());
    catalogDetails_->setVisible(usesCatalog() && detailsToggle_->isChecked());
    if (const auto issue = openingIssue(); !issue.isEmpty()) {
        geometryScope_->setText(issue);
        geometryScope_->setVisible(true);
    }
    const auto a = assignment();
    favorite_->setEnabled(a.has_value());
    favorite_->setText(std::find(favoritePairs_.begin(), favoritePairs_.end(), currentPair()) ==
                               favoritePairs_.end()
                           ? QStringLiteral("В избранное")
                           : QStringLiteral("Убрать из избранного"));
    if (!a) {
        QString missing;
        for (const auto *tool : {stockPunch, stockDie})
            if (tool && !tool->parametersConfirmed)
                missing +=
                    title(*tool) + QStringLiteral(": ") + text(tool->notes) + QLatin1Char('\n');
        details_->setText(
            missing.isEmpty()
                ? QStringLiteral("Выберите пуансон, матрицу и ручей. Совместимость пары "
                                 "проверяется при подготовке гибки.")
                : missing + QStringLiteral("Для расчёта дополните параметры в каталоге мастерской. "
                                           "Доступные исходные контуры показываются отдельно."));
        details_->setMinimumHeight(details_->heightForWidth(240));
        sources_->clear();
        sources_->setMinimumHeight(0);
        return;
    }
    const auto &p = a->punch;
    const auto &d = a->die;
    details_->setText(QStringLiteral("%1\n%2° · R %3 мм · L %4 мм\n\n%5\nV %6 мм · %7° · L %8 "
                                     "мм")
                          .arg(title(p))
                          .arg(p.angleRadians * 180 / std::numbers::pi)
                          .arg(*p.noseRadiusMm)
                          .arg(*core::toolLength(p, a->punchAssembly))
                          .arg(title(d))
                          .arg(d.openingsMm[a->groove])
                          .arg(core::dieGroove(d, a->groove)->angleRadians * 180 / std::numbers::pi)
                          .arg(*core::toolLength(d, a->dieAssembly)));
    details_->setMinimumHeight(details_->heightForWidth(240));
    sources_->setText(text(p.source + "\n" + p.notes + "\n\n" + d.source + "\n" + d.notes));
    sources_->setMinimumHeight(sources_->heightForWidth(240));
}
void ToolSelectionWidget::changed(Change selected) {
    recommendation_->clear();
    recommendation_->hide();
    if (updating_)
        return;
    updateDetails();
    if (onChanged)
        onChanged(selected);
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
    auto settings = preferences();
    settings->setValue(QStringLiteral("tools/lastPunch"), pair.punchId);
    settings->setValue(QStringLiteral("tools/lastDie"), pair.dieId);
    settings->setValue(QStringLiteral("tools/lastGroove"), pair.groove);
    settings->setValue(QStringLiteral("tools/lastDrawingGeometry"), realGeometry_->isChecked());
    settings->setValue(QStringLiteral("tools/machine"), machine_->text());
}
void ToolSelectionWidget::setSelectionRequest(std::optional<core::ToolSelectionRequest> request,
                                              bool allBends, double lengthToleranceMm) {
    const bool different = selectionRequest_ != request || allBends_ != allBends;
    const bool toleranceChanged = lengthToleranceMm_ != lengthToleranceMm;
    lengthToleranceMm_ = lengthToleranceMm;
    if (different) {
        allBends_ = allBends;
        selectionRequest_ = request;
        fitAssemblies();
        recommendation_->clear();
        recommendation_->hide();
    }
    if (different || toleranceChanged)
        updateDetails();
    recommend_->setEnabled(usesCatalog() && selectionRequest_.has_value());
}
void ToolSelectionWidget::setCheckedSearchState(bool busy, const QString &status, bool resumable) {
    checkedBusy_ = busy;
    recommend_->setText(busy        ? QStringLiteral("Отменить подбор")
                        : resumable ? QStringLiteral("Продолжить подбор")
                                    : QStringLiteral("Подобрать инструмент"));
    if (!status.isEmpty()) {
        recommendation_->setText(status);
        recommendation_->show();
    }
    recommend_->setEnabled(busy || (usesCatalog() && selectionRequest_.has_value()));
}
std::vector<core::ToolAssignment> ToolSelectionWidget::checkedCandidates() const {
    std::vector<core::ToolAssignment> candidates;
    if (!selectionRequest_)
        return candidates;
    const core::ToolCatalog workshop(catalog_.begin(), catalog_.begin() + workshopSize_);
    auto request = *selectionRequest_;
    request.maximumSegmentGapMm = segmentGap_->value();
    const auto result = core::selectTools(workshop, request);
    const auto current = selectedTool(core::ToolKind::Punch);
    for (const auto &c : result.candidates) {
        core::ToolAssignment a{workshop[c.punch],
                               workshop[c.die],
                               c.groove,
                               clearance_->value(),
                               machine_->text().toStdString(),
                               true};
        a.punchAssembly = c.punchAssembly ? std::optional{*c.punchAssembly} : std::nullopt;
        a.dieAssembly = c.dieAssembly ? std::optional{*c.dieAssembly} : std::nullopt;
        a.machineModel = demoMachine_->isChecked() ? machineModel_ : core::MachineModel::None;
        a.punchReversed = current && current->id == a.punch.id
                              ? punchReversed_
                              : core::defaultPunchOrientation(a.punch).value_or(false);
        if (!core::hasCompleteDrawingToolGeometry(a))
            continue;
        candidates.push_back(a);
        a.punchReversed = !a.punchReversed;
        candidates.push_back(std::move(a));
        if (candidates.size() >= 64)
            break;
    }
    return candidates;
}
void ToolSelectionWidget::suggestForNewPart() {
    auto candidates = checkedCandidates();
    if (!candidates.empty()) {
        auto a = std::move(candidates.front());
        a.punchReversed = core::defaultPunchOrientation(a.punch).value_or(false);
        setChoice(core::toolParameters(a), a);
        changed(Change::Pair);
        setCheckedSearchState(false, QStringLiteral("Предварительный подбор"));
        return;
    }
    updating_ = true;
    punch_->setCurrentIndex(0);
    die_->setCurrentIndex(0);
    populateGrooves();
    punchAssembly_.reset();
    dieAssembly_.reset();
    updating_ = false;
    changed();
    setCheckedSearchState(
        false,
        QStringLiteral("В библиотеке нет подходящей пары. Выберите или добавьте инструмент."));
}
void ToolSelectionWidget::recommend() {
    if (!selectionRequest_)
        return;
    const core::ToolCatalog workshop(catalog_.begin(), catalog_.begin() + workshopSize_);
    auto request = *selectionRequest_;
    request.maximumSegmentGapMm = segmentGap_->value();
    auto result = core::selectTools(workshop, request);
    std::erase_if(result.candidates, [&](const auto &c) {
        const auto &p = workshop[c.punch], &d = workshop[c.die];
        // A recommendation is a new choice: known drawings will be enabled regardless of
        // the previous checkbox. Do not suggest a pair that immediately becomes unusable.
        core::ToolAssignment candidate{
            p, d, c.groove, clearance_->value(), machine_->text().toStdString(), true};
        return !core::hasCompleteDrawingToolGeometry(candidate);
    });
    QString message;
    if (!result.validRequest) {
        message = QStringLiteral("Для подбора нужны толщина, угол, радиус и длина гиба.");
    } else if (result.candidates.empty()) {
        message =
            QStringLiteral("В библиотеке нет пары в рамках предварительного подбора (V/t ≥ 5, "
                           "поддержанные рабочие участки известных DXF). Это не доказывает "
                           "невозможность гибки. Записей с недостающими данными: %1.")
                .arg(result.missingData);
    } else {
        const auto &c = result.candidates.front();
        const auto &p = workshop[c.punch];
        const auto &d = workshop[c.die];
        const bool newPunch = selectedTool(core::ToolKind::Punch) != std::optional{p};
        mode_->setCurrentIndex(0);
        selectPair({text(p.id), text(d.id), static_cast<int>(c.groove)});
        punchAssembly_ = c.punchAssembly ? std::optional{*c.punchAssembly} : std::nullopt;
        dieAssembly_ = c.dieAssembly ? std::optional{*c.dieAssembly} : std::nullopt;
        chooseDrawingGeometry();
        if (newPunch)
            resetPunchOrientation();
        changed(Change::Pair);
        message = QStringLiteral("Предварительно: %1 / %2 · V %3 мм (%4 толщин). Вариантов: %5. "
                                 "Далее — проверка опор и пересечений.")
                      .arg(text(p.code), text(d.code))
                      .arg(d.openingsMm[c.groove])
                      .arg(c.openingThicknessRatio, 0, 'f', 1)
                      .arg(result.candidates.size());
        if (allBends_)
            message += QStringLiteral(" Общая пара для всех гибов; длина не менее %1 мм.")
                           .arg(request.bendLengthMm, 0, 'f', 2);
    }
    recommendation_->setText(message);
    if (result.segmentSearchLimited)
        recommendation_->setText(message +
                                 QStringLiteral(" Достигнут предел поиска секций; отсутствие "
                                                "варианта не означает невозможность сборки."));
    recommendation_->setToolTip(QStringLiteral(
        "Отбор по длине гиба, углу и радиусу носика. V ближе к 8t — ориентир ранжирования, не "
        "расчёт материала. Учебный инструмент и записи без подтверждённых параметров исключены. "
        "Материал, усилие, пружинение и крепления не проверены."));
    recommendation_->setMinimumHeight(recommendation_->heightForWidth(240));
    recommendation_->show();
}
void ToolSelectionWidget::fitSelectedAssemblies() {
    if (updating_ || !isEnabled() || !usesCatalog() || !selectionRequest_ || !preset(punch_) ||
        !preset(die_))
        return;
    const auto previousPunch = punchAssembly_, previousDie = dieAssembly_;
    const auto plans = fitAssemblies();
    QStringList issues;
    const std::array names{QStringLiteral("Пуансон"), QStringLiteral("Матрица")};
    for (std::size_t i = 0; i < plans.size(); ++i) {
        switch (plans[i].status) {
        case core::SegmentPlanStatus::Complete:
            break;
        case core::SegmentPlanStatus::InsufficientStock:
            if (const auto *tool = preset(i == 0 ? punch_ : die_); tool && !tool->segments)
                issues << names[i] + QStringLiteral(": цельная длина %1 мм меньше нужной.")
                                         .arg(tool->lengthMm);
            else
                issues << names[i] + QStringLiteral(": не хватает секций из указанного наличия.");
            break;
        case core::SegmentPlanStatus::SearchLimit:
            issues << names[i] + QStringLiteral(": достигнут предел поиска секций.");
            break;
        case core::SegmentPlanStatus::InvalidInput:
            issues << names[i] + QStringLiteral(": уточните данные секций и длину.");
            break;
        }
    }
    const double length = selectionRequest_->bendLengthMm;
    QString message;
    if (!issues.isEmpty()) {
        punchAssembly_ = previousPunch;
        dieAssembly_ = previousDie;
        updateDetails();
        message = QStringLiteral("Для длины гиба %1 мм: ").arg(length) +
                  issues.join(QLatin1Char(' ')) + QStringLiteral(" Прежний состав сохранён.");
    } else {
        const bool different = punchAssembly_ != previousPunch || dieAssembly_ != previousDie;
        if (different)
            changed();
        else
            updateDetails();
        message =
            allBends_
                ? QStringLiteral("Состав для всех включённых гибов · длина до %1 мм.").arg(length)
                : QStringLiteral("Состав для гиба длиной %1 мм.").arg(length);
        message += different ? QStringLiteral(" Инструмент и ручей сохранены.")
                             : QStringLiteral(" Состав уже подобран; изменений нет.");
    }
    recommendation_->setText(message);
    recommendation_->setToolTip(fitSections_->toolTip());
    recommendation_->setMinimumHeight(recommendation_->heightForWidth(240));
    recommendation_->show();
}
std::array<core::SegmentPlan, 2>
ToolSelectionWidget::fitAssemblies(std::optional<core::ToolKind> only) {
    if (!only || *only == core::ToolKind::Punch)
        punchAssembly_.reset();
    if (!only || *only == core::ToolKind::Die)
        dieAssembly_.reset();
    std::array<core::SegmentPlan, 2> plans;
    if (!selectionRequest_)
        return plans;
    const auto fit = [&](const core::ToolPreset *tool,
                         std::optional<core::ToolAssembly> &assembly) -> core::SegmentPlan {
        if (!tool)
            return {};
        if (!tool->segments)
            return {tool->lengthMm + 1e-5 >= selectionRequest_->bendLengthMm
                        ? core::SegmentPlanStatus::Complete
                        : core::SegmentPlanStatus::InsufficientStock,
                    {},
                    tool->lengthMm};
        auto plan = core::planToolSegments(*tool->segments, selectionRequest_->bendLengthMm, 200000,
                                           segmentGap_->value());
        if (plan.status == core::SegmentPlanStatus::Complete)
            assembly = plan.assembly;
        return plan;
    };
    if (!only || *only == core::ToolKind::Punch)
        plans[0] = fit(preset(punch_), punchAssembly_);
    if (!only || *only == core::ToolKind::Die)
        plans[1] = fit(preset(die_), dieAssembly_);
    return plans;
}
void ToolSelectionWidget::importCatalog() {
    if (!loadCatalog) {
        QMessageBox::information(this, QStringLiteral("Каталог"),
                                 QStringLiteral("Загрузка каталога недоступна в этом запуске."));
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Каталог мастерской"), {},
                                                   QStringLiteral("Каталог JSON (*.json)"));
    if (path.isEmpty())
        return;
#ifdef _WIN32
    const auto native = std::filesystem::path(path.toStdWString());
#else
    const auto native = std::filesystem::path(path.toUtf8().constData());
#endif
    const auto loaded = loadCatalog(native);
    if (const auto *error = std::get_if<std::string>(&loaded)) {
        QMessageBox::warning(this, QStringLiteral("Каталог не добавлен"), text(*error));
        return;
    }
    auto merged = catalog_;
    merged.resize(workshopSize_);
    std::size_t skipped = 0;
    for (const auto &tool : std::get<core::ToolCatalog>(loaded)) {
        if (!core::hasLibraryProfile(tool)) {
            ++skipped;
            continue;
        }
        auto found = std::find_if(merged.begin(), merged.end(),
                                  [&](const auto &t) { return t.id == tool.id; });
        if (found == merged.end())
            merged.push_back(tool);
        else
            *found = tool;
    }
    if (!core::validToolCatalog(merged)) {
        QMessageBox::warning(this, QStringLiteral("Каталог не добавлен"),
                             QStringLiteral("Слишком много инструментов или некорректные данные."));
        return;
    }
    if (!library_)
        return;
    if (const auto error = library_->replace(std::move(merged)); !error.empty()) {
        QMessageBox::warning(this, QStringLiteral("Каталог не добавлен"), text(error));
        return;
    }
    refreshWorkshop();
    if (skipped)
        recommendation_->setText(QStringLiteral("Пропущено записей без DXF: %1").arg(skipped));
}

void ToolSelectionWidget::setToolProfileImport(
    ImportController &controller, std::shared_ptr<const application::ToolProfileImporter> importer,
    application::ToolLibraryStorage writer, core::ToolCatalog starter) {
    controller_ = &controller;
    profileImporter_ = std::move(importer);
    libraryStorage_ = std::move(writer);
    const auto path =
        QDir(toolLibraryDirectory()).filePath(QStringLiteral("tools/tool-catalog.json"));
#ifdef _WIN32
    const auto native = std::filesystem::path(path.toStdWString());
#else
    const auto native = std::filesystem::path(path.toUtf8().constData());
#endif
    library_ = std::make_unique<application::ToolLibrary>(
        core::ToolCatalog(catalog_.begin(), catalog_.begin() + workshopSize_), native,
        libraryStorage_, std::move(starter));
    if (library_->data().tools !=
        core::ToolCatalog(catalog_.begin(), catalog_.begin() + workshopSize_))
        refreshWorkshop();
    stock_->setEnabled(static_cast<bool>(libraryStorage_));
    if (!library_->error().empty()) {
        recommendation_->setText(text(library_->error()));
        recommendation_->show();
    }
}
void ToolSelectionWidget::importDxf(std::optional<core::ToolKind> initialKind) {
    showLibrary(initialKind, true);
}
void ToolSelectionWidget::showLibrary(std::optional<core::ToolKind> kind, bool add) {
    if (!library_ || !controller_ || !libraryStorage_)
        return;
    const auto current = kind ? selectedTool(*kind) : selectedTool(core::ToolKind::Punch);
    ToolLibraryDialog dialog(
        *library_, *controller_, profileImporter_,
        [this](const auto &tool, auto groove) { return optionIssue(tool, groove); }, this, kind,
        current ? current->id : std::string{},
        current && current->kind == core::ToolKind::Die ? selectedGroove() : 0);
    dialog.onLibraryChanged = [this] { refreshWorkshop(); };
    dialog.onColorsChanged = [this] {
        if (onAppearanceChanged)
            onAppearanceChanged();
    };
    dialog.onImportCatalog = [this] { importCatalog(); };
    if (add)
        dialog.startImport();
    dialog.exec();
    if (dialog.choice())
        chooseTool(dialog.choice()->first, dialog.choice()->second);
}
void ToolSelectionWidget::refreshWorkshop() {
    if (!library_)
        return;
    const auto &active = library_->data().tools;
    const bool different = active.size() != workshopSize_ ||
                           !std::equal(active.begin(), active.end(), catalog_.begin());
    if (different && onCancelChecked)
        onCancelChecked();
    const auto punch = selectedTool(core::ToolKind::Punch), die = selectedTool(core::ToolKind::Die);
    const auto groove = selectedGroove();
    const core::ToolCatalog captured(catalog_.begin() + workshopSize_, catalog_.end());
    updating_ = true;
    catalog_ = library_->data().tools;
    workshopSize_ = catalog_.size();
    for (const auto &tool : captured)
        if (std::find(catalog_.begin(), catalog_.end(), tool) == catalog_.end())
            catalog_.push_back(tool);
    punch_->clear();
    die_->clear();
    punch_->addItem(QStringLiteral("Выберите пуансон…"), -1);
    die_->addItem(QStringLiteral("Выберите матрицу…"), -1);
    for (std::size_t i = 0; i < catalog_.size(); ++i)
        (catalog_[i].kind == core::ToolKind::Punch ? punch_ : die_)
            ->addItem(title(catalog_[i]) +
                          (i >= workshopSize_ ? QStringLiteral(" · снимок проекта") : QString{}),
                      static_cast<int>(i));
    if (punch)
        selectPreset(punch_, *punch);
    if (die)
        selectPreset(die_, *die);
    populateGrooves(groove);
    updating_ = false;
    auto settings = preferences();
    settings->setValue(QStringLiteral("tools/catalogPath"),
                       QStringLiteral("tools/tool-catalog.json"));
    settings->sync();
    refreshFavorites();
    updateDetails();
    // Deliberately no changed()/fitAssemblies(): editing the workshop is not an assignment.
}
} // namespace bending::ui
