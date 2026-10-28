#include "PartInputDialog.h"
#include "bending/ui/ImportController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
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
QString materialName(core::SheetMaterial material) {
    switch (material) {
    case core::SheetMaterial::MildSteel:
        return QStringLiteral("Низкоуглеродистая сталь");
    case core::SheetMaterial::StainlessSteel:
        return QStringLiteral("Нержавеющая сталь");
    case core::SheetMaterial::Aluminium:
        return QStringLiteral("Алюминий");
    default:
        return QStringLiteral("Материал не указан");
    }
}
QString mm(double v) {
    return QLocale().toString(v, 'f', 2);
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
        .arg(materialName(settings.material),
             thickness ? mm(*thickness) + QStringLiteral(" мм") : QStringLiteral("не определена"),
             settings.manualK ? QStringLiteral("Ручной K = %1").arg(*settings.manualK, 0, 'f', 4)
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
    auto *intro = new QLabel(QStringLiteral("Выберите материал и проверьте толщину.\nСправочные "
                                            "пресеты · теоретический расчёт развёртки."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    material_ = new QComboBox(this);
    material_->setObjectName(QStringLiteral("sheetMaterial"));
    material_->setAccessibleName(QStringLiteral("Материал листа"));
    material_->addItems({QStringLiteral("Выберите материал…"),
                         materialName(core::SheetMaterial::MildSteel),
                         materialName(core::SheetMaterial::StainlessSteel),
                         materialName(core::SheetMaterial::Aluminium)});
    material_->setCurrentIndex(static_cast<int>(settings_.material));
    form->addRow(QStringLiteral("Материал"), material_);
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
    auto *rule = new QLabel(
        QStringLiteral(
            "Справочный пресет, v1 · Wiz Parts (2024).\nBA = β · (Ri + K · t). K зависит от "
            "Ri/t.\nЭто теоретическая оценка; поправки по образцам пока не заданы."),
        advanced);
    rule->setWordWrap(true);
    advancedLayout->addWidget(rule);
    auto *kForm = new QFormLayout;
    customK_ = new QCheckBox(QStringLiteral("Задать K вручную"), advanced);
    customK_->setObjectName(QStringLiteral("customUnfoldK"));
    customK_->setChecked(settings_.manualK.has_value());
    k_ = new QDoubleSpinBox(advanced);
    k_->setObjectName(QStringLiteral("unfoldK"));
    k_->setAccessibleName(QStringLiteral("Ручной коэффициент K"));
    k_->setRange(0., .5);
    k_->setDecimals(4);
    k_->setSingleStep(.01);
    k_->setValue(settings_.manualK.value_or(.4));
    kForm->addRow(customK_, k_);
    advancedLayout->addLayout(kForm);
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
    advanced->setVisible(settings_.manualK.has_value());
    toggle->setChecked(settings_.manualK.has_value());
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
    connect(material_, &QComboBox::currentIndexChanged, this, [this](int index) {
        settings_.material = static_cast<core::SheetMaterial>(index);
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
    const auto updateK = [this] {
        settings_.manualK = customK_->isChecked() ? std::optional{k_->value()} : std::nullopt;
        refreshDraft();
    };
    connect(customK_, &QCheckBox::toggled, this, updateK);
    connect(k_, &QDoubleSpinBox::valueChanged, this, updateK);
    connect(&controller_, &ImportController::stateChanged, this, [this] {
        if (!applying_ && (controller_.hasWork() || controller_.session().revision() != revision_ ||
                           controller_.session().editRevision() != editRevision_))
            reject();
    });
    refreshDraft();
    material_->setFocus();
}
void PartInputDialog::refreshDraft() {
    using namespace application;
    const auto &analysis = document_->metadata().thickness;
    const auto thickness = manual_ ? manual_ : automatic_;
    const auto validation =
        thickness ? validateThickness(*thickness, analysis) : ThicknessValidation::Invalid;
    origin_->setText(manual_ ? QStringLiteral("Ручное значение. CAD-геометрия не изменяется.")
                     : analysis.status == ThicknessStatus::Detected && automatic_
                         ? QStringLiteral("Из геометрии · постоянная толщина подтверждена.")
                     : automatic_
                         ? QStringLiteral("Предварительное локальное измерение · постоянство "
                                          "толщины всей детали не подтверждено.")
                         : QStringLiteral("Достоверное значение не найдено. Укажите толщину "
                                          "вручную или перейдите к просмотру."));
    if (manual_ && automatic_ && analysis.status != ThicknessStatus::Detected &&
        std::abs(*manual_ - *automatic_) > analysis.toleranceMm)
        origin_->setText(QStringLiteral("Ручное значение отличается от предварительного измерения "
                                        "(%1 мм). CAD-геометрия не изменяется.")
                             .arg(mm(*automatic_)));
    k_->setEnabled(customK_->isChecked());
    automaticButton_->setEnabled(manual_.has_value());
    QString error;
    if (validation == ThicknessValidation::ConflictsWithGeometry)
        error =
            QStringLiteral(
                "Толщина отличается от CAD (%1 мм). Проверьте значение; модель не масштабируется.")
                .arg(mm(*analysis.thicknessMm));
    else if (validation == ThicknessValidation::Invalid)
        error = QStringLiteral("Укажите толщину больше нуля, чтобы продолжить.");
    else if (settings_.material == core::SheetMaterial::Unknown && !settings_.manualK)
        error = QStringLiteral("Выберите материал, чтобы продолжить.");
    error_->setStyleSheet(validation == ThicknessValidation::ConflictsWithGeometry
                              ? QStringLiteral("color:#9D2828;")
                              : QStringLiteral("color:#526171;"));
    error_->setText(error);
    continue_->setEnabled(error.isEmpty());
}
} // namespace bending::ui
