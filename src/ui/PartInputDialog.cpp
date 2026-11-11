#include "PartInputDialog.h"
#include "MaterialPresets.h"
#include "bending/ui/ImportController.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace bending::ui {
namespace {
class ThicknessInput final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        auto result = QDoubleSpinBox::textFromValue(value);
        const auto point = result.indexOf(locale().decimalPoint());
        if (point >= 0)
            while (result.endsWith(QLatin1Char('0')) && result.size() > point + 3)
                result.chop(1);
        return result;
    }
};
QString mm(double v) {
    return QLocale().toString(v, 'f', 2);
}
QString factorText(double k) {
    return QLocale().toString(k, 'g', QLocale::FloatingPointShortest);
}
QString fileName(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.filename().native());
#else
    return QString::fromUtf8(path.filename().native());
#endif
}
} // namespace

QString partInputsSummary(const application::DocumentSession &session) {
    if (!session.document())
        return QStringLiteral("Материал и толщина появятся после загрузки.");
    const auto &settings = session.unfoldSettings();
    const auto thickness =
        session.manualThicknessMm()
            ? session.manualThicknessMm()
            : application::suggestThickness(session.document()->metadata().thickness);
    return QStringLiteral("%1 · t %2\n%3")
        .arg(settings.material == core::SheetMaterial::Unknown &&
                     settings.customMaterialName.empty()
                 ? QStringLiteral("Материал не указан")
                 : materialName(settings),
             thickness ? mm(*thickness) + QStringLiteral(" мм") : QStringLiteral("не определена"),
             settings.manualK ? QStringLiteral("Постоянный K = %1")
                                    .arg(QLocale().toString(*settings.manualK, 'g', 4))
                              : QStringLiteral("Справочный пресет · теоретический расчёт"));
}

