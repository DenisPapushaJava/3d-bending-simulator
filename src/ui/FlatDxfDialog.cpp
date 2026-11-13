#include "FlatDxfDialog.h"
#include "ExportFileDialog.h"
#include "FlatDxfExport.h"
#include "FlatDxfPreview.h"
#include "PreviewIcons.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/Preferences.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <array>

namespace bending::ui {
namespace {
constexpr auto layerKey = "flatDxf/bendLayerName";
constexpr auto legacyLayerKey = "flatDxf/bendLayer";
constexpr auto colorKey = "flatDxf/bendColor";

struct BendColor {
    int aci;
    const char *name;
    QRgb rgb;
};

constexpr std::array<BendColor, 18> bendColors{{
    {1, "Красный", 0xFF0000},
    {2, "Жёлтый", 0xFFFF00},
    {3, "Зелёный", 0x00FF00},
    {4, "Голубой", 0x00FFFF},
    {5, "Синий", 0x0000FF},
    {6, "Пурпурный", 0xFF00FF},
    {30, "Оранжевый", 0xFF7F00},
    {40, "Янтарный", 0xFFBF00},
    {70, "Салатовый", 0x7FFF00},
    {112, "Изумрудный", 0x00A552},
    {120, "Бирюзовый", 0x00FFBF},
    {150, "Лазурный", 0x007FFF},
    {190, "Фиолетовый", 0x7F00FF},
    {230, "Розовый", 0xFF007F},
    {11, "Коралловый", 0xFF7F7F},
    {32, "Коричневый", 0xA55200},
    {8, "Серый", 0x808080},
    {9, "Светло-серый", 0xC0C0C0},
}};

const BendColor &bendColor(int aci) {
    const auto found = std::find_if(bendColors.begin(), bendColors.end(),
                                    [aci](const auto &entry) { return entry.aci == aci; });
    return found != bendColors.end() ? *found : bendColors.front();
}

QIcon swatch(const QColor &color) {
    QPixmap pixmap(24, 18);
    pixmap.fill(color);
    return QIcon(pixmap);
}
} // namespace

void FlatDxfDialog::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

FlatDxfDialog::FlatDxfDialog(ImportController &controller,
                             std::shared_ptr<const application::FlatPatternBuilder> builder,
                             const QString &modelPath, QWidget *parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("flatDxfDialog"));
    const auto &session = controller.session();
    const auto document = session.document();
    const bool planar = document && isPlanarCutCandidate(document->metadata().bendAnalysis);
    const auto &savedExport = controller.dxfExport();
    setWindowTitle(QStringLiteral("Развёртка / DXF"));
    setSizeGripEnabled(true);
    const auto *display = parent ? parent->screen() : screen();
    const auto available = display ? display->availableGeometry().size() : QSize(1080, 750);
    resize(std::min(1080, available.width() - 48), std::min(750, available.height() - 64));
    auto *root = new QVBoxLayout(this);
    auto *content = new QHBoxLayout;
    root->addLayout(content, 1);
    auto *canvas = new QVBoxLayout;
    content->addLayout(canvas, 1);
    auto *tools = new QHBoxLayout;
    canvas->addLayout(tools);
    auto *fit = new QPushButton(previewIcons::fit(), QString{}, this);
    auto *measure = new QPushButton(previewIcons::ruler(), QString{}, this);
    fit->setObjectName(QStringLiteral("flatDxfFit"));
    fit->setAccessibleName(QStringLiteral("Вписать развёртку"));
    fit->setToolTip(QStringLiteral("Вписать развёртку целиком"));
    measure->setObjectName(QStringLiteral("flatDxfMeasure"));
    measure->setAccessibleName(QStringLiteral("Измерить"));
    measure->setCheckable(true);
    measure->setToolTip(QStringLiteral("Измерить"));
    for (auto *button : {fit, measure}) {
        button->setAutoDefault(false);
        button->setIconSize(QSize(24, 24));
        button->setFixedSize(34, 34);
    }
    tools->addWidget(fit);
    tools->addWidget(measure);
    tools->addStretch();
    auto *preview = new FlatDxfPreview(this);
    canvas->addWidget(preview, 1);
    auto *measurement = new QLabel(this);
    measurement->setObjectName(QStringLiteral("flatDxfMeasurement"));
    measurement->setWordWrap(true);
    measurement->setMinimumHeight(measurement->fontMetrics().height() + 4);
    canvas->addWidget(measurement);
    auto *measurementHint =
        new QLabel(QStringLiteral("Ctrl или Alt + клик — свободная точка · "
                                  "Shift + клик — точка на контуре или квадрант · Esc — сбросить"),
                   this);
    measurementHint->setObjectName(QStringLiteral("flatDxfMeasurementHint"));
    measurementHint->setTextFormat(Qt::PlainText);
    measurementHint->setWordWrap(true);
    measurementHint->setForegroundRole(QPalette::WindowText);
    auto hintPolicy = measurementHint->sizePolicy();
    hintPolicy.setRetainSizeWhenHidden(true);
    measurementHint->setSizePolicy(hintPolicy);
    measurementHint->hide();
    canvas->addWidget(measurementHint);
    connect(fit, &QPushButton::clicked, preview, &FlatDxfPreview::fitDrawing);
    connect(measure, &QPushButton::toggled, preview, &FlatDxfPreview::setMeasuring);
    connect(measure, &QPushButton::toggled, measurementHint, &QLabel::setVisible);
    connect(preview, &FlatDxfPreview::measurementChanged, measurement, &QLabel::setText);

