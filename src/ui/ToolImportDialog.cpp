#include "ToolImportDialog.h"
#include "ToolDrawingWidget.h"
#include "bending/core/ToolGeometry.h"
#include "bending/ui/ImportController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>
namespace bending::ui {
namespace {
std::filesystem::path native(const QString &s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toUtf8().constData());
#endif
}
QLabel *label(const QString &s, QWidget *parent) {
    auto *l = new QLabel(s, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    return l;
}
QDoubleSpinBox *spin(QWidget *parent, const char *name, double max) {
    auto *s = new QDoubleSpinBox(parent);
    s->setObjectName(QLatin1String(name));
    s->setRange(0, max);
    s->setDecimals(4);
    s->setSpecialValueText(QStringLiteral("Не указано"));
    s->setKeyboardTracking(false);
    return s;
}
} // namespace
ToolImportDialog::ToolImportDialog(ImportController &controller,
                                   std::shared_ptr<const application::ToolProfileImporter> importer,
                                   core::ToolCatalog targets,
                                   std::function<QString(core::ToolPreset)> save, QWidget *parent,
                                   std::optional<core::ToolKind> initialKind)
    : QDialog(parent), controller_(controller), importer_(std::move(importer)),
      targets_(std::move(targets)), save_(std::move(save)) {
    setObjectName(QStringLiteral("toolImportDialog"));
    setWindowTitle(QStringLiteral("Добавить инструмент"));
    setAcceptDrops(true);
    resize(860, 640);
    setMinimumSize(660, 500);
    auto *root = new QVBoxLayout(this);
    chooseButton_ = new QPushButton(QStringLiteral("Выбрать DXF…"), this);
    chooseButton_->setObjectName(QStringLiteral("chooseToolDxf"));
    root->addWidget(chooseButton_);
    source_ = label(QStringLiteral("Подготовленное замкнутое сечение · DXF"), this);
    root->addWidget(source_);
    connect(chooseButton_, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, QStringLiteral("Сечение инструмента"), {},
                                         QStringLiteral("Текстовый DXF (*.dxf *.DXF)"));
        if (!path.isEmpty())
            loadFile(path);
    });
    auto *body = new QHBoxLayout;
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    form_ = new QWidget(scroll);
    auto *left = new QVBoxLayout(form_);
    left->setContentsMargins(0, 0, 8, 0);
    auto *basic = new QFormLayout;
    basic->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    kind_ = new QComboBox(form_);
    kind_->setObjectName(QStringLiteral("importToolKind"));
    kind_->addItems({QStringLiteral("Пуансон"), QStringLiteral("Матрица")});
    kind_->setCurrentIndex(initialKind == core::ToolKind::Die ? 1 : 0);
    kind_->setToolTip(QStringLiteral(
        "Тип определяется по рабочему профилю DXF. При необходимости измените его."));
    basic->addRow(QStringLiteral("Тип:"), kind_);
    code_ = new QLineEdit(form_);
    code_->setMaxLength(60);
    code_->setObjectName(QStringLiteral("importToolCode"));
    basic->addRow(QStringLiteral("Название:"), code_);
    length_ = spin(form_, "importToolLength", 100000);
    length_->setDecimals(2);
    length_->setSuffix(QStringLiteral(" мм"));
    length_->setToolTip(QStringLiteral(
        "Длина самого инструмента, например 835 или 415 мм. В поперечном сечении DXF она "
        "направлена вглубь изображения. Секции настраиваются в библиотеке."));
    basic->addRow(QStringLiteral("Длина инструмента:"), length_);
    keepSections_ =
        new QCheckBox(QStringLiteral("Использовать прежние секции этого инструмента"), form_);
    keepSections_->setObjectName("keepToolSections");
    keepSections_->setToolTip(
        QStringLiteral("Включайте, если новый DXF описывает тот же физический инструмент. Иначе "
                       "наличие будет не указано."));
    keepSections_->hide();
    basic->addRow(keepSections_);
    left->addLayout(basic);
    advancedToggle_ = new QToolButton(form_);
    advancedToggle_->setObjectName("toolImportAdvancedToggle");
    advancedToggle_->setText(QStringLiteral("Дополнительно"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setArrowType(Qt::RightArrow);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    left->addWidget(advancedToggle_);
    advanced_ = new QWidget(form_);
    advanced_->setObjectName("toolImportAdvanced");
    auto *form = new QFormLayout(advanced_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    target_ = new QComboBox(advanced_);
    target_->setObjectName(QStringLiteral("importToolTarget"));
    target_->addItem(QStringLiteral("Новый инструмент"));
    for (const auto &t : targets_)
        target_->addItem(QString::fromStdString(t.code));
    form->addRow(QStringLiteral("Запись библиотеки:"), target_);
    manufacturer_ = new QLineEdit(QStringLiteral("Мастерская"), advanced_);
    manufacturer_->setMaxLength(60);
    form->addRow(QStringLiteral("Производитель:"), manufacturer_);
    contour_ = new QComboBox(advanced_);
    contour_->setObjectName(QStringLiteral("importToolContour"));
    contour_->setPlaceholderText(QStringLiteral("Выберите контур"));
    form->addRow(QStringLiteral("Контур:"), contour_);
    units_ = new QComboBox(advanced_);
    units_->setObjectName(QStringLiteral("importToolUnits"));
    units_->addItem(QStringLiteral("Выберите единицы"), 0.);
    units_->addItem(QStringLiteral("Миллиметры"), 1.);
    units_->addItem(QStringLiteral("Дюймы"), 25.4);
    units_->addItem(QStringLiteral("Сантиметры"), 10.);
    units_->addItem(QStringLiteral("Метры"), 1000.);
    form->addRow(QStringLiteral("Единицы DXF:"), units_);
    rotation_ = spin(advanced_, "importToolRotation", 180);
    rotation_->setMinimum(-180);
    rotation_->setSpecialValueText({});
    form->addRow(QStringLiteral("Поворот, °:"), rotation_);
    grooves_ = new QTableWidget(0, 3, advanced_);
    grooves_->setObjectName(QStringLiteral("importToolGrooves"));
    grooves_->setAccessibleName(QStringLiteral("Измеренные рабочие положения матрицы"));
    grooves_->setHorizontalHeaderLabels(
        {QStringLiteral("V, мм"), QStringLiteral("Угол, °"), QStringLiteral("Поворот, °")});
    grooves_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    grooves_->verticalHeader()->hide();
    grooves_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grooves_->setSelectionBehavior(QAbstractItemView::SelectRows);
    grooves_->setSelectionMode(QAbstractItemView::SingleSelection);
    grooves_->setMinimumHeight(140);
    grooves_->setMaximumHeight(160);
    form->addRow(grooves_);
    notices_ = label({}, advanced_);
    notices_->setObjectName(QStringLiteral("toolImportNotices"));
    form->addRow(notices_);
    left->addWidget(advanced_);
    advanced_->hide();
    connect(advancedToggle_, &QToolButton::toggled, this, [this](bool expanded) {
        advanced_->setVisible(expanded);
        advancedToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    left->addStretch();
    scroll->setWidget(form_);
    body->addWidget(scroll, 1);
    auto *right = new QVBoxLayout;
    sizeLabel_ = label(QStringLiteral("Размеры появятся после загрузки DXF"), this);
    sizeLabel_->setObjectName("importToolProfileSize");
    sizeLabel_->setAlignment(Qt::AlignCenter);
    right->addWidget(sizeLabel_);
    previewHost_ = new QWidget(this);
    previewLayout_ = new QVBoxLayout(previewHost_);
    previewLayout_->setContentsMargins(0, 0, 0, 0);
    emptyPreview_ = label(QStringLiteral("Перетащите сюда DXF\nили выберите файл"), previewHost_);
    emptyPreview_->setAlignment(Qt::AlignCenter);
    previewLayout_->addWidget(emptyPreview_);
    right->addWidget(previewHost_, 1);
    grooveHost_ = new QWidget(this);
    grooveButtons_ = new QHBoxLayout(grooveHost_);
    grooveButtons_->setContentsMargins(0, 0, 0, 0);
    right->addWidget(grooveHost_);
    auto *controls = new QWidget(this);
    controls->setObjectName("toolImportPreviewControls");
    auto *transforms = new QHBoxLayout(controls);
    transforms->setContentsMargins(0, 0, 0, 0);
    for (const int degrees : {-90, 90}) {
        auto *button = new QToolButton(controls);
        button->setText(degrees < 0 ? QStringLiteral("−90°") : QStringLiteral("+90°"));
        button->setObjectName(degrees < 0 ? "rotateToolProfileLeft" : "rotateToolProfileRight");
        button->setToolTip(QStringLiteral("Повернуть сечение на %1°").arg(degrees));
        connect(button, &QToolButton::clicked, this, [this, degrees] {
            double angle = rotation_->value() + degrees;
            if (angle > 180)
                angle -= 360;
            if (angle < -180)
                angle += 360;
            rotation_->setValue(angle);
        });
        transforms->addWidget(button);
    }
    mirror_ = new QCheckBox(QStringLiteral("Отразить"), controls);
    mirror_->setObjectName(QStringLiteral("importToolMirror"));
    transforms->addWidget(mirror_);
    transforms->addStretch();
    right->addWidget(controls);
    body->addLayout(right, 1);
    root->addLayout(body, 1);
    status_ = label({}, this);
    status_->setObjectName(QStringLiteral("toolImportStatus"));
    root->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    saveButton_ = buttons->button(QDialogButtonBox::Save);
    saveButton_->setText(QStringLiteral("Добавить"));
    saveButton_->setObjectName(QStringLiteral("saveImportedTool"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ToolImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ToolImportDialog::reject);
    root->addWidget(buttons);
    connect(target_, &QComboBox::currentIndexChanged, this, [this] { selectTarget(); });
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] {
        kindChosen_ = true;
        updatePreview();
    });
    for (auto *c : {units_, contour_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
    connect(rotation_, &QDoubleSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(length_, &QDoubleSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(mirror_, &QCheckBox::toggled, this, [this] { updatePreview(); });
    connect(grooves_, &QTableWidget::currentCellChanged, this, [this] { updatePreview(); });
    for (auto *e : {code_, manufacturer_})
        connect(e, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    updatePreview();
}
void ToolImportDialog::setBusy(bool value) {
    busy_ = value;
    form_->setEnabled(!value);
    mirror_->parentWidget()->setEnabled(!value);
    grooveHost_->setEnabled(!value);
    chooseButton_->setEnabled(!value);
    saveButton_->setEnabled(!value && preset().has_value());
}
void ToolImportDialog::embed(std::optional<std::string> replaceId) {
    setWindowFlags(Qt::Widget);
    setMinimumSize(0, 0);
    if (replaceId) {
        for (std::size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].id == *replaceId) {
                target_->setCurrentIndex(static_cast<int>(i + 1));
                auto *before = new QWidget(this);
                auto *row = new QHBoxLayout(before);
                row->addWidget(label(QStringLiteral("Было: %1 · L %2 мм")
                                         .arg(QString::fromStdString(targets_[i].code))
                                         .arg(targets_[i].lengthMm),
                                     before));
                if (targets_[i].drawing) {
                    auto *old = new ToolDrawingWidget(*targets_[i].drawing, before, false);
                    old->setMinimumSize(100, 100);
                    old->setFixedHeight(110);
                    row->addWidget(old);
                }
                static_cast<QVBoxLayout *>(layout())->insertWidget(0, before);
                break;
            }
    }
    target_->setEnabled(false);
}
void ToolImportDialog::loadFile(const QString &path) {
    if (busy_)
        return;
    setBusy(true);
    status_->setText(QStringLiteral("Чтение сечения…"));
    QPointer<ToolImportDialog> self(this);
    if (!controller_.prepareToolProfile(
            native(path), importer_, [self, path](application::ToolProfileResult result) {
                if (!self)
                    return;
                self->setBusy(false);
                if (const auto *error = std::get_if<application::ToolProfileError>(&result)) {
                    // Keep the previous successfully captured file and preview on a failed
                    // replacement.
                    self->status_->setText(QString::fromStdString(error->message));
                    return;
                }
                if (!std::holds_alternative<application::ToolProfileFile>(result)) {
                    self->status_->setText(
                        QStringLiteral("Импортёр вернул неожиданный результат."));
                    return;
                }
                self->kindChosen_ = self->target_->currentIndex() > 0;
                self->file_ = std::get<application::ToolProfileFile>(std::move(result));
                self->source_->setText(QStringLiteral("Файл: %1 · контуров: %2")
                                           .arg(QFileInfo(path).fileName())
                                           .arg(self->file_->contours.size()));
                self->contour_->clear();
                for (std::size_t i = 0; i < self->file_->contours.size(); ++i)
                    self->contour_->addItem(
                        QStringLiteral("%1 · контур %2")
                            .arg(QString::fromUtf8(self->file_->contours[i].layer.c_str()))
                            .arg(i + 1));
                self->contour_->setCurrentIndex(self->file_->contours.size() == 1 ? 0 : -1);
                self->units_->setCurrentIndex(0);
                if (self->file_->millimetersPerUnit)
                    self->units_->setCurrentIndex(
                        self->units_->findData(*self->file_->millimetersPerUnit));
                QStringList notes;
                for (const auto &n : self->file_->notices)
                    notes << QString::fromStdString(n);
                self->notices_->setText(notes.join(QLatin1Char('\n')));
                self->notices_->setVisible(!notes.empty());
                if (self->units_->currentData().toDouble() <= 0 || self->file_->contours.size() > 1)
                    self->advancedToggle_->setChecked(true);
                if (self->target_->currentIndex() == 0 && self->code_->text().isEmpty())
                    self->code_->setText(QFileInfo(path).completeBaseName().left(60));
                self->updatePreview();
            })) {
        setBusy(false);
        status_->setText(
            QStringLiteral("Дождитесь завершения текущей операции и повторите загрузку."));
    }
}
std::optional<core::ToolDrawing> ToolImportDialog::drawing() const {
    if (!file_ || contour_->currentIndex() < 0 ||
        static_cast<std::size_t>(contour_->currentIndex()) >= file_->contours.size() ||
        units_->currentData().toDouble() <= 0)
        return {};
    auto d = file_->contours[static_cast<std::size_t>(contour_->currentIndex())].drawing;
    const double scale = units_->currentData().toDouble(),
                 radians = rotation_->value() * std::numbers::pi / 180;
    const auto transform = [&](core::DrawingPoint &p) {
        const double x = (mirror_->isChecked() ? -p.x : p.x) * scale, y = p.y * scale;
        p = {x * std::cos(radians) - y * std::sin(radians),
             x * std::sin(radians) + y * std::cos(radians)};
    };
    for (auto &e : d.edges) {
        transform(e.start);
        transform(e.end);
        if (e.center)
            transform(*e.center);
        if (mirror_->isChecked())
            e.sweepRadians = -e.sweepRadians;
    }
    d.millimeters = true;
    return core::validToolDrawing(d) ? std::optional{d} : std::nullopt;
}
std::optional<core::ToolPreset> ToolImportDialog::preset() const {
    const auto d = drawing();
    if (!d)
        return {};
    core::ToolPreset t;
    if (target_->currentIndex() > 0)
        t = targets_[static_cast<std::size_t>(target_->currentIndex() - 1)];
    if (!keepSections_->isChecked())
        t.segments.reset();
    t.code = code_->text().trimmed().toStdString();
    t.manufacturer = manufacturer_->text().trimmed().toStdString();
    t.kind = kind_->currentIndex() == 0 ? core::ToolKind::Punch : core::ToolKind::Die;
    t.lengthMm = length_->value();
    t.angleRadians = measuredPunch_             ? measuredPunch_->angleRadians
                     : !grooveSettings_.empty() ? grooveSettings_.front().angleRadians
                                                : 0;
    t.heightMm = measuredSize_ ? std::optional{measuredSize_->heightMm} : std::nullopt;
    t.teaching = false;
    t.drawing = *d;
    t.grooveSettings =
        kind_->currentIndex() == 1 ? grooveSettings_ : std::vector<core::DieGroove>{};
    t.noseRadiusMm.reset();
    t.openingsMm.clear();
    if (t.kind == core::ToolKind::Punch && measuredPunch_)
        t.noseRadiusMm = measuredPunch_->noseRadiusMm;
    if (t.kind == core::ToolKind::Die)
        t.openingsMm = measuredOpenings_;
    t.source = "Локальный DXF: " + d->sourceFile;
    t.parametersConfirmed =
        t.lengthMm >= 1 && t.angleRadians > 0 &&
        (t.kind == core::ToolKind::Punch ? t.noseRadiusMm.has_value() : !t.openingsMm.empty());
    // IDs are allocated only after the final validation, not on every preview update.
    if (t.id.empty())
        t.id = "draft-local-tool";
    return core::validToolPreset(t) ? std::optional{t} : std::nullopt;
}
void ToolImportDialog::selectTarget() {
    measuredDrawing_.reset();
    measuredKind_ = -1;
    keepSections_->setChecked(false);
    keepSections_->setVisible(target_->currentIndex() > 0);
    if (target_->currentIndex() <= 0) {
        kind_->setEnabled(true);
        setWindowTitle(QStringLiteral("Добавить инструмент"));
        saveButton_->setText(QStringLiteral("Добавить"));
        code_->clear();
        manufacturer_->setText(QStringLiteral("Мастерская"));
        length_->setValue(0);
    } else {
        const auto &t = targets_[static_cast<std::size_t>(target_->currentIndex() - 1)];
        kind_->setCurrentIndex(t.kind == core::ToolKind::Punch ? 0 : 1);
        kind_->setEnabled(false);
        setWindowTitle(QStringLiteral("Заменить сечение инструмента"));
        code_->setText(QString::fromStdString(t.code));
        manufacturer_->setText(QString::fromStdString(t.manufacturer));
        length_->setValue(t.lengthMm);
        saveButton_->setText(QStringLiteral("Сохранить"));
    }
    updatePreview();
}
void ToolImportDialog::showGrooves() {
    const QSignalBlocker block(grooves_);
    grooves_->setRowCount(static_cast<int>(grooveSettings_.size()));
    const auto previous = grooves_->currentRow();
    for (std::size_t i = 0; i < grooveSettings_.size(); ++i) {
        const auto row = static_cast<int>(i);
        grooves_->setItem(row, 0,
                          new QTableWidgetItem(QString::number(measuredOpenings_[i], 'g', 12)));
        grooves_->setItem(row, 1,
                          new QTableWidgetItem(QString::number(
                              grooveSettings_[i].angleRadians * 180 / std::numbers::pi, 'g', 8)));
        grooves_->setItem(
            row, 2,
            new QTableWidgetItem(QString::number(
                grooveSettings_[i].rotationRadians * 180 / std::numbers::pi, 'g', 8)));
    }
    if (!grooveSettings_.empty())
        grooves_->setCurrentCell(
            std::clamp(previous, 0, static_cast<int>(grooveSettings_.size()) - 1), 0);
}
void ToolImportDialog::measureProfile(const std::optional<core::ToolDrawing> &drawing) {
    if (measuredDrawing_ == drawing && measuredKind_ == kind_->currentIndex())
        return;
    if (drawing && !kindChosen_ && target_->currentIndex() == 0) {
        const bool punch = core::detectPunchProfile(*drawing).has_value();
        const bool die = !core::detectDieGrooves(*drawing).empty();
        if (punch != die) {
            const QSignalBlocker block(kind_);
            kind_->setCurrentIndex(die ? 1 : 0);
        }
    }
    measuredDrawing_ = drawing;
    measuredKind_ = kind_->currentIndex();
    measuredPunch_.reset();
    measuredSize_.reset();
    measuredOpenings_.clear();
    grooveSettings_.clear();
    if (drawing) {
        measuredSize_ = core::toolProfileSize(*drawing);
        if (measuredKind_ == 0)
            measuredPunch_ = core::detectPunchProfile(*drawing);
        else {
            const auto found = core::detectDieGrooves(*drawing);
            // The current catalogue requires unique V values. Do not invent a working side.
            const bool duplicate =
                std::adjacent_find(found.begin(), found.end(), [](const auto &a, const auto &b) {
                    return std::abs(a.openingMm - b.openingMm) < 1.e-5;
                }) != found.end();
            if (!duplicate)
                for (const auto &g : found) {
                    measuredOpenings_.push_back(g.openingMm);
                    grooveSettings_.push_back(g.settings);
                }
        }
    }
    while (auto *item = grooveButtons_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (std::size_t i = 0; i < measuredOpenings_.size(); ++i) {
        auto *button = new QToolButton(grooveHost_);
        button->setText(QStringLiteral("V %1").arg(measuredOpenings_[i], 0, 'g', 6));
        button->setObjectName(QStringLiteral("importToolGroove%1").arg(i));
        button->setCheckable(true);
        button->setStyleSheet(
            "QToolButton:checked{background:#E7F4F4;border:1px solid #087F85;border-radius:3px;}");
        connect(button, &QToolButton::clicked, this, [this, i] {
            grooves_->setCurrentCell(static_cast<int>(i), 0);
            updatePreview();
        });
        grooveButtons_->addWidget(button);
    }
    showGrooves();
}
void ToolImportDialog::updatePreview() {
    if (!saveButton_)
        return;
    auto d = drawing();
    measureProfile(d);
    const auto row = grooves_->currentRow();
    grooves_->setVisible(kind_->currentIndex() == 1 && !grooveSettings_.empty());
    grooveHost_->setVisible(kind_->currentIndex() == 1 && grooveSettings_.size() > 1);
    for (std::size_t i = 0; i < measuredOpenings_.size(); ++i)
        if (auto *button =
                grooveHost_->findChild<QToolButton *>(QStringLiteral("importToolGroove%1").arg(i)))
            button->setChecked(static_cast<int>(i) == row);
    if (d && kind_->currentIndex() == 1 && row >= 0 &&
        static_cast<std::size_t>(row) < grooveSettings_.size())
        d = core::rotateToolDrawing(*d,
                                    grooveSettings_[static_cast<std::size_t>(row)].rotationRadians);
    if (previewDrawing_ != d) {
        previewDrawing_ = d;
        previewSize_ = d ? core::toolProfileSize(*d) : std::nullopt;
        if (d) {
            if (!preview_) {
                preview_ = new ToolDrawingWidget(*d, previewHost_, false);
                previewLayout_->addWidget(preview_);
            } else
                preview_->setDrawing(*d);
            emptyPreview_->hide();
            preview_->show();
        } else {
            if (preview_)
                preview_->hide();
            emptyPreview_->show();
        }
    }
    QString measurements;
    if (measuredPunch_)
        measurements = QStringLiteral("Угол %1° · R %2 мм")
                           .arg(measuredPunch_->angleRadians * 180 / std::numbers::pi, 0, 'g', 6)
                           .arg(measuredPunch_->noseRadiusMm, 0, 'g', 6);
    else if (row >= 0 && static_cast<std::size_t>(row) < grooveSettings_.size())
        measurements = QStringLiteral("V %1 мм · угол %2°")
                           .arg(measuredOpenings_[static_cast<std::size_t>(row)], 0, 'g', 6)
                           .arg(grooveSettings_[static_cast<std::size_t>(row)].angleRadians * 180 /
                                    std::numbers::pi,
                                0, 'g', 6);
    if (previewSize_)
        measurements += (measurements.isEmpty() ? QString{} : QStringLiteral("\n")) +
                        QStringLiteral("Сечение %1 × %2 мм")
                            .arg(previewSize_->widthMm, 0, 'f', 3)
                            .arg(previewSize_->heightMm, 0, 'f', 3);
    sizeLabel_->setText(measurements.isEmpty() ? QStringLiteral("Размеры из DXF") : measurements);
    const auto t = preset();
    saveButton_->setEnabled(!busy_ && t.has_value());
    if (!busy_) {
        const bool measured = measuredPunch_.has_value() || !grooveSettings_.empty();
        status_->setText(
            !file_ ? QString{}
            : contour_->currentIndex() < 0 ? QStringLiteral("Выберите сечение из контуров DXF.")
            : units_->currentData().toDouble() <= 0 ? QStringLiteral("Укажите единицы DXF.")
            : !d ? QStringLiteral("Проверьте контур и единицы DXF.")
            : !t ? QStringLiteral("Укажите название инструмента.")
            : !measured ? QStringLiteral("Рабочий профиль не распознан. Поверните сечение или "
                                         "сохраните только для просмотра.")
            : length_->value() < 1
                ? QStringLiteral("Укажите длину инструмента для подбора и симуляции.")
                : QString{});
    }
}
namespace {
QString droppedDxf(const QMimeData *mime) {
    if (!mime || !mime->hasUrls() || mime->urls().size() != 1 ||
        !mime->urls().front().isLocalFile())
        return {};
    const auto path = mime->urls().front().toLocalFile();
    return QFileInfo(path).suffix().compare(QStringLiteral("dxf"), Qt::CaseInsensitive) == 0
               ? path
               : QString{};
}
} // namespace
void ToolImportDialog::dragEnterEvent(QDragEnterEvent *event) {
    if (!busy_ && !droppedDxf(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else
        event->ignore();
}
void ToolImportDialog::dragMoveEvent(QDragMoveEvent *event) {
    if (!busy_ && !droppedDxf(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else
        event->ignore();
}
void ToolImportDialog::dropEvent(QDropEvent *event) {
    const auto path = droppedDxf(event->mimeData());
    if (busy_ || path.isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    loadFile(path);
}
void ToolImportDialog::accept() {
    if (busy_)
        return;
    auto t = preset();
    if (!t)
        return;
    setBusy(true);
    status_->setText(QStringLiteral("Проверка замкнутого сечения…"));
    QPointer<ToolImportDialog> self(this);
    if (!controller_.prepareToolProfile(
            *t->drawing, importer_,
            [self, t = *t, newRecord = target_->currentIndex() == 0](
                application::ToolProfileResult result) mutable {
                if (!self)
                    return;
                self->setBusy(false);
                if (const auto *error = std::get_if<application::ToolProfileError>(&result)) {
                    self->status_->setText(QString::fromStdString(error->message));
                    return;
                }
                if (!std::holds_alternative<application::CheckedToolProfile>(result)) {
                    self->status_->setText(QStringLiteral("Проверка сечения не подтверждена."));
                    return;
                }
                if (newRecord)
                    t.id =
                        "local-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                const auto error = self->save_(std::move(t));
                if (!error.isEmpty()) {
                    self->status_->setText(error);
                    return;
                }
                self->QDialog::accept();
            })) {
        setBusy(false);
        status_->setText(
            QStringLiteral("Дождитесь завершения текущей операции и повторите сохранение."));
    }
}
void ToolImportDialog::reject() {
    if (busy_)
        controller_.cancelToolProfile();
    QDialog::reject();
}
} // namespace bending::ui