PartInputDialog::PartInputDialog(ImportController &controller, QWidget *parent)
    : QDialog(parent), controller_(controller), document_(controller.session().document()),
      revision_(controller.session().revision()),
      editRevision_(controller.session().editRevision()),
      manual_(controller.session().manualThicknessMm()),
      automatic_(application::suggestThickness(document_->metadata().thickness)),
      settings_(controller.session().unfoldSettings()) {
    setObjectName(QStringLiteral("partInputDialog"));
    setWindowTitle(QStringLiteral("Исходные данные детали"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumWidth(460);
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    auto *name = new QLabel(fileName(document_->metadata().sourcePath), this);
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    layout->addWidget(name);
    auto *intro = new QLabel(QStringLiteral("Выберите материал и проверьте толщину."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    material_ = new QComboBox(this);
    material_->setObjectName(QStringLiteral("sheetMaterial"));
    material_->setAccessibleName(QStringLiteral("Материал листа"));
    auto presets = materialPresets();
    material_->addItem(QStringLiteral("Выберите материал…"));
    int selected = 0;
    for (const auto &preset : presets) {
        material_->addItem(preset.name);
        if (preset.material == settings_.material &&
            (preset.material != core::SheetMaterial::Unknown ||
             preset.name.toStdString() == settings_.customMaterialName))
            selected = material_->count() - 1;
    }
    if (!selected && !settings_.customMaterialName.empty()) {
        const auto categoryName = materialName(settings_);
        presets.push_back({categoryName, settings_.material, settings_.manualK, {}});
        material_->addItem(categoryName);
        selected = material_->count() - 1;
    }
    if (!selected && settings_.manualK)
        material_->setItemText(0, QStringLiteral("Без категории (из проекта)"));
    material_->setCurrentIndex(selected);
    form->addRow(QStringLiteral("Материал"), material_);
    auto *rule = new QHBoxLayout;
    factor_ = new QLineEdit(this);
    factor_->setObjectName(QStringLiteral("partUnfoldK"));
    factor_->setAccessibleName(QStringLiteral("Коэффициент K для этой детали"));
    rule->addWidget(factor_, 1);
    categoryFactorButton_ = new QPushButton(QStringLiteral("По категории"), this);
    categoryFactorButton_->setObjectName(QStringLiteral("partUseCategoryK"));
    categoryFactorButton_->setToolTip(QStringLiteral("Вернуть правило развёртки категории"));
    rule->addWidget(categoryFactorButton_);
    form->addRow(QStringLiteral("Коэффициент K"), rule);
    categoryRule_ = selectedMaterialRule();
    refreshFactor();
    thickness_ = new ThicknessInput(this);
    thickness_->setObjectName(QStringLiteral("partThickness"));
    thickness_->setAccessibleName(QStringLiteral("Толщина листа в миллиметрах"));
    thickness_->setDecimals(6);
    thickness_->setRange(0., 1.e6);
    thickness_->setSuffix(QStringLiteral(" мм"));
    thickness_->setSpecialValueText(QStringLiteral("Не определена"));
    thickness_->setValue(manual_.value_or(automatic_.value_or(0.)));
    form->addRow(QStringLiteral("Толщина"), thickness_);
    layout->addLayout(form);
    origin_ = new QLabel(this);
    origin_->setObjectName(QStringLiteral("partThicknessOrigin"));
    origin_->setWordWrap(true);
    layout->addWidget(origin_);
    auto *automatic = new QPushButton(QStringLiteral("Вернуть измеренную толщину"), this);
    automaticButton_ = automatic;
    automatic->setObjectName(QStringLiteral("partAutomaticThickness"));
    automatic->setVisible(automatic_.has_value());
    layout->addWidget(automatic);
    const auto &bends = document_->metadata().bendAnalysis.bends;
    QString summary = QStringLiteral("Распознано зон сгибов: %1").arg(bends.size());
    if (!bends.empty()) {
        const auto [low, high] =
            std::minmax_element(bends.begin(), bends.end(), [](const auto &a, const auto &b) {
                return a.lengthMm < b.lengthMm;
            });
        summary += QStringLiteral(" · L %1%2 мм")
                       .arg(mm(low->lengthMm), high->lengthMm == low->lengthMm
                                                   ? QString{}
                                                   : QStringLiteral("–") + mm(high->lengthMm));
    }
    auto *bendLabel = new QLabel(summary, this);
    bendLabel->setWordWrap(true);
    layout->addWidget(bendLabel);
    auto *toggle = new QToolButton(this);
    toggle->setObjectName(QStringLiteral("partInputAdvanced"));
    toggle->setText(QStringLiteral("Дополнительно ▸"));
    toggle->setCheckable(true);
    layout->addWidget(toggle);
    auto *advanced = new QWidget(this);
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    if (!bends.empty()) {
        auto *details = new QPlainTextEdit(advanced);
        details->setObjectName(QStringLiteral("partBendMeasurements"));
        details->setReadOnly(true);
        details->setMaximumHeight(110);
        QStringList lines;
        for (std::size_t i = 0; i < bends.size(); ++i) {
            const auto &b = bends[i];
            lines << QStringLiteral("B%1: L %2 мм · Ri %3 мм · внутренний угол %4°")
                         .arg(i + 1)
                         .arg(mm(b.lengthMm), mm(b.innerRadiusMm),
                              mm(180. - b.angleRadians * 180. / std::numbers::pi));
        }
        details->setPlainText(lines.join(QLatin1Char('\n')));
        advancedLayout->addWidget(details);
    }
    layout->addWidget(advanced);
    advanced->hide();
    toggle->setVisible(!bends.empty());
    connect(toggle, &QToolButton::toggled, advanced, &QWidget::setVisible);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("partInputError"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color:#9D2828;"));
    layout->addWidget(error_);
    auto *buttons = new QDialogButtonBox(this);
    continue_ = buttons->addButton(QStringLiteral("Продолжить"), QDialogButtonBox::AcceptRole);
    continue_->setObjectName(QStringLiteral("partInputContinue"));
    continue_->setDefault(true);
    auto *view =
        buttons->addButton(QStringLiteral("Только просмотр"), QDialogButtonBox::RejectRole);
    view->setObjectName(QStringLiteral("partInputViewOnly"));
    layout->addWidget(buttons);
    connect(view, &QPushButton::clicked, this, &QDialog::reject);
    connect(continue_, &QPushButton::clicked, this, [this] {
        applying_ = true;
        const bool applied =
            controller_.setPartInputs(revision_, editRevision_, manual_, settings_);
        applying_ = false;
        if (applied)
            accept();
        else
            reject(); // Replaced document or changed inputs; never apply a stale draft.
    });
    connect(material_, &QComboBox::currentIndexChanged, this, [this](int) { applyMaterialRule(); });
    connect(categoryFactorButton_, &QPushButton::clicked, this, [this] { applyMaterialRule(); });
    connect(factor_, &QLineEdit::textChanged, this, [this](const QString &value) {
        const auto input = value.trimmed();
        if (input.isEmpty()) {
            settings_.manualK = categoryRule_ ? categoryRule_->manualK : std::nullopt;
            validFactor_ = core::validUnfoldSettings(settings_);
        } else {
            bool ok = false;
            auto normalized = input;
            normalized.replace(QLocale().decimalPoint(), QStringLiteral("."));
            normalized.replace(QLatin1Char(','), QLatin1Char('.'));
            const double parsed = normalized.toDouble(&ok);
            validFactor_ = ok && std::isfinite(parsed) && parsed >= 0. && parsed <= 1.;
            if (validFactor_)
                settings_.manualK = parsed;
        }
        refreshDraft();
    });
    connect(thickness_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        manual_ = v;
        refreshDraft();
    });
    connect(automatic, &QPushButton::clicked, this, [this] {
        manual_.reset();
        const QSignalBlocker blocker(thickness_);
        thickness_->setValue(automatic_.value_or(0.));
        refreshDraft();
    });
    connect(&controller_, &ImportController::stateChanged, this, [this] {
        if (!applying_ && (controller_.hasWork() || controller_.session().revision() != revision_ ||
                           controller_.session().editRevision() != editRevision_))
            reject();
    });
    refreshDraft();
    material_->setFocus();
}
std::optional<core::UnfoldSettings> PartInputDialog::selectedMaterialRule() const {
    const auto name = material_->currentText();
    const auto presets = materialPresets();
    const auto row = material_->currentIndex() - 1;
    if (row >= 0 && static_cast<std::size_t>(row) < presets.size() &&
        presets[static_cast<std::size_t>(row)].name == name) {
        const auto &preset = presets[static_cast<std::size_t>(row)];
        return core::UnfoldSettings{
            preset.material, preset.k, 1,
            preset.material == core::SheetMaterial::Unknown ? name.toStdString() : ""};
    }
    return std::nullopt;
}
void PartInputDialog::applyMaterialRule() {
    categoryRule_ = selectedMaterialRule();
    if (categoryRule_) {
        settings_ = *categoryRule_;
    } else if (material_->currentText() == materialName(controller_.session().unfoldSettings())) {
        settings_ = controller_.session().unfoldSettings();
    } else if (material_->currentIndex() == 0) {
        const auto &saved = controller_.session().unfoldSettings();
        settings_ =
            saved.material == core::SheetMaterial::Unknown && saved.customMaterialName.empty()
                ? saved
                : core::UnfoldSettings{};
    }
    refreshFactor();
    refreshDraft();
}
void PartInputDialog::refreshFactor() {
    const QSignalBlocker blocker(factor_);
    validFactor_ = true;
    factor_->setEnabled(material_->currentIndex() > 0 || settings_.manualK.has_value());
    factor_->setText(settings_.manualK ? factorText(*settings_.manualK) : QString{});
}

void PartInputDialog::refreshDraft() {
    using namespace application;
    const auto &analysis = document_->metadata().thickness;
    const auto thickness = manual_ ? manual_ : automatic_;
    const auto validation =
        thickness ? validateThickness(*thickness, analysis) : ThicknessValidation::Invalid;
    QString origin;
    if (manual_) {
        origin = QStringLiteral("Задана вручную");
        if (automatic_ && analysis.status != ThicknessStatus::Detected &&
            std::abs(*manual_ - *automatic_) > analysis.toleranceMm)
            origin =
                QStringLiteral("Задана вручную · в модели измерено %1 мм").arg(mm(*automatic_));
    } else if (automatic_) {
        origin = analysis.status == ThicknessStatus::Detected
                     ? QStringLiteral("Из модели")
                     : QStringLiteral("По измерениям в модели");
    }
    origin_->setText(origin);
    origin_->setVisible(!origin.isEmpty());
    factor_->setPlaceholderText(settings_.manualK ? factorText(*settings_.manualK)
                                : settings_.material != core::SheetMaterial::Unknown
                                    ? QStringLiteral("По справочной таблице")
                                    : QString{});
    categoryFactorButton_->setEnabled(
        categoryRule_ && (!validFactor_ || settings_.manualK != categoryRule_->manualK));
    automaticButton_->setEnabled(manual_.has_value());
    QString error;
    if (validation == ThicknessValidation::ConflictsWithGeometry)
        error = QStringLiteral("Толщина отличается от модели (%1 мм). Проверьте значение.")
                    .arg(mm(*analysis.thicknessMm));
    else if (validation == ThicknessValidation::Invalid)
        error = QStringLiteral("Укажите толщину больше нуля, чтобы продолжить.");
    else if (!validFactor_)
        error = QStringLiteral("Введите коэффициент K от 0 до 1.");
    else if (settings_.material == core::SheetMaterial::Unknown && !settings_.manualK)
        error = QStringLiteral("Выберите материал, чтобы продолжить.");
    error_->setStyleSheet(validation == ThicknessValidation::ConflictsWithGeometry
                              ? QStringLiteral("color:#9D2828;")
                              : QStringLiteral("color:#526171;"));
    error_->setText(error);
    continue_->setEnabled(error.isEmpty());
}
} // namespace bending::ui