    auto *sidebar = new QWidget(this);
    sidebar->setFixedWidth(252);
    content->addWidget(sidebar);
    auto *layout = new QFormLayout(sidebar);
    auto *status = new QLabel(this);
    status->setObjectName(QStringLiteral("flatDxfStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addRow(status);
    auto *openFolder = new QPushButton(QStringLiteral("Открыть папку"), this);
    openFolder->setObjectName(QStringLiteral("flatDxfOpenFolder"));
    openFolder->hide();
    connect(openFolder, &QPushButton::clicked, this, [this, status, &controller] {
        const auto &saved = controller.dxfExport();
        if (!saved)
            return;
        const QFileInfo file(saved->path);
        if (!file.isAbsolute() || !file.isFile()) {
            const auto message = QStringLiteral("Сохранённый DXF не найден.");
            status->setText(message);
            auto *warning = new QMessageBox(QMessageBox::Warning, windowTitle(), message,
                                            QMessageBox::Ok, this);
            warning->setObjectName(QStringLiteral("flatDxfMissingFile"));
            warning->setAttribute(Qt::WA_DeleteOnClose);
            warning->button(QMessageBox::Ok)->setText(QStringLiteral("ОК"));
            warning->open();
        } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()))) {
            status->setText(QStringLiteral("Не удалось открыть папку сохранения."));
        } else {
            status->setText(QStringLiteral("DXF сохранён."));
        }
    });
    auto *size = new QLabel(this);
    size->setWordWrap(true);
    layout->addRow(size);

    auto *bends = new QCheckBox(QStringLiteral("Добавить линии гиба"), this);
    bends->setChecked(!planar && (!savedExport || savedExport->includeBends));
    bends->setEnabled(!planar);
    layout->addRow(bends);
    const auto settings = preferences();
    const auto defaultLayer = FlatDxfOptions{}.bendLayer;
    auto savedLayer = settings->value(layerKey).toString();
    if (!settings->contains(layerKey)) {
        savedLayer = settings->value(legacyLayerKey).toString();
        if (savedLayer == QStringLiteral("BENDING"))
            savedLayer = defaultLayer;
    }
    if (savedExport)
        savedLayer = QString::fromStdString(savedExport->bendLayer);
    auto *layer = new QLineEdit(validBendLayer(savedLayer) ? savedLayer : defaultLayer, this);
    layer->setMaxLength(63);
    layer->setToolTip(QStringLiteral("Латинские буквы, цифры, дефис и подчёркивание"));
    layout->addRow(QStringLiteral("Слой"), layer);
    auto *color = new QToolButton(this);
    color->setObjectName(QStringLiteral("flatDxfColor"));
    color->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    color->setPopupMode(QToolButton::InstantPopup);
    auto *palette = new QMenu(color);
    color->setMenu(palette);
    auto *paletteAction = new QWidgetAction(palette);
    auto *swatches = new QWidget(palette);
    auto *grid = new QGridLayout(swatches);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setSpacing(4);
    auto *colors = new QButtonGroup(palette);
    for (int i = 0; i < static_cast<int>(bendColors.size()); ++i) {
        const auto &entry = bendColors[static_cast<std::size_t>(i)];
        const auto name = QString::fromUtf8(entry.name);
        auto *button = new QToolButton(swatches);
        button->setObjectName(QStringLiteral("flatDxfColor%1").arg(entry.aci));
        button->setAccessibleName(name);
        button->setToolTip(QStringLiteral("%1 (ACI %2)").arg(name).arg(entry.aci));
        button->setIcon(swatch(QColor::fromRgb(entry.rgb)));
        button->setIconSize(QSize(24, 18));
        button->setFixedSize(38, 32);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::StrongFocus);
        colors->addButton(button, entry.aci);
        grid->addWidget(button, i / 6, i % 6);
    }
    paletteAction->setDefaultWidget(swatches);
    palette->addAction(paletteAction);
    const auto updateColor = [color](int index) {
        const auto &entry = bendColor(index);
        const auto name = QString::fromUtf8(entry.name);
        color->setText(name);
        color->setIcon(swatch(QColor::fromRgb(entry.rgb)));
        color->setAccessibleName(QStringLiteral("Цвет линий гиба — %1").arg(name));
    };
    const int initialColor =
        bendColor(savedExport ? savedExport->bendColor : settings->value(colorKey, 1).toInt()).aci;
    colors->button(initialColor)->setChecked(true);
    updateColor(initialColor);
    connect(palette, &QMenu::aboutToShow, this, [palette, colors] {
        QTimer::singleShot(0, palette, [palette, colors] {
            if (palette->isVisible())
                colors->checkedButton()->setFocus(Qt::PopupFocusReason);
        });
    });
    connect(colors, &QButtonGroup::idClicked, this, [palette, updateColor](int index) {
        updateColor(index);
        palette->close();
    });
    layout->addRow(QStringLiteral("Цвет"), color);

    auto *buttons = new QHBoxLayout;
    auto *save = new QPushButton(QStringLiteral("Сохранить DXF…"), this);
    save->setObjectName(QStringLiteral("flatDxfSave"));
    save->setDefault(true);
    auto *close = new QPushButton(QStringLiteral("Закрыть"), this);
    close->setObjectName(QStringLiteral("flatDxfClose"));
    buttons->addStretch();
    buttons->addWidget(save);
    buttons->addWidget(openFolder);
    buttons->addSpacing(8);
    buttons->addWidget(close);
    root->addLayout(buttons);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    const auto revision = session.revision();
    const auto request = [&controller] {
        const auto &active = controller.session();
        return application::FlatPatternRequest{active.unfoldSettings(), active.manualThicknessMm()};
    };
    const auto inputsCurrent = [&controller, revision, planar] {
        const auto &active = controller.session();
        return active.revision() == revision && !active.activeRequest() &&
               (planar || active.hasPartInputs());
    };
    const auto current = [inputsCurrent, &controller] {
        return inputsCurrent() ? controller.dxfFlatPattern() : nullptr;
    };
    const auto saveDialogName = QStringLiteral("flatDxfFileDialog");
    const auto options = [=] {
        return FlatDxfOptions{bends->isChecked(), layer->text().trimmed(), colors->checkedId()};
    };
    const auto refresh = [=, &controller] {
        const auto flat = current();
        const auto profile = flat ? flat->info().cutProfile : nullptr;
        const auto selected = options();
        const auto &saved = controller.dxfExport();
        const bool useBends = selected.includeBends;
        const bool validLayer = !useBends || validBendLayer(layer->text().trimmed());
        const bool showExport =
            profile && validLayer && saved && saved->includeBends == useBends &&
            (!useBends || (saved->bendLayer == selected.bendLayer.toStdString() &&
                           saved->bendColor == selected.bendColor));
        openFolder->setVisible(showExport);
        if (showExport)
            openFolder->setToolTip(QDir::toNativeSeparators(QFileInfo(saved->path).filePath()));
        preview->setPattern(profile ? flat : nullptr);
        preview->setBends(useBends, QColor::fromRgb(bendColor(colors->checkedId()).rgb));
        fit->setEnabled(bool(profile));
        measure->setEnabled(bool(profile));
        if (!profile)
            measure->setChecked(false);
        const auto dimensions = preview->drawingSize();
        const QLocale locale;
        size->setText(profile ? QStringLiteral("%1 × %2 мм")
                                    .arg(locale.toString(dimensions.width(), 'f', 2),
                                         locale.toString(dimensions.height(), 'f', 2))
                              : QString{});
        save->setEnabled(profile && validLayer);
        layer->setEnabled(useBends);
        color->setEnabled(useBends);
        if (!inputsCurrent())
            status->setText(controller.session().activeRequest()
                                ? QStringLiteral("Загружаем деталь…")
                                : QStringLiteral("Задайте материал и толщину."));
        else if (!profile && controller.isPreparingFlatPattern())
            status->setText(planar ? QStringLiteral("Проверяем плоский контур…")
                                   : QStringLiteral("Готовим развёртку…"));
        else if (!flat)
            status->setText(planar ? QStringLiteral("Плоский контур резки недоступен.")
                                   : QStringLiteral("Развёртка недоступна для этой детали."));
        else if (!profile)
            status->setText(QStringLiteral("Не удалось подтвердить контуры сквозной резки."));
        else if (!validLayer)
            status->setText(QStringLiteral("Укажите допустимое имя слоя гибов."));
        else if (showExport) {
            const QFileInfo file(saved->path);
            status->setText(file.isAbsolute() && file.isFile()
                                ? QStringLiteral("DXF сохранён.")
                                : QStringLiteral("Сохранённый DXF не найден."));
        } else {
            auto message = QStringLiteral("Контуры резки в слое 0 · 1:1, мм");
            if (planar)
                message.prepend(QStringLiteral("Деталь без гибов\n"));
            if (profile->midThickness)
                message += QStringLiteral("\nРасчётный контур по середине толщины");
            if (profile->blindOpenings)
                message +=
                    QStringLiteral("\nГлухих вырезов исключено: %1").arg(profile->blindOpenings);
            status->setText(message);
        }
    };
    auto *rebuildTimer = new QTimer(this);
    rebuildTimer->setSingleShot(true);
    connect(rebuildTimer, &QTimer::timeout, this, [=, this, &controller] {
        if (!isVisible() || !rebuildPending_ || !inputsCurrent() ||
            !controller.canPrepareFlatPattern())
            return;
        rebuildPending_ = false;
        if (!controller.flatPattern() && !controller.isPreparingFlatPattern())
            controller.prepareFlatPattern(builder);
    });
    connect(&controller, &ImportController::stateChanged, this,
            [=, this, &controller, lastRequest = request()]() mutable {
                if (!isVisible())
                    return;
                const auto flat = current();
                if (!flat)
                    closeExportFileDialogs(this, saveDialogName);
                if (controller.session().revision() != revision) {
                    reject();
                    return;
                }
                const auto nextRequest = request();
                const bool needsRebuild =
                    lastRequest != nextRequest || controller.session().activeRequest();
                if (needsRebuild)
                    rebuildPending_ = true;
                lastRequest = nextRequest;
                refresh();
                // A close or a successful import can still change the document in this turn.
                if (rebuildPending_ && inputsCurrent() && controller.canPrepareFlatPattern() &&
                    !rebuildTimer->isActive())
                    rebuildTimer->start(0);
            });
    connect(bends, &QCheckBox::toggled, this, refresh);
    connect(layer, &QLineEdit::textChanged, this, refresh);
    connect(colors, &QButtonGroup::idClicked, this, refresh);
    connect(save, &QPushButton::clicked, this, [=, this, &controller] {
        const auto flat = current();
        if (!flat || !flat->info().cutProfile)
            return;
        chooseExportFile(
            this, saveDialogName, QStringLiteral("Сохранить развёртку для резки"),
            suggestedFlatDxfPath(modelPath), QStringLiteral("DXF (*.dxf)"), QStringLiteral("dxf"),
            [=, this, &controller](const QString &path) {
                if (!isVisible())
                    return;
                if (flat != current()) {
                    status->setText(QStringLiteral("Развёртка изменилась. Повторите сохранение."));
                    return;
                }
                const auto selected = options();
                const auto error = writeFlatDxf(path, flat->info(), selected);
                if (!error.isEmpty()) {
                    status->setText(error);
                    return;
                }
                rememberFlatDxfDirectory(path);
                if (selected.includeBends) {
                    auto saved = preferences();
                    saved->setValue(layerKey, selected.bendLayer);
                    saved->setValue(colorKey, selected.bendColor);
                    saved->sync();
                }
                controller.recordDxfExport({QFileInfo(path).filesystemAbsoluteFilePath(),
                                            selected.includeBends, selected.bendLayer.toStdString(),
                                            selected.bendColor});
                refresh();
            });
    });
    refresh();
}
} // namespace bending::ui
