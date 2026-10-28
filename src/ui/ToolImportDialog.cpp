#include "ToolImportDialog.h"
#include "ToolDrawingWidget.h"
#include "bending/core/ToolGeometry.h"
#include "bending/ui/ImportController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
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
                                   std::function<QString(core::ToolPreset)> save, QWidget *parent)
    : QDialog(parent), controller_(controller), importer_(std::move(importer)),
      targets_(std::move(targets)), save_(std::move(save)) {
    setObjectName(QStringLiteral("toolImportDialog"));
    setWindowTitle(QStringLiteral("Инструмент из DXF"));
    resize(880, 720);
    setMinimumSize(660, 540);
    auto *root = new QVBoxLayout(this);
    chooseButton_ = new QPushButton(QStringLiteral("Выбрать DXF…"), this);
    chooseButton_->setObjectName(QStringLiteral("chooseToolDxf"));
    root->addWidget(chooseButton_);
    source_ = label(QStringLiteral("Файл DXF ещё не выбран."), this);
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
    auto *form = new QFormLayout(form_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    target_ = new QComboBox(form_);
    target_->setObjectName(QStringLiteral("importToolTarget"));
    target_->addItem(QStringLiteral("Новый инструмент"));
    for (const auto &t : targets_)
        target_->addItem(QString::fromStdString(t.code));
    form->addRow(QStringLiteral("Запись библиотеки:"), target_);
    kind_ = new QComboBox(form_);
    kind_->setObjectName(QStringLiteral("importToolKind"));
    kind_->addItems({QStringLiteral("Пуансон"), QStringLiteral("Матрица")});
    form->addRow(QStringLiteral("Тип:"), kind_);
    code_ = new QLineEdit(form_);
    code_->setMaxLength(60);
    code_->setObjectName(QStringLiteral("importToolCode"));
    form->addRow(QStringLiteral("Название / артикул:"), code_);
    manufacturer_ = new QLineEdit(QStringLiteral("Мастерская"), form_);
    manufacturer_->setMaxLength(60);
    form->addRow(QStringLiteral("Производитель:"), manufacturer_);
    contour_ = new QComboBox(form_);
    contour_->setObjectName(QStringLiteral("importToolContour"));
    form->addRow(QStringLiteral("Слой / контур:"), contour_);
    units_ = new QComboBox(form_);
    units_->setObjectName(QStringLiteral("importToolUnits"));
    units_->addItem(QStringLiteral("Выберите единицы"), 0.);
    units_->addItem(QStringLiteral("Миллиметры"), 1.);
    units_->addItem(QStringLiteral("Дюймы"), 25.4);
    units_->addItem(QStringLiteral("Сантиметры"), 10.);
    units_->addItem(QStringLiteral("Метры"), 1000.);
    form->addRow(QStringLiteral("Единицы DXF:"), units_);
    rotation_ = spin(form_, "importToolRotation", 180);
    rotation_->setMinimum(-180);
    rotation_->setSpecialValueText({});
    form->addRow(QStringLiteral("Поворот, °:"), rotation_);
    mirror_ = new QCheckBox(QStringLiteral("Отразить слева направо"), form_);
    mirror_->setObjectName(QStringLiteral("importToolMirror"));
    form->addRow(mirror_);
    form->addRow(label(QStringLiteral("Пуансон — носиком вниз, матрица — ручьём вверх. Выберите "
                                      "один сплошной контур без внутренних полостей."),
                       form_));
    length_ = spin(form_, "importToolLength", 100000);
    form->addRow(QStringLiteral("Длина вдоль гиба, мм:"), length_);
    angle_ = spin(form_, "importToolAngle", 120);
    form->addRow(QStringLiteral("Рабочий угол, °:"), angle_);
    radius_ = spin(form_, "importToolRadius", 250);
    form->addRow(QStringLiteral("Радиус носика, мм:"), radius_);
    openings_ = new QLineEdit(form_);
    openings_->setObjectName(QStringLiteral("importToolOpenings"));
    openings_->setMaxLength(256);
    openings_->setPlaceholderText(QStringLiteral("Например: 12 или 10; 16; 22; 35"));
    form->addRow(QStringLiteral("Раскрытие V, мм:"), openings_);
    recognizeButton_ = new QPushButton(QStringLiteral("Распознать ручьи по контуру"), form_);
    recognizeButton_->setObjectName(QStringLiteral("recognizeToolGrooves"));
    form->addRow(recognizeButton_);
    grooves_ = new QTableWidget(0, 3, form_);
    grooves_->setObjectName(QStringLiteral("importToolGrooves"));
    grooves_->setAccessibleName(
        QStringLiteral("Рабочие положения матрицы. Выберите ручей для просмотра."));
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
    form->addRow(label(
        QStringLiteral("Неизвестные длину и параметры можно оставить пустыми: запись сохранится "
                       "для просмотра, без подбора. Форма не определяет допустимую нагрузку."),
        form_));
    scroll->setWidget(form_);
    body->addWidget(scroll, 1);
    previewHost_ = new QWidget(this);
    previewLayout_ = new QVBoxLayout(previewHost_);
    previewLayout_->setContentsMargins(0, 0, 0, 0);
    previewLayout_->addWidget(
        label(QStringLiteral("Загрузите DXF и выберите замкнутый контур."), previewHost_));
    body->addWidget(previewHost_, 1);
    root->addLayout(body, 1);
    notices_ = label({}, this);
    notices_->setObjectName(QStringLiteral("toolImportNotices"));
    notices_->setMaximumHeight(100);
    auto *noticeScroll = new QScrollArea(this);
    noticeScroll->setWidgetResizable(true);
    noticeScroll->setMaximumHeight(100);
    noticeScroll->setWidget(notices_);
    root->addWidget(noticeScroll);
    status_ = label(QStringLiteral("Поддерживаются LINE, ARC, CIRCLE и простые 2D "
                                   "POLYLINE/LWPOLYLINE. Размеры и подписи не входят в сечение."),
                    this);
    status_->setObjectName(QStringLiteral("toolImportStatus"));
    root->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    saveButton_ = buttons->button(QDialogButtonBox::Save);
    saveButton_->setText(QStringLiteral("Добавить в библиотеку"));
    saveButton_->setObjectName(QStringLiteral("saveImportedTool"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ToolImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ToolImportDialog::reject);
    root->addWidget(buttons);
    connect(target_, &QComboBox::currentIndexChanged, this, [this] { selectTarget(); });
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] {
        clearGrooves();
        updatePreview();
    });
    for (auto *c : {units_, contour_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] {
            clearGrooves();
            updatePreview();
        });
    connect(rotation_, &QDoubleSpinBox::valueChanged, this, [this] {
        clearGrooves();
        updatePreview();
    });
    for (auto *s : {length_, angle_, radius_})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { updatePreview(); });
    connect(mirror_, &QCheckBox::toggled, this, [this] {
        clearGrooves();
        updatePreview();
    });
    connect(recognizeButton_, &QPushButton::clicked, this, [this] { recognizeGrooves(); });
    connect(grooves_, &QTableWidget::currentCellChanged, this, [this] { updatePreview(); });
    for (auto *e : {code_, manufacturer_, openings_})
        connect(e, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    updatePreview();
}
void ToolImportDialog::setBusy(bool value) {
    busy_ = value;
    form_->setEnabled(!value);
    chooseButton_->setEnabled(!value);
    saveButton_->setEnabled(!value && preset().has_value());
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
                self->clearGrooves();
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
                self->units_->setCurrentIndex(0);
                if (self->file_->millimetersPerUnit)
                    self->units_->setCurrentIndex(
                        self->units_->findData(*self->file_->millimetersPerUnit));
                QStringList notes;
                for (const auto &n : self->file_->notices)
                    notes << QString::fromStdString(n);
                self->notices_->setText(notes.join(QLatin1Char('\n')));
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
    t.code = code_->text().trimmed().toStdString();
    t.manufacturer = manufacturer_->text().trimmed().toStdString();
    t.kind = kind_->currentIndex() == 0 ? core::ToolKind::Punch : core::ToolKind::Die;
    t.lengthMm = length_->value();
    t.angleRadians = angle_->value() * std::numbers::pi / 180;
    t.drawing = *d;
    t.grooveSettings =
        kind_->currentIndex() == 1 ? grooveSettings_ : std::vector<core::DieGroove>{};
    t.noseRadiusMm.reset();
    t.openingsMm.clear();
    if (t.kind == core::ToolKind::Punch && radius_->value() > 0)
        t.noseRadiusMm = radius_->value();
    if (t.kind == core::ToolKind::Die) {
        const auto tokens = openings_->text().trimmed().split(
            QRegularExpression(QStringLiteral("[;\\s]+")), Qt::SkipEmptyParts);
        for (auto token : tokens) {
            bool ok = false;
            token.replace(QLatin1Char(','), QLatin1Char('.'));
            const double v = token.toDouble(&ok);
            if (!ok || !std::isfinite(v))
                return {};
            t.openingsMm.push_back(v);
        }
    }
    t.source = "Локальный DXF: " + d->sourceFile;
    t.parametersConfirmed =
        t.lengthMm >= 1 && angle_->value() > 0 &&
        (t.kind == core::ToolKind::Punch ? t.noseRadiusMm.has_value() : !t.openingsMm.empty());
    // IDs are allocated only after the final validation, not on every preview update.
    if (t.id.empty())
        t.id = "draft-local-tool";
    return core::validToolPreset(t) ? std::optional{t} : std::nullopt;
}
void ToolImportDialog::selectTarget() {
    clearGrooves();
    if (target_->currentIndex() <= 0) {
        kind_->setEnabled(true);
        saveButton_->setText(QStringLiteral("Добавить в библиотеку"));
        code_->clear();
        manufacturer_->setText(QStringLiteral("Мастерская"));
        length_->setValue(0);
        angle_->setValue(0);
        radius_->setValue(0);
        openings_->clear();
    } else {
        const auto &t = targets_[static_cast<std::size_t>(target_->currentIndex() - 1)];
        kind_->setCurrentIndex(t.kind == core::ToolKind::Punch ? 0 : 1);
        kind_->setEnabled(false);
        code_->setText(QString::fromStdString(t.code));
        manufacturer_->setText(QString::fromStdString(t.manufacturer));
        length_->setValue(t.lengthMm);
        angle_->setValue(t.angleRadians * 180 / std::numbers::pi);
        radius_->setValue(t.noseRadiusMm.value_or(0));
        QStringList values;
        for (double v : t.openingsMm)
            values << QString::number(v, 'g', 12);
        openings_->setText(values.join(QStringLiteral("; ")));
        // Saved rotations belong to that captured section, not an unrelated replacement DXF.
        if (!file_ || t.drawing == drawing())
            grooveSettings_ = t.grooveSettings;
        showGrooves();
        saveButton_->setText(QStringLiteral("Сохранить сечение инструмента"));
    }
    updatePreview();
}
void ToolImportDialog::clearGrooves() {
    grooveSettings_.clear();
    showGrooves();
}
void ToolImportDialog::showGrooves() {
    const QSignalBlocker block(grooves_);
    grooves_->setRowCount(static_cast<int>(grooveSettings_.size()));
    const auto values = openings_->text().split(QLatin1Char(';'));
    for (std::size_t i = 0; i < grooveSettings_.size(); ++i) {
        const auto row = static_cast<int>(i);
        grooves_->setItem(row, 0, new QTableWidgetItem(values.value(row).trimmed()));
        grooves_->setItem(row, 1,
                          new QTableWidgetItem(QString::number(
                              grooveSettings_[i].angleRadians * 180 / std::numbers::pi, 'g', 8)));
        grooves_->setItem(
            row, 2,
            new QTableWidgetItem(QString::number(
                grooveSettings_[i].rotationRadians * 180 / std::numbers::pi, 'g', 8)));
    }
    if (!grooveSettings_.empty())
        grooves_->setCurrentCell(0, 0);
    recognizeButton_->setText(grooveSettings_.empty()
                                  ? QStringLiteral("Распознать ручьи по контуру")
                                  : QStringLiteral("Ввести ручьи вручную"));
}
void ToolImportDialog::recognizeGrooves() {
    if (!grooveSettings_.empty()) {
        clearGrooves();
        updatePreview();
        return;
    }
    const auto d = drawing();
    if (!d)
        return;
    const auto found = core::detectDieGrooves(*d);
    const bool duplicate =
        std::adjacent_find(found.begin(), found.end(), [](const auto &a, const auto &b) {
            return std::abs(a.openingMm - b.openingMm) < 1.e-5;
        }) != found.end();
    if (found.empty() || duplicate) {
        status_->setText(QStringLiteral(
            "Ручьи не распознаны однозначно. Поддержаны симметричные скруглённые V в положениях "
            "0/±90/180°. Можно сохранить контур с ручными параметрами."));
        return;
    }
    const QSignalBlocker blockV(openings_), blockAngle(angle_);
    QStringList values;
    for (const auto &g : found) {
        values << QString::number(g.openingMm, 'g', 12);
        grooveSettings_.push_back(g.settings);
    }
    openings_->setText(values.join(QStringLiteral("; ")));
    angle_->setValue(found.front().settings.angleRadians * 180 / std::numbers::pi);
    showGrooves();
    updatePreview();
    status_->setText(QStringLiteral("Распознано ручьёв: %1. Выберите строку для просмотра. При "
                                    "выборе V корпус будет поворачиваться автоматически.")
                         .arg(found.size()));
}
void ToolImportDialog::updatePreview() {
    if (!saveButton_)
        return;
    radius_->setVisible(kind_->currentIndex() == 0);
    openings_->setVisible(kind_->currentIndex() == 1);
    recognizeButton_->setVisible(kind_->currentIndex() == 1);
    recognizeButton_->setEnabled(drawing().has_value());
    grooves_->setVisible(kind_->currentIndex() == 1 && !grooveSettings_.empty());
    openings_->setReadOnly(!grooveSettings_.empty());
    angle_->setEnabled(grooveSettings_.empty());
    auto *form = qobject_cast<QFormLayout *>(form_->layout());
    form->labelForField(radius_)->setVisible(kind_->currentIndex() == 0);
    form->labelForField(openings_)->setVisible(kind_->currentIndex() == 1);
    auto d = drawing();
    const auto row = grooves_->currentRow();
    if (d && kind_->currentIndex() == 1 && row >= 0 &&
        static_cast<std::size_t>(row) < grooveSettings_.size())
        d = core::rotateToolDrawing(*d,
                                    grooveSettings_[static_cast<std::size_t>(row)].rotationRadians);
    while (auto *item = previewLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (d)
        previewLayout_->addWidget(new ToolDrawingWidget(*d, previewHost_));
    else
        previewLayout_->addWidget(
            label(QStringLiteral("Выберите файл, контур и единицы DXF."), previewHost_));
    const auto t = preset();
    saveButton_->setEnabled(!busy_ && t.has_value());
    if (!busy_)
        status_->setText(
            !t ? QStringLiteral(
                     "Укажите название, производителя и единицы; проверьте параметры и контур.")
            : t->parametersConfirmed
                ? QStringLiteral("Параметры заданы. Перед сохранением будет проверено тело "
                                 "сечения. Поддержка контакта проверяется при выборе пары.")
                : QStringLiteral("Сохранится для просмотра. Для подбора нужны длина, рабочий угол "
                                 "и R носика / V матрицы."));
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
