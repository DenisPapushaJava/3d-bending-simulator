#include "ToolSelectionWidget.h"
#include "ToolDrawingWidget.h"
#include "ToolImportDialog.h"
#include "ToolProfilesWidget.h"
#include "ToolStockDialog.h"
#include "bending/core/ToolGeometry.h"
#include <QCheckBox>

#include "bending/ui/Preferences.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignTop);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("toolSourceMode"));
    mode_->setAccessibleName(QStringLiteral("Источник параметров инструмента"));
    mode_->addItems({QStringLiteral("Каталог инструмента"), QStringLiteral("Ручные параметры")});
    layout->addWidget(mode_);
    recommend_ = new QPushButton(QStringLiteral("Подобрать по детали"), this);
    recommend_->setObjectName(QStringLiteral("recommendTools"));
    layout->addWidget(recommend_);
    stock_ = new QPushButton(QStringLiteral("Секции и наличие…"), this);
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
    connect(stock_, &QPushButton::clicked, this, [this] {
        const auto previous = currentPair();
        const auto previousMode = mode_->currentIndex();
        ToolStockDialog dialog(
            core::ToolCatalog(catalog_.begin(), catalog_.begin() + workshopSize_),
            [this](core::ToolPreset tool) { return saveImportedTool(std::move(tool)); }, this);
        dialog.exec();
        updating_ = true;
        selectPair(previous);
        mode_->setCurrentIndex(previousMode);
        updating_ = false;
        fitAssemblies();
        changed();
    });
    recommendation_ = wrapped(this);
    recommendation_->setObjectName(QStringLiteral("toolRecommendation"));
    recommendation_->hide();
    layout->addWidget(recommendation_);
    connect(recommend_, &QPushButton::clicked, this, [this] { recommend(); });
    catalogPanel_ = new QWidget(this);
    auto *catalogLayout = new QVBoxLayout(catalogPanel_);
    catalogLayout->setContentsMargins(0, 0, 0, 0);
    importDxf_ = new QPushButton(QStringLiteral("Добавить инструмент из DXF…"), catalogPanel_);
    importDxf_->setObjectName(QStringLiteral("importToolDxf"));
    importDxf_->setEnabled(false);
    catalogLayout->addWidget(importDxf_);
    connect(importDxf_, &QPushButton::clicked, this, [this] { importDxf(); });
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
    auto *import = new QPushButton(QStringLiteral("Добавить каталог мастерской…"), catalogDetails_);
    import->setObjectName(QStringLiteral("importToolCatalog"));
    detailsLayout->addWidget(import);
    connect(import, &QPushButton::clicked, this, [this] { importCatalog(); });
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
    realGeometry_ = new QCheckBox(QStringLiteral("Использовать известные формы DXF"), this);
    realGeometry_->setObjectName(QStringLiteral("useDrawingToolGeometry"));
    realGeometry_->setToolTip(
        QStringLiteral("Для каждого инструмента используется его известный контур. Недостающая "
                       "форма отдельно отмечена как условная. При новом выборе формы DXF "
                       "включаются автоматически. Держатели и станок не моделируются."));
    layout->addWidget(realGeometry_);
    geometryScope_ = wrapped(this);
    geometryScope_->setObjectName(QStringLiteral("toolGeometryScope"));
    layout->addWidget(geometryScope_);
    connect(realGeometry_, &QCheckBox::toggled, this, [this] { changed(); });
    profiles_ = new ToolProfilesWidget(this);
    profiles_->setObjectName(QStringLiteral("toolProfilesPreview"));
    layout->addWidget(profiles_);
    enlargeProfiles_ = new QPushButton(QStringLiteral("Открыть сечения крупно…"), this);
    enlargeProfiles_->setObjectName(QStringLiteral("enlargeToolProfiles"));
    layout->addWidget(enlargeProfiles_);
    drawings_ = new QPushButton(QStringLiteral("Профили из чертежей…"), this);
    drawings_->setObjectName(QStringLiteral("showToolDrawings"));
    layout->addWidget(drawings_);
    connect(drawings_, &QPushButton::clicked, this, [this] { showDrawings(); });
    auto *profileScope = wrapped(this);
    profileScope->setText(QStringLiteral("Сечения выбранного инструмента · общий масштаб"));
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
        const auto tools = rawParameters();
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
        view->setDrawingAssignment(assignment());
        dialogLayout->addWidget(view, 1);
        auto *note = wrapped(&dialog);
        note->setText(
            assignment() && assignment()->useDrawingGeometry
                ? QStringLiteral("Полные сечения из DXF используются и в 3D-проверке. Масштаб "
                                 "одинаковый. Держатели, станок и нагрузка не моделируются.")
                : QStringLiteral("Условные рабочие сечения. Высота клина зависит от V; полные "
                                 "корпуса не показаны."));
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
    scope->setText(QStringLiteral("Источник формы каждого инструмента указан возле сечений. "
                                  "Держатели, станок и допустимая нагрузка не проверяются."));
    setupLayout->addWidget(scope);
    const auto selected = [this] {
        if (!updating_) {
            fitAssemblies();
            chooseDrawingGeometry();
        }
        changed();
    };
    connect(mode_, &QComboBox::currentIndexChanged, this, selected);
    connect(punch_, &QComboBox::currentIndexChanged, this, selected);
    connect(die_, &QComboBox::currentIndexChanged, this, [this] {
        populateGrooves();
        if (!updating_) {
            fitAssemblies();
            chooseDrawingGeometry();
        }
        changed();
    });
    connect(groove_, &QComboBox::currentIndexChanged, this, selected);
    for (auto *combo : {punch_, die_, groove_})
        connect(combo, &QComboBox::activated, this, [this] {
            if (!updating_ && chooseDrawingGeometry())
                changed();
        });
    connect(machine_, &QLineEdit::textEdited, this, [this] { changed(); });
    connect(favorites_, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && static_cast<std::size_t>(i - 1) < favoritePairs_.size()) {
            selectPair(favoritePairs_[static_cast<std::size_t>(i - 1)]);
            chooseDrawingGeometry();
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
    realGeometry_->setChecked(
        settings->value(QStringLiteral("tools/lastDrawingGeometry"), false).toBool());
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
std::optional<core::ToolAssignment> ToolSelectionWidget::assignment() const {
    const auto *p = preset(punch_);
    const auto *d = preset(die_);
    if (!usesCatalog() || !p || !d || groove_->currentIndex() < 0)
        return {};
    core::ToolAssignment a{*p,
                           *d,
                           static_cast<std::size_t>(groove_->currentIndex()),
                           fields_[5]->value(),
                           machine_->text().toStdString(),
                           realGeometry_->isChecked()};
    a.punchAssembly = punchAssembly_;
    a.dieAssembly = dieAssembly_;
    return core::toolParameters(a) ? std::optional{a} : std::nullopt;
}
std::optional<core::PressTools> ToolSelectionWidget::parameters() const {
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
QString ToolSelectionWidget::openingIssue() const {
    if (!selectionRequest_)
        return {};
    double v = fields_[0]->value();
    if (usesCatalog()) {
        const auto *d = preset(die_);
        const int g = groove_->currentIndex();
        if (!d || g < 0 || static_cast<std::size_t>(g) >= d->openingsMm.size())
            return {};
        v = d->openingsMm[static_cast<std::size_t>(g)];
    }
    return core::standardAirBendOpening(selectionRequest_->thicknessMm, v)
               ? QString{}
               : QStringLiteral("V %1 мм слишком мало для t %2 мм. Проверка заблокирована. "
                                "Ориентир 8t: %3 мм.")
                     .arg(v)
                     .arg(selectionRequest_->thicknessMm)
                     .arg(core::referenceDieOpeningMm(selectionRequest_->thicknessMm));
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
bool ToolSelectionWidget::chooseDrawingGeometry() {
    const auto *p = preset(punch_), *d = preset(die_);
    if (!usesCatalog() || !p || !d)
        return false;
    const bool known = p->drawing.has_value() || d->drawing.has_value();
    if (realGeometry_->isChecked() == known)
        return false;
    const QSignalBlocker block(realGeometry_);
    realGeometry_->setChecked(known);
    return true;
}
void ToolSelectionWidget::setChoice(const std::optional<core::PressTools> &tools,
                                    const std::optional<core::ToolAssignment> &a) {
    updating_ = true;
    if (a) {
        punchAssembly_ = a->punchAssembly;
        dieAssembly_ = a->dieAssembly;
        selectPreset(punch_, a->punch);
        selectPreset(die_, a->die);
        populateGrooves(a->groove);
        fields_[5]->setValue(a->clearanceMm);
        machine_->setText(text(a->machineName));
        realGeometry_->setChecked(a->useDrawingGeometry);
        mode_->setCurrentIndex(0);
    } else if (tools) {
        punchAssembly_.reset();
        dieAssembly_.reset();
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
        realGeometry_->setChecked(false);
        mode_->setCurrentIndex(1);
    } else {
        // New/unassigned bend: use workshop stock, never a previous project's captured stock.
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
    recommend_->setEnabled(usesCatalog() && selectionRequest_.has_value());
    const auto *selectedPunch = preset(punch_);
    const auto *selectedDie = preset(die_);
    drawings_->setVisible(usesCatalog() && ((selectedPunch && selectedPunch->drawing) ||
                                            (selectedDie && selectedDie->drawing)));
    std::optional<core::ToolAssignment> proposed;
    if (selectedPunch && selectedDie && groove_->currentIndex() >= 0)
        proposed = core::ToolAssignment{*selectedPunch, *selectedDie,
                                        static_cast<std::size_t>(groove_->currentIndex()),
                                        fields_[5]->value(), machine_->text().toStdString()};
    const bool ready = proposed && core::drawingToolGeometry(*proposed).has_value();
    realGeometry_->setVisible(usesCatalog());
    realGeometry_->setEnabled(usesCatalog() && (ready || realGeometry_->isChecked()));
    geometryScope_->setText(
        realGeometry_->isChecked() && !ready
            ? QStringLiteral("Рабочие участки DXF этой пары не распознаны. Выберите другую пару "
                             "или явно отключите форму DXF.")
        : realGeometry_->isChecked()
            ? QStringLiteral(
                  "По контуру DXF · рабочие точки распознаны. Пределы нагрузки ещё не проверяются.")
        : ready ? QStringLiteral("Доступна форма из DXF. Пока выбран условный профиль.")
        : (selectedPunch && selectedPunch->drawing) || (selectedDie && selectedDie->drawing)
            ? QStringLiteral("Условные формы. Рабочие участки загруженных DXF не подтверждены.")
            : QStringLiteral("Условные формы: контуры DXF не загружены."));
    if (ready) {
        const bool enabled = realGeometry_->isChecked();
        geometryScope_->setText(
            QStringLiteral("Пуансон: %1. Матрица: %2.%3")
                .arg(enabled && selectedPunch->drawing ? QStringLiteral("по DXF")
                                                       : QStringLiteral("условный"),
                     enabled && selectedDie->drawing ? QStringLiteral("по DXF")
                                                     : QStringLiteral("условная"),
                     enabled ? QStringLiteral(" Нагрузка не проверяется.")
                             : QStringLiteral(
                                   " Известные контуры DXF выключены для этого назначения.")));
    }
    geometryScope_->setVisible(usesCatalog());
    catalogPanel_->setVisible(usesCatalog());
    manualPanel_->setVisible(!usesCatalog());
    detailsToggle_->setVisible(usesCatalog());
    catalogDetails_->setVisible(usesCatalog() && detailsToggle_->isChecked());
    if (const auto issue = openingIssue(); !issue.isEmpty()) {
        geometryScope_->setText(issue);
        geometryScope_->setVisible(true);
    }
    const auto tools = rawParameters();
    profiles_->setTools(tools);
    profiles_->setDrawingAssignment(assignment());
    enlargeProfiles_->setEnabled(tools.has_value());
    const auto a = assignment();
    favorite_->setEnabled(a.has_value());
    favorite_->setText(std::find(favoritePairs_.begin(), favoritePairs_.end(), currentPair()) ==
                               favoritePairs_.end()
                           ? QStringLiteral("В избранное")
                           : QStringLiteral("Убрать из избранного"));
    if (!a) {
        QString missing;
        for (const auto *tool : {selectedPunch, selectedDie})
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
void ToolSelectionWidget::changed() {
    recommendation_->clear();
    recommendation_->hide();
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
    auto settings = preferences();
    settings->setValue(QStringLiteral("tools/lastPunch"), pair.punchId);
    settings->setValue(QStringLiteral("tools/lastDie"), pair.dieId);
    settings->setValue(QStringLiteral("tools/lastGroove"), pair.groove);
    settings->setValue(QStringLiteral("tools/lastDrawingGeometry"), realGeometry_->isChecked());
    settings->setValue(QStringLiteral("tools/machine"), machine_->text());
}
void ToolSelectionWidget::setSelectionRequest(std::optional<core::ToolSelectionRequest> request,
                                              bool allBends) {
    const bool different = selectionRequest_ != request || allBends_ != allBends;
    if (different) {
        allBends_ = allBends;
        selectionRequest_ = request;
        fitAssemblies();
        recommendation_->clear();
        recommendation_->hide();
        updateDetails();
    }
    recommend_->setEnabled(usesCatalog() && selectionRequest_.has_value());
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
        return (p.drawing || d.drawing) &&
               !core::drawingToolGeometry(
                   {p, d, c.groove, fields_[5]->value(), machine_->text().toStdString()});
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
        mode_->setCurrentIndex(0);
        selectPair({text(p.id), text(d.id), static_cast<int>(c.groove)});
        punchAssembly_ = c.punchAssembly ? std::optional{*c.punchAssembly} : std::nullopt;
        dieAssembly_ = c.dieAssembly ? std::optional{*c.dieAssembly} : std::nullopt;
        chooseDrawingGeometry();
        changed();
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
void ToolSelectionWidget::fitAssemblies() {
    punchAssembly_.reset();
    dieAssembly_.reset();
    if (!selectionRequest_)
        return;
    const auto fit = [&](const core::ToolPreset *tool,
                         std::optional<core::ToolAssembly> &assembly) {
        if (!tool || !tool->segments)
            return;
        const auto plan = core::planToolSegments(*tool->segments, selectionRequest_->bendLengthMm,
                                                 200000, segmentGap_->value());
        if (plan.status == core::SegmentPlanStatus::Complete)
            assembly = plan.assembly;
    };
    fit(preset(punch_), punchAssembly_);
    fit(preset(die_), dieAssembly_);
}
void ToolSelectionWidget::showDrawings() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("toolDrawingsDialog"));
    dialog.setWindowTitle(QStringLiteral("Профили из чертежей"));
    dialog.resize(740, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *row = new QHBoxLayout;
    for (const auto *tool : {preset(punch_), preset(die_)}) {
        if (!tool || !tool->drawing)
            continue;
        auto *column = new QVBoxLayout;
        auto *caption = wrapped(&dialog);
        caption->setText(title(*tool) + QLatin1Char('\n') + text(tool->drawing->sourceFile));
        column->addWidget(caption);
        column->addWidget(new ToolDrawingWidget(*tool->drawing, &dialog), 1);
        row->addLayout(column, 1);
    }
    layout->addLayout(row, 1);
    auto *scope = wrapped(&dialog);
    scope->setText(QStringLiteral("Исходные контуры из локальных файлов. Каждый вписан в своё "
                                  "окно; масштаб окон может отличаться. Выбранный режим формы "
                                  "каждого инструмента указан в основной панели. Крепления и "
                                  "допустимая нагрузка не проверяются."));
    scope->setMinimumHeight(scope->heightForWidth(650));
    layout->addWidget(scope);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
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
    for (const auto &tool : std::get<core::ToolCatalog>(loaded)) {
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
    const auto pair = currentPair();
    updating_ = true;
    catalog_ = std::move(merged);
    workshopSize_ = catalog_.size();
    punch_->clear();
    die_->clear();
    punch_->addItem(QStringLiteral("Выберите пуансон…"), -1);
    die_->addItem(QStringLiteral("Выберите матрицу…"), -1);
    for (std::size_t i = 0; i < catalog_.size(); ++i)
        (catalog_[i].kind == core::ToolKind::Punch ? punch_ : die_)
            ->addItem(title(catalog_[i]), static_cast<int>(i));
    selectPair(pair);
    updating_ = false;
    auto settings = preferences();
    settings->setValue(QStringLiteral("tools/catalogPath"), path);
    refreshFavorites();
    changed();
}
void ToolSelectionWidget::setToolProfileImport(
    ImportController &controller, std::shared_ptr<const application::ToolProfileImporter> importer,
    application::ToolLibraryWriter writer) {
    controller_ = &controller;
    profileImporter_ = std::move(importer);
    libraryWriter_ = std::move(writer);
    importDxf_->setEnabled(profileImporter_ && libraryWriter_);
    stock_->setEnabled(static_cast<bool>(libraryWriter_));
}
void ToolSelectionWidget::importDxf() {
    if (!controller_ || !profileImporter_ || !libraryWriter_)
        return;
    ToolImportDialog dialog(
        *controller_, profileImporter_,
        core::ToolCatalog(catalog_.begin(), catalog_.begin() + workshopSize_),
        [this](core::ToolPreset tool) { return saveImportedTool(std::move(tool)); }, this);
    dialog.exec();
}
QString ToolSelectionWidget::saveImportedTool(core::ToolPreset tool) {
    auto merged = core::ToolCatalog(catalog_.begin(), catalog_.begin() + workshopSize_);
    const auto found =
        std::find_if(merged.begin(), merged.end(), [&](const auto &t) { return t.id == tool.id; });
    if (found == merged.end())
        merged.push_back(tool);
    else
        *found = tool;
    if (!core::validToolCatalog(merged))
        return QStringLiteral("Каталог переполнен или данные инструмента некорректны.");
    auto settings = preferences();
    // A separate writable copy beside settings: portable data/ on Windows, user config on Linux.
    const auto folder = QDir(toolLibraryDirectory());
    if (!QDir().mkpath(folder.filePath(QStringLiteral("tools"))))
        return QStringLiteral("Не удалось создать папку локальной библиотеки.");
    const auto path = folder.filePath(QStringLiteral("tools/tool-catalog.json"));
#ifdef _WIN32
    const auto native = std::filesystem::path(path.toStdWString());
#else
    const auto native = std::filesystem::path(path.toUtf8().constData());
#endif
    if (const auto error = libraryWriter_(native, merged))
        return text(*error);
    const core::ToolCatalog projectOnly(catalog_.begin() + workshopSize_, catalog_.end());
    auto pair = currentPair();
    if (tool.kind == core::ToolKind::Punch)
        pair.punchId = text(tool.id);
    else {
        pair.dieId = text(tool.id);
        pair.groove = 0;
    }
    updating_ = true;
    catalog_ = std::move(merged);
    workshopSize_ = catalog_.size();
    for (const auto &entry : projectOnly)
        if (std::none_of(catalog_.begin(), catalog_.end(),
                         [&](const auto &t) { return t.id == entry.id; }))
            catalog_.push_back(entry);
    punch_->clear();
    die_->clear();
    punch_->addItem(QStringLiteral("Выберите пуансон…"), -1);
    die_->addItem(QStringLiteral("Выберите матрицу…"), -1);
    for (std::size_t i = 0; i < catalog_.size(); ++i)
        (catalog_[i].kind == core::ToolKind::Punch ? punch_ : die_)
            ->addItem(title(catalog_[i]), static_cast<int>(i));
    selectPair(pair);
    mode_->setCurrentIndex(0);
    updating_ = false;
    chooseDrawingGeometry();
    settings->setValue(QStringLiteral("tools/catalogPath"),
                       QStringLiteral("tools/tool-catalog.json"));
    settings->sync();
    refreshFavorites();
    fitAssemblies();
    changed();
    return {};
}
} // namespace bending::ui
