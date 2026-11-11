#include "FlatDxfExport.h"
#include "FlatDxfGeometry.h"
#include "FlatDxfPreview.h"
#include "ProjectFormatFixtures.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/project/ProjectFiles.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include "bending/ui/Preferences.h"
#include "bending/viewer/ViewerWidget.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QThread>
#include <QToolButton>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace bending;
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}
QString qtPath(const std::filesystem::path &value) {
#ifdef _WIN32
    return QString::fromStdWString(value.native());
#else
    return QString::fromUtf8(value.native());
#endif
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    check(condition(), "Operation timed out");
}
class LateBuilder final : public application::FlatPatternBuilder {
  public:
    mutable std::atomic<bool> entered = false, release = false;
    class Snapshot final : public application::FlatPattern {
      public:
        application::FlatPatternInfo data;
        const application::FlatPatternInfo &info() const noexcept override {
            return data;
        }
    };
    application::FlatPatternResult build(application::DocumentPtr, application::FlatPatternRequest,
                                         std::stop_token) const override {
        entered = true;
        while (!release)
            QThread::msleep(1);
        return application::FlatPatternPtr(std::make_shared<Snapshot>());
    }
};
class SharedFlatBuilder final : public application::FlatPatternBuilder {
  public:
    mutable std::atomic<int> calls = 0;
    std::atomic<bool> release = false;
    bool fail = false;
    application::FlatPatternResult build(application::DocumentPtr document,
                                         application::FlatPatternRequest request,
                                         std::stop_token stop) const override {
        ++calls;
        while (!release && !stop.stop_requested())
            QThread::msleep(1);
        if (stop.stop_requested())
            return application::FlatPatternError{application::FlatPatternErrorCode::Cancelled,
                                                 "Cancelled"};
        if (fail)
            return application::FlatPatternError{
                application::FlatPatternErrorCode::UnsupportedGeometry, "Test unsupported blank"};
        return cad::FlatPatternBuilder{}.build(std::move(document), request, stop);
    }
};
class SharedDrawingBuilder final : public application::BendingDrawingBuilder {
  public:
    mutable std::atomic<int> calls = 0;
    mutable application::FlatPatternPtr prepared;
    application::DrawingResult build(application::DocumentPtr document,
                                     application::DrawingRequest request,
                                     std::stop_token stop) const override {
        ++calls;
        prepared = request.preparedFlatPattern;
        return cad::BendingDrawingBuilder{}.build(std::move(document), std::move(request), stop);
    }
};
void sharedFlatWorkflow(const QString &sample) {
    ui::ImportController c(std::make_shared<cad::StepImporter>(),
                           [](application::DocumentPtr, std::uint64_t) { return QString{}; });
    c.open(native(sample));
    wait([&] { return !c.hasWork(); });
    check(c.setUnfoldSettings({core::SheetMaterial::MildSteel, .45, 1}),
          "Cannot set shared flat inputs");
    auto drawing = std::make_shared<SharedDrawingBuilder>();
    check(c.prepareDrawing(drawing, {}), "Cold drawing did not start");
    wait([&] { return !c.hasWork(); });
    check(c.drawing() && c.flatPattern() && c.flatPattern() == c.drawing()->preparedFlatPattern,
          "Cold drawing did not publish its prepared stock for the viewport");

    for (bool drawingFirst : {true, false}) {
        check(c.setUnfoldSettings({core::SheetMaterial::MildSteel, drawingFirst ? .46 : .47, 1}),
              "Shared flat inputs did not invalidate the cache");
        auto flat = std::make_shared<SharedFlatBuilder>();
        check(c.prefetchFlatPattern(flat) && !c.hasWork() && !c.isPreparingFlatPattern(),
              "Speculative flat blocked the foreground");
        wait([&] { return flat->calls == 1; });
        if (drawingFirst) {
            check(c.prepareDrawing(drawing, {}), "Drawing could not await speculative stock");
            c.cancelDrawing();
            check(!c.hasWork(), "Closing drawing left a speculative flat marked busy");
            check(c.prepareDrawing(drawing, {}) && c.prepareFlatPattern(flat),
                  "Viewport could not adopt flat already awaited by drawing");
            c.cancelDrawing();
            check(c.isPreparingFlatPattern() && !c.isPreparingDrawing(),
                  "Closing drawing cancelled the viewport consumer");
        } else {
            check(c.prepareFlatPattern(flat) && c.prepareDrawing(drawing, {}),
                  "Drawing could not adopt flat already awaited by viewport");
        }
        flat->release = true;
        wait([&] { return !c.hasWork(); });
        check(flat->calls == 1 && c.flatPattern(), "Shared flat restarted or failed to publish");
        if (drawingFirst) {
            check(!c.drawing() && c.prepareDrawing(drawing, {}), "Cancelled drawing was published");
            wait([&] { return !c.hasWork(); });
        }
        check(c.drawing() && drawing->prepared == c.flatPattern(),
              "Drawing did not consume the viewport's exact stock snapshot");
    }

    check(c.setUnfoldSettings({core::SheetMaterial::MildSteel, .48, 1}),
          "Cannot set unsupported flat test inputs");
    auto unsupported = std::make_shared<SharedFlatBuilder>();
    unsupported->fail = true;
    check(c.prefetchFlatPattern(unsupported) && c.prepareDrawing(drawing, {}),
          "Drawing could not attach to a failing flat job");
    unsupported->release = true;
    wait([&] { return !c.hasWork(); });
    check(unsupported->calls == 1 && c.drawing() && !c.drawing()->flatPattern &&
              c.drawing()->flatPatternError &&
              c.drawing()->flatPatternError->detail == "Test unsupported blank" &&
              !c.flatPatternError(),
          "Failed warm-up hid folded views, recalculated stock or exposed a background error");
    auto recovered = std::make_shared<SharedFlatBuilder>();
    recovered->release = true;
    check(c.prepareFlatPattern(recovered), "Explicit flat retry did not start");
    wait([&] { return !c.hasWork(); });
    check(c.flatPattern() && c.prepareDrawing(drawing, {}), "Recovered flat is missing");
    wait([&] { return !c.hasWork(); });
    check(c.drawing() && c.drawing()->flatPattern && drawing->prepared == c.flatPattern(),
          "The old folded-only drawing masked a recovered flat snapshot");

    check(c.setUnfoldSettings({core::SheetMaterial::MildSteel, .49, 1}),
          "Cannot set cancelled flat test inputs");
    auto stale = std::make_shared<SharedFlatBuilder>();
    check(c.prefetchFlatPattern(stale), "Cancellable flat job did not start");
    wait([&] { return stale->calls == 1; });
    c.open(native(sample));
    wait([&] { return !c.hasWork(); });
    check(c.session().document() && !c.flatPattern() && !c.flatPatternError(),
          "Opening another document reused the old flat job");
}
void checkDxfMeasurements() {
    auto pattern = std::make_shared<LateBuilder::Snapshot>();
    auto profile = std::make_shared<application::FlatCutProfile>();
    profile->loops = {
        {{{{0, 0, 0}}, {{120, 0, 0}}, {{120, 80, 0}}, {{0, 80, 0}}}, {}, 0},
        {{}, {30, 20, 0}, 6},
        {{}, {70, 20, 0}, 7},
        {{{{110, 54, 0}, -1}, {{90, 54, 0}}, {{90, 66, 0}, -1}, {{110, 66, 0}}}, {}, 0}};
    pattern->data.cutProfile = profile;
    pattern->data.bends = {{0, {0, 40, 0}, {120, 40, 0}, {}, 1, 1, 2, .5, 5}};
    ui::FlatDxfPreview view;
    view.resize(720, 480);
    view.show();
    view.setPattern(pattern);
    view.setBends(true, Qt::red);
    QString result;
    QObject::connect(&view, &ui::FlatDxfPreview::measurementChanged, &view,
                     [&](const QString &value) {
                         result = QTextDocumentFragment::fromHtml(value).toPlainText();
                     });
    view.setMeasuring(true);
    const auto click = [&](double x, double y, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const auto position = view.mapFromScene(QPointF(x, -y));
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                          QPointF(view.viewport()->mapToGlobal(position)), Qt::LeftButton,
                          Qt::LeftButton, modifiers);
        QApplication::sendEvent(view.viewport(), &press);
    };
    const auto formatted = [](double value) { return QLocale().toString(value, 'f', 2); };
    click(100, 44);
    check(result == QStringLiteral("R%1 мм").arg(formatted(10)), "Incorrect DXF arc radius");
    click(100.5, 54.5);
    click(99.5, 66.5);
    check(result.startsWith(formatted(12)) &&
              result.contains(QStringLiteral("ΔX %1").arg(formatted(0))),
          "DXF arc centres did not snap");
    click(100.7, 75.6, Qt::ShiftModifier);
    click(99.4, 44.4);
    check(result.startsWith(formatted(32)) &&
              result.contains(QStringLiteral("ΔX %1").arg(formatted(0))),
          "DXF quadrants did not snap to the two extremes of the slot");
    const auto slotLength = result;
    click(100, 56, Qt::ShiftModifier);
    check(result == slotLength, "A quadrant outside the actual arc became selectable");
    click(36, 20);
    check(result == QStringLiteral("Ø%1 мм").arg(formatted(12)), "Incorrect DXF bore diameter");
    click(30, 20);
    click(70, 20);
    check(result.startsWith(formatted(40)) && result.contains(QStringLiteral("ΔX")),
          "DXF hole centres did not snap");
    click(50, 0);
    click(85, 40);
    check(result == QStringLiteral("Между линиями %1 мм").arg(formatted(40)),
          "DXF bend clearance is not perpendicular");
    click(50, 0, Qt::ShiftModifier);
    click(85, 40, Qt::ShiftModifier);
    check(result.contains(QStringLiteral("ΔX")) && !result.startsWith(QStringLiteral("≈")),
          "Shift did not select exact points on DXF lines");
    click(30, 20);
    click(50, 0);
    check(result == QStringLiteral("До линии %1 мм").arg(formatted(20)),
          "DXF point-to-edge measurement is incorrect");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &escape);
    check(!result.contains(formatted(20)), "Escape did not clear the measurement");
    view.setBends(false, Qt::red);
    click(50, 0);
    const auto waitingForSecond = result;
    click(85, 40);
    check(result == waitingForSecond, "A blank click changed the active measurement");
    click(85, 40, Qt::AltModifier);
    check(result.startsWith(QStringLiteral("До линии ≈")), "Alt did not select a free point");
    const auto approximate = result;
    click(85, 40, Qt::ShiftModifier);
    check(result == approximate, "Shift alone selected a free point");
    click(85, 40, Qt::ControlModifier);
    click(50, 0);
    check(result == approximate, "Ctrl did not select a free point");

    auto marking = pattern->info();
    auto &bend = marking.bends.front();
    bend.start = {-10, 20, 0};
    bend.end = {130, 20, 0};
    const auto checkMarking = [&](std::initializer_list<std::array<double, 2>> expected) {
        const auto segments = ui::dxfBendLines(marking);
        check(segments.size() == expected.size(), "Incorrect clipped DXF segment count");
        std::size_t i = 0;
        for (const auto &ends : expected) {
            const auto &line = segments[i++];
            check(std::abs(line.x1() - ends[0]) < 1.e-8 && std::abs(line.x2() - ends[1]) < 1.e-8 &&
                      line.y1() == bend.start.y && line.y2() == bend.end.y,
                  "Clipped DXF marking shifted or has the wrong edge clearance");
        }
    };
    checkMarking({{.3, 23.7}, {36.3, 62.7}, {77.3, 119.7}}); // Round holes.
    bend.start.y = bend.end.y = 60;
    checkMarking({{.3, 89.7}, {110.3, 119.7}}); // Slot with circular ends.
    bend.start.y = bend.end.y = 50;
    checkMarking({{.3, 99.7 - std::sqrt(84.)}, {100.3 + std::sqrt(84.), 119.7}});
    bend.start.y = bend.end.y = 44;
    checkMarking({{.3, 99.7}, {100.3, 119.7}}); // Tangent to the slot.
    bend.start.y = bend.end.y = 0;
    checkMarking({}); // Coincides with the cut edge.
    auto notched = std::make_shared<application::FlatCutProfile>();
    notched->loops = {{{{{0, 0, 0}},
                        {{120, 0, 0}},
                        {{120, 80, 0}},
                        {{110, 80, 0}},
                        {{60, 30, 0}},
                        {{10, 80, 0}},
                        {{0, 80, 0}}},
                       {},
                       0}};
    marking.cutProfile = notched;
    bend.start = {20, 40, 0};
    bend.end = {100, 40, 0};
    checkMarking({{20, 49.7}, {70.3, 100}}); // Oblique notch and endpoints inside the material.

    // The slot has clockwise semicircles. Add a counterclockwise arc crossing 0°.
    auto exportProfile = std::make_shared<application::FlatCutProfile>(*profile);
    exportProfile->loops.push_back(
        {{{{130, 0, 0}, std::sqrt(2.) - 1}, {{140, 10, 0}}, {{130, 10, 0}}}, {}, 0});
    auto exportInfo = pattern->info();
    exportInfo.cutProfile = std::move(exportProfile);
    QTemporaryDir output;
    check(output.isValid(), "Cannot create a primitive DXF test folder");
    QFile exported(output.filePath("primitives.dxf"));
    check(ui::writeFlatDxf(exported.fileName(), exportInfo, {false, {}, 0}).isEmpty() &&
              exported.open(QIODevice::ReadOnly),
          "Cannot export the mixed cut profile");
    const auto primitives = exported.readAll();
    check(primitives.count("\n0\nARC\n") == 3 && primitives.count("\n0\nCIRCLE\n") == 2 &&
              primitives.count("\n0\nLINE\n") == 8 &&
              primitives.count("\nAcDbArc\n50\n0\n51\n180\n") == 1 &&
              primitives.count("\nAcDbArc\n50\n180\n51\n0\n") == 1 &&
              primitives.contains("\nAcDbArc\n50\n270\n51\n") && !primitives.contains("LWPOLYLINE"),
          "DXF primitives lost the contour geometry or arc direction");

    auto rotated = std::make_shared<LateBuilder::Snapshot>();
    auto cut = std::make_shared<application::FlatCutProfile>(*profile);
    // Make the segmented datum oblique; the stock still contains holes and arcs.
    const auto turn = [](application::PointMm p) {
        return application::PointMm{100 + .6 * p.x - .8 * p.y, 20 + .8 * p.x + .6 * p.y, 0};
    };
    for (auto &loop : cut->loops) {
        loop.centre = turn(loop.centre);
        for (auto &v : loop.vertices)
            v.point = turn(v.point);
    }
    rotated->data.cutProfile = cut;
    for (std::size_t i = 0; i < 3; ++i) {
        const double x = 30. * i;
        rotated->data.bends.push_back(
            {i, turn({x, 5, 0}), turn({x + 20, 5, 0}), {}, 1, 1, 2, .5, 5});
    }
    const auto frame = ui::flatDxfFrame(rotated->info());
    const auto hole = frame.map(cut->loops[1].centre);
    check(std::abs(hole.x - 30) < 1.e-8 && std::abs(hole.y - 20) < 1.e-8,
          "Aligning the DXF datum shifted or mirrored the holes");
    auto dense = rotated->data;
    dense.bends.clear();
    for (std::size_t i = 0; i < 33; ++i) {
        const double x = 3. * i;
        dense.bends.push_back({i, turn({x, 5, 0}), turn({x + 2, 5, 0}), {}, 1, 1, 2, .5, 5});
    }
    const auto denseHole = ui::flatDxfFrame(dense).map(cut->loops[1].centre);
    check(std::abs(denseHole.x - hole.x) < 1.e-8 && std::abs(denseHole.y - hole.y) < 1.e-8,
          "The route limit disabled DXF orientation for a segmented datum");
    view.setPattern(rotated);
    const auto size = view.drawingSize();
    check(std::abs(size.width() - 120) < 1.e-8 && std::abs(size.height() - 80) < 1.e-8,
          "The preview did not use the DXF datum");
    auto unrelated = rotated->data;
    unrelated.bends.back().start.x += 1;
    unrelated.bends.back().end.x += 1;
    const auto unrotatedHole = ui::flatDxfFrame(unrelated).map(cut->loops[1].centre);
    check(unrotatedHole.x == cut->loops[1].centre.x && unrotatedHole.y == cut->loops[1].centre.y,
          "Offset parallel bends changed the DXF orientation");
    auto portrait = rotated->data;
    for (auto &b : portrait.bends) {
        b.start = {20, b.start.y, 0};
        b.end = {20, b.end.y, 0};
    }
    check(ui::flatDxfFrame(portrait).map({10, 20, 0}).x == 10,
          "A vertical datum unnecessarily rotated the drawing");
}

void checkPlanarDxf(ui::MainWindow &window) {
    window.setFlatPatternBuilder(std::make_shared<cad::FlatPatternBuilder>());
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/plate-mm.step");
    auto &controller = window.importController();
    wait([&] { return !controller.hasWork(); });
    auto *action = window.findChild<QToolButton *>("flatDxfStageButton");
    check(action && action->isEnabled() &&
              action->accessibleName().contains(QStringLiteral("Деталь без гибов")) &&
              !controller.session().hasPartInputs() &&
              !window.findChild<QDialog *>("partInputDialog"),
          "Planar DXF was unavailable without material or not labelled");
    action->click();
    wait([&] { return !controller.hasWork(); });
    auto *dialog = window.findChild<QDialog *>("flatDxfDialog");
    check(dialog && controller.flatPattern() && controller.flatPattern()->info().cutProfile,
          "Opening planar DXF did not prepare a cutting profile");
    auto *preview = dialog->findChild<ui::FlatDxfPreview *>("flatDxfPreview");
    auto *bends = dialog->findChild<QCheckBox *>();
    check(preview && !preview->drawingSize().isEmpty() && bends && !bends->isEnabled() &&
              !bends->isChecked() && !dialog->findChild<QLineEdit *>()->isEnabled() &&
              !dialog->findChild<QToolButton *>("flatDxfColor")->isEnabled() &&
              dialog->findChild<QPushButton *>("flatDxfMeasure")->isEnabled() &&
              dialog->findChild<QPushButton *>("flatDxfFit")->isEnabled() &&
              dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled(),
          "Planar DXF preview or bending controls are in the wrong state");
    QTemporaryDir output;
    check(output.isValid(), "Cannot create planar DXF output folder");
    QFile dxf(output.filePath("plate.dxf"));
    check(ui::writeFlatDxf(dxf.fileName(), controller.flatPattern()->info(), {}).isEmpty() &&
              dxf.open(QIODevice::ReadOnly),
          "Cannot write planar DXF");
    const auto bytes = dxf.readAll();
    check(bytes.contains("\nCUT\n") && !bytes.contains("\nBENDING\n"),
          "Planar DXF contains a bend layer");
    dialog->reject();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        checkDxfMeasurements();
        const bool graphics = app.arguments().contains(QStringLiteral("--graphics"));
        auto files = std::make_shared<project::ProjectFiles>();
        QTemporaryDir preferencesDir;
        check(preferencesDir.isValid(), "No temporary settings directory");
        ui::setPreferencesFile(preferencesDir.filePath("settings.ini"));
        const auto setSteelK = [](double factor) {
            auto settings = ui::preferences();
            settings->setValue("materials/presets", QVariantList{QVariantMap{{"k", factor}}});
            settings->sync();
        };
        auto builder = std::make_shared<SharedFlatBuilder>();
        ui::MainWindow window({"test", "test"}, files, files,
                              std::make_shared<cad::SingleBendPreviewBuilder>());
        window.resize(1280, 850);
        window.show();
        checkPlanarDxf(window);
        window.setFlatPatternBuilder(builder);
        const auto sample =
            app.arguments().contains(QStringLiteral("--sample"))
                ? app.arguments().at(app.arguments().indexOf(QStringLiteral("--sample")) + 1)
                : QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step";
        sharedFlatWorkflow(QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step");
        window.openStep(sample);
        auto &c = window.importController();
        wait([&] { return !c.hasWork(); });
        check(c.session().document() != nullptr, "No document");
        wait([&] { return window.findChild<QDialog *>("partInputDialog") != nullptr; });
        auto *material = window.findChild<QComboBox *>("sheetMaterial");
        auto *button = window.findChild<QPushButton *>("showFlatPattern");
        check(material && button && material->currentIndex() == 0 &&
                  !c.session().unfoldSettings().manualK && !c.session().modified() &&
                  !window.findChild<QPushButton *>("partInputContinue")->isEnabled(),
              "New part silently selected a material or modified the session");
        material->setCurrentIndex(1);
        window.findChild<QPushButton *>("partInputContinue")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(c.session().modified() &&
                  c.session().unfoldSettings().material == core::SheetMaterial::MildSteel &&
                  c.session().unfoldSettings().manualK == .50,
              "Material not edited in project");
        auto *dxfButton = window.findChild<QToolButton *>("flatDxfStageButton");
        check(dxfButton && !dxfButton->isEnabled(), "DXF enabled without a flat snapshot");
        button->click();
        check(c.isPreparingFlatPattern() && !dxfButton->isEnabled(),
              "DXF enabled during flat-pattern preparation");
        builder->release = true;
        wait([&] { return !c.hasWork(); });
        if (!c.flatPattern())
            throw std::runtime_error(c.flatPatternError() ? c.flatPatternError()->detail
                                                          : "No flat snapshot");
        const auto first = c.flatPattern();
        check(!first->info().bends.empty() &&
                  std::all_of(first->info().bends.begin(), first->info().bends.end(),
                              [](const auto &bend) { return bend.k == .50; }),
              "Default 0.50 did not reach the actual flat-pattern calculation");
        check(dxfButton->isEnabled(), "DXF unavailable with verified cut profile");
        dxfButton->click();
        auto *dxfDialog = window.findChild<QDialog *>("flatDxfDialog");
        check(dxfDialog && c.flatPattern() == first && !c.hasWork(),
              "DXF did not use the existing flat snapshot");
        auto *preview = dxfDialog->findChild<ui::FlatDxfPreview *>("flatDxfPreview");
        auto *measure = dxfDialog->findChild<QPushButton *>("flatDxfMeasure");
        auto *measurementHint = dxfDialog->findChild<QLabel *>("flatDxfMeasurementHint");
        check(preview && preview->drawingSize().width() > 0 && measure && measure->isEnabled(),
              "DXF dialog did not present the cutting geometry");
        check(measurementHint && !measurementHint->isVisible(),
              "DXF measurement hint is always shown");
        measure->click();
        check(measurementHint->isVisible(), "DXF measurement mode did not show the shortcut hint");
        for (int press = 0; press < 2; ++press) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(preview, &escape);
            check(dxfDialog->isVisible() && measure->isChecked(),
                  "Escape closed the DXF preview or disabled measurement mode");
        }
        if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty()) {
            QApplication::processEvents();
            check(dxfDialog->grab().save(path + "-dxf.png"), "Could not capture the DXF preview");
        }
        measure->click();
        check(!measurementHint->isVisible(),
              "DXF measurement hint remained after disabling the mode");
        auto *colorButton = dxfDialog->findChild<QToolButton *>("flatDxfColor");
        check(colorButton && colorButton->menu(), "DXF colour palette is missing");
        colorButton->menu()->popup(colorButton->mapToGlobal(colorButton->rect().bottomLeft()));
        QApplication::processEvents();
        auto *orange = colorButton->menu()->findChild<QToolButton *>("flatDxfColor30");
        check(orange && orange->isVisible(), "DXF colour swatches are hidden");
        orange->click();
        const auto previewItems = preview->scene()->items();
        check(orange->isChecked() && !colorButton->menu()->isVisible() &&
                  std::any_of(previewItems.begin(), previewItems.end(),
                              [](QGraphicsItem *item) {
                                  const auto *path = qgraphicsitem_cast<QGraphicsPathItem *>(item);
                                  return path && path->pen().color() == QColor(255, 127, 0);
                              }),
              "DXF colour selection did not update the bend preview");
        auto *dxfButtons = dxfDialog->findChild<QDialogButtonBox *>();
        auto *saveDxf = dxfButtons->button(QDialogButtonBox::Save);
        auto *layer = dxfDialog->findChild<QLineEdit *>();
        check(saveDxf->isEnabled() && layer->text() == "BENDING", "DXF defaults are incorrect");
        auto drawingBuilder = std::make_shared<SharedDrawingBuilder>();
        window.setDrawingBuilder(drawingBuilder);
        auto *drawingAction = window.findChild<QAction *>("bendingDrawingAction");
        check(!dxfDialog->isModal() && drawingAction && drawingAction->isEnabled(),
              "DXF blocked access to the drawing");
        drawingAction->trigger();
        auto *pdfDialog = window.findChild<QDialog *>("bendingDrawingDialog");
        check(pdfDialog && pdfDialog->isVisible() && !pdfDialog->isModal(),
              "PDF did not open alongside DXF");
        auto *savePdf = pdfDialog->findChild<QPushButton *>("exportDrawingPdf");
        wait([&] { return savePdf->isEnabled(); });
        check(drawingBuilder->prepared == first && c.flatPattern() == first,
              "The two previews did not share the flat pattern");
        measure->click();
        dxfButton->click();
        check(window.findChildren<QDialog *>("flatDxfDialog").size() == 1 &&
                  dxfDialog->isVisible() && measure->isChecked(),
              "Reopening DXF reset the ruler or created a second preview");
        savePdf->click();
        saveDxf->click();
        auto *pdfPicker = pdfDialog->findChild<QFileDialog *>("drawingPdfFileDialog");
        auto *dxfPicker = dxfDialog->findChild<QFileDialog *>("flatDxfFileDialog");
        check(pdfPicker && dxfPicker && pdfPicker->isVisible() && dxfPicker->isVisible() &&
                  !QApplication::activeModalWidget(),
              "File selection blocked the other preview");
        pdfPicker->reject();
        dxfPicker->reject();
        pdfDialog->reject();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(dxfDialog->isVisible() && measure->isChecked(), "Closing PDF reset the DXF view");
        measure->click();
        layer->setText("cut");
        check(!saveDxf->isEnabled(), "Bends can overwrite the cut layer");
        auto *bendLines = dxfDialog->findChild<QCheckBox *>();
        bendLines->setChecked(false);
        check(saveDxf->isEnabled() && !layer->isEnabled(), "Cut-only mode cannot export");
        QKeyEvent layerEscape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(layer, &layerEscape);
        check(dxfDialog->isVisible(), "Escape closed the DXF dialog outside measurement mode");
        dxfButtons->button(QDialogButtonBox::Cancel)->click();
        check(!dxfDialog->isVisible(), "The Close button did not close the DXF dialog");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTemporaryDir exportDir;
        check(exportDir.isValid(), "Cannot create DXF output folder");
        const auto dxfFlag = app.arguments().indexOf(QStringLiteral("--dxf-output"));
        const auto dxfPath =
            dxfFlag > 0 ? app.arguments().value(dxfFlag + 1) : exportDir.filePath("Развёртка.dxf");
        check(ui::writeFlatDxf(dxfPath, first->info(), {}).isEmpty(), "DXF export failed");
        const auto cutOnlyPath =
            dxfFlag > 0 ? dxfPath + ".cut-only.dxf" : exportDir.filePath("cut-only.dxf");
        check(ui::writeFlatDxf(cutOnlyPath, first->info(), {false, {}, 0}).isEmpty(),
              "Cut-only export failed");
        QFile exported(dxfPath), cutOnly(cutOnlyPath);
        check(exported.open(QIODevice::ReadOnly) && cutOnly.open(QIODevice::ReadOnly),
              "Cannot read DXF outputs");
        const auto dxf = exported.readAll(), cuts = cutOnly.readAll();
        check(dxf.contains("$INSUNITS\n70\n4\n") && !dxf.contains("LWPOLYLINE") &&
                  dxf.contains("AcDbEntity\n8\nBENDING\n100\nAcDbLine") &&
                  cuts.contains("AcDbEntity\n8\nCUT\n100\nAcDbLine") && !cuts.contains("BENDING") &&
                  !dxf.contains("\nDIMENSION\n") && !dxf.contains("\nTEXT\n"),
              "DXF units or cut/bend separation is incorrect");
        ui::rememberFlatDxfDirectory(dxfPath);
        check(ui::suggestedFlatDxfPath(QStringLiteral("/missing/01.23 Деталь.step")) ==
                  QFileInfo(dxfPath).dir().filePath(QStringLiteral("01.23 Деталь — развёртка.dxf")),
              "DXF filename did not use the STEP basename or last export folder");
        auto invalid = first->info();
        invalid.cutProfile.reset();
        exported.close();
        check(!ui::writeFlatDxf(dxfPath, invalid, {}).isEmpty() &&
                  exported.open(QIODevice::ReadOnly) && exported.readAll() == dxf,
              "An unconfirmed profile overwrote a valid export");
        const auto edit = c.session().editRevision();
        button->click();
        check(c.flatPattern() == first && !c.hasWork() && edit == c.session().editRevision(),
              "Show recomputed flat or edited project");
        auto *tabs = window.findChild<QTabWidget *>("propertiesTabs");
        tabs->setCurrentIndex(2);
        auto *checkFlat = window.findChild<QPushButton *>("checkFlatPattern");
        checkFlat->click();
        check(c.flatPattern() == first && !c.hasWork(), "Check flow recomputed flat");
        auto *viewer = window.findChild<viewer::ViewerWidget *>("viewer");
        if (graphics) {
            wait([&] { return viewer->isReady(); });
            check(viewer->dimensionsVisible(), "Flat dimensions are not shown");
            const auto *dimensions = viewer->findChild<QToolButton *>("showDimensions");
            check(dimensions != nullptr, "Missing dimensions control");
            auto *bends = window.findChild<QListView *>("bendList");
            check(bends != nullptr, "No bend list");
            // Overview must not carry every bend's full dimensions.
            viewer->highlightFaces({}, c.session().revision());
            check(!dimensions->accessibleDescription().contains(QStringLiteral("ВВЕРХ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral("ВНИЗ")),
                  "Overview retained full bend callouts");
            const auto revisionBeforeSelection = c.session().editRevision();
            int picked = -1;
            QObject::connect(viewer, &viewer::ViewerWidget::bendSelectionChanged, &window,
                             [&](quint64, int index) { picked = index; });
            auto *canvas = viewer->findChild<QWidget *>("cadCanvas");
            const auto sendClick = [&](QPoint point) {
                QMouseEvent event(QEvent::MouseButtonPress, QPointF(point),
                                  QPointF(canvas->mapToGlobal(point)), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas, &event);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
                                    QPointF(canvas->mapToGlobal(point)), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(canvas, &release);
            };
            const auto clickBendLine = [&](const QImage &image) {
                // Use an actual rendered line instead of a fixed camera position.
                for (int y = 30; y < image.height() - 30; ++y)
                    for (int x = 30; x < image.width() - 220; ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.red() > 200 && color.blue() > 185 && color.green() < 155) {
                            sendClick(QPoint(x, y) / canvas->devicePixelRatioF());
                            if (picked >= 0)
                                return true;
                        }
                    }
                return false;
            };
            const auto overview = viewer->captureImage();
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty())
                overview.save(path + "-overview.png");
            check(clickBendLine(overview) && bends->currentIndex().row() == picked,
                  "Flat line click did not select its source bend in the list");
            check(dimensions->accessibleDescription().contains(QStringLiteral(" · L ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · Y ")) &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · X ")) &&
                      !c.hasWork() && c.flatPattern() == first &&
                      c.session().editRevision() == revisionBeforeSelection,
                  "Flat selection recalculated CAD/edited project or omitted selected dimensions");
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty())
                viewer->captureImage().save(path + "-selected-bend.png");
            const auto topLabels = dimensions->accessibleDescription();
            const auto measuredLabels = [](const QString &description) {
                // Compact ID visibility depends on viewport fitting, while the
                // selected measurements and their datum must remain identical.
                const QRegularExpression marker(QStringLiteral("^B[0-9]+(?:/B[0-9]+)*$"));
                QStringList result;
                for (const auto &line : description.split('\n'))
                    if (!marker.match(line).hasMatch())
                        result << line;
                return result;
            };
            const bool up = topLabels.contains(QStringLiteral("ВВЕРХ"));
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Bottom);
            check(dimensions->accessibleDescription().contains(up ? QStringLiteral("ВНИЗ")
                                                                  : QStringLiteral("ВВЕРХ")),
                  "Reverse flat side kept the old bend direction");
            const auto bendFromTop = picked;
            sendClick(QPoint(20, canvas->height() / 2));
            check(picked == -1, "Could not clear the selection from below the flat");
            const auto bottom = viewer->captureImage();
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty())
                bottom.save(path + "-bottom.png");
            check(clickBendLine(bottom) && picked == bendFromTop,
                  "The same bend line is not visible and selectable from below");
            viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty()) {
                viewer->findChild<QAction *>("displayWireframe")->trigger();
                viewer->captureImage().save(path + "-wireframe-top.png");
                viewer->setOrientation(viewer::ViewerWidget::Orientation::Bottom);
                viewer->captureImage().save(path + "-wireframe-bottom.png");
                viewer->findChild<QAction *>("displayEdges")->trigger();
                viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
            }
            const auto returnedLabels = dimensions->accessibleDescription();
            check(measuredLabels(returnedLabels) == measuredLabels(topLabels),
                  "Returning to the flat side changed the selected measurement");
            viewer->highlightFaces({}, c.session().revision() + 1);
            check(dimensions->accessibleDescription() == returnedLabels,
                  "Stale selection changed flat labels");
            sendClick(QPoint(20, canvas->height() / 2));
            check(picked == -1 && !bends->currentIndex().isValid() &&
                      !dimensions->accessibleDescription().contains(QStringLiteral(" · L ")),
                  "Empty flat click retained selected dimensions");
            // Source-list changes must update this same flat snapshot.
            bends->setCurrentIndex(bends->model()->index(0, 0));
            check(dimensions->accessibleDescription().contains(QStringLiteral("B01 ·")) &&
                      c.flatPattern() == first && !c.hasWork(),
                  "List selection did not refresh the flat bend or triggered a calculation");
            if (const auto measures = first->info().measurements;
                measures && !measures->openings.empty()) {
                const auto image = viewer->captureImage();
                int minX = image.width(), maxX = 0, minY = image.height(), maxY = 0;
                for (int y = 0; y < image.height(); ++y)
                    for (int x = 0; x < image.width(); ++x) {
                        const auto color = image.pixelColor(x, y);
                        if (color.red() > 50 && color.green() > 110 &&
                            color.green() > color.red() * 1.35 && color.blue() > color.green()) {
                            minX = std::min(minX, x);
                            maxX = std::max(maxX, x);
                            minY = std::min(minY, y);
                            maxY = std::max(maxY, y);
                        }
                    }
                check(maxX > minX && maxY > minY, "Cannot locate rendered flat stock");
                const auto &opening = measures->openings.front();
                const auto &bounds = first->info().bounds;
                const QPoint position(
                    qRound(minX + opening.center.x * (maxX - minX) / bounds.maximum.x),
                    qRound(maxY - opening.center.y * (maxY - minY) / bounds.maximum.y));
                const auto beforeClick = c.session().editRevision();
                std::cout << "Opening center " << opening.center.x << ',' << opening.center.y
                          << " pixel " << position.x() << ',' << position.y() << " body " << minX
                          << ',' << minY << ':' << maxX << ',' << maxY << '\n';
                sendClick(position / canvas->devicePixelRatioF());
                check(dimensions->accessibleDescription().contains(
                          opening.circular ? QStringLiteral("Отверстие") : QStringLiteral("Вырез")),
                      "Flat opening click did not reveal measured dimensions");
                check(dimensions->accessibleDescription().contains(QStringLiteral("Развёртка")) &&
                          dimensions->accessibleDescription().contains(QStringLiteral("Ширина")) &&
                          c.session().editRevision() == beforeClick && !c.hasWork(),
                      "Opening selection removed permanent dimensions or changed CAD/project");
                if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT");
                    !path.isEmpty())
                    viewer->captureImage(true).save(path + "-opening.png");
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(canvas, &escape);
                check(!dimensions->accessibleDescription().contains(QStringLiteral("Отверстие")) &&
                          !dimensions->accessibleDescription().contains(QStringLiteral("Вырез")) &&
                          dimensions->accessibleDescription().contains(QStringLiteral("Развёртка")),
                      "Escape did not preserve the permanent dimension layer");
            }
            wait([&] { return !viewer->captureImage().isNull(); });
            if (const auto output = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT");
                !output.isEmpty()) {
                check(viewer->captureImage(true).save(output + "-viewport.png"),
                      "Viewport capture failed");
                auto shot = window.grab().toImage();
                QPainter painter(&shot);
                painter.drawImage(viewer->mapTo(&window, QPoint(0, 0)), viewer->captureImage(true));
                painter.end();
                check(shot.save(output + "-ui.png"), "Window capture failed");
                tabs->setCurrentIndex(0);
                QApplication::processEvents();
                auto part = window.grab().toImage();
                QPainter partPainter(&part);
                partPainter.drawImage(viewer->mapTo(&window, QPoint(0, 0)),
                                      viewer->captureImage(true));
                partPainter.end();
                check(part.save(output + "-part.png"), "Part panel capture failed");
                tabs->setCurrentIndex(2);
            }
            const auto revision = c.session().revision();
            check(viewer->presentFlatPattern(first, revision + 1)
                      .contains(QStringLiteral("Неактуальная")),
                  "Stale flat accepted by viewer");
            check(viewer->endBendPreview().isEmpty(), "Return to source failed");
            viewer->highlightFaces({}, revision);
            if (const auto path = qEnvironmentVariable("BENDING_FLAT_SNAPSHOT"); !path.isEmpty()) {
                viewer->setOrientation(viewer::ViewerWidget::Orientation::Isometric);
                viewer->captureImage(true).save(path + "-source.png");
            }
            if (const auto nativeDoc =
                    std::dynamic_pointer_cast<const cad::CadDocument>(c.session().document())) {
                const auto data = cad::OcctDocumentAccess::measurements(*nativeDoc);
                if (data && !data->openings.empty()) {
                    viewer->setOrientation(viewer::ViewerWidget::Orientation::Top);
                    const auto image = viewer->captureImage();
                    int minX = image.width(), maxX = 0, minY = image.height(), maxY = 0;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto color = image.pixelColor(x, y);
                            if (color.red() > 50 && color.green() > 110 &&
                                color.green() > color.red() * 1.35 &&
                                color.blue() > color.green()) {
                                minX = std::min(minX, x);
                                maxX = std::max(maxX, x);
                                minY = std::min(minY, y);
                                maxY = std::max(maxY, y);
                            }
                        }
                    const auto &bounds = nativeDoc->metadata().bounds;
                    const auto project = [&](application::PointMm p) {
                        return QPoint(qRound(minX + (p.x - bounds.minimum.x) * (maxX - minX) /
                                                        (bounds.maximum.x - bounds.minimum.x)),
                                      qRound(maxY - (p.y - bounds.minimum.y) * (maxY - minY) /
                                                        (bounds.maximum.y - bounds.minimum.y))) /
                               canvas->devicePixelRatioF();
                    };
                    const auto measurementEdit = c.session().editRevision();
                    int drawingSelections = 0;
                    const auto connection =
                        QObject::connect(viewer, &viewer::ViewerWidget::selectionChanged, &window,
                                         [&](quint64, int) { ++drawingSelections; });
                    const auto &opening = data->openings.front();
                    const auto rim =
                        std::find_if(data->edges.begin(), data->edges.end(), [&](const auto &edge) {
                            return std::find(opening.edges.begin(), opening.edges.end(), edge.id) !=
                                   opening.edges.end();
                        });
                    check(rim != data->edges.end(), "Source hole has no prepared rim");
                    sendClick(project(rim->first));
                    check(
                        dimensions->accessibleDescription().contains(QStringLiteral("Отверстие")) &&
                            dimensions->accessibleDescription().contains(QStringLiteral("X ")),
                        "Source hole selection lost permanent dimensions or did not use prepared "
                        "hole data");
                    const auto face = std::max_element(
                        data->faces.begin(), data->faces.end(),
                        [](const auto &a, const auto &b) { return a.areaMm2 < b.areaMm2; });
                    if (face != data->faces.end()) {
                        sendClick(project(face->anchor));
                        check(dimensions->accessibleDescription().contains(
                                  QStringLiteral("Плоская грань")),
                              "Source plane selection did not reveal its measured area");
                    }
                    const auto edge = std::max_element(
                        data->edges.begin(), data->edges.end(),
                        [](const auto &a, const auto &b) { return a.lengthMm < b.lengthMm; });
                    if (edge != data->edges.end() && edge->straight) {
                        sendClick(project({(edge->first.x + edge->second.x) / 2,
                                           (edge->first.y + edge->second.y) / 2,
                                           (edge->first.z + edge->second.z) / 2}));
                        check(dimensions->accessibleDescription().contains(QStringLiteral("Ребро")),
                              "Source edge selection did not reveal its actual length");
                    }
                    check(drawingSelections == 0 && c.session().editRevision() == measurementEdit &&
                              !c.hasWork(),
                          "Measurement picking changed drawing/project inputs or started CAD work");
                    QObject::disconnect(connection);
                }
            }
            check(viewer->presentFlatPattern(first, revision).isEmpty(), "Return to flat failed");
            viewer->setDimensionsVisible(false);
            viewer->setDimensionsVisible(true);
            checkFlat->click();
        }
        dxfButton->click();
        auto *liveDxf = window.findChild<QDialog *>("flatDxfDialog");
        check(liveDxf && liveDxf->isVisible(), "Cannot open DXF before editing the part");
        auto *liveSave = liveDxf->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        drawingAction->trigger();
        auto *livePdf = window.findChild<QDialog *>("bendingDrawingDialog");
        check(livePdf, "Cannot open PDF before editing the part");
        auto *livePdfSave = livePdf->findChild<QPushButton *>("exportDrawingPdf");
        wait([&] { return livePdfSave->isEnabled(); });
        livePdfSave->click();
        liveSave->click();
        auto *oldPicker = liveDxf->findChild<QFileDialog *>("flatDxfFileDialog");
        auto *oldPdfPicker = livePdf->findChild<QFileDialog *>("drawingPdfFileDialog");
        check(oldPicker && oldPdfPicker, "Cannot open export choosers before editing the part");
        const auto stalePath = exportDir.filePath("stale.dxf");
        const auto stalePdfPath = exportDir.filePath("stale.pdf");
        oldPicker->selectFile(stalePath);
        oldPdfPicker->selectFile(stalePdfPath);
        setSteelK(.41);
        window.findChild<QPushButton *>("editPartInputs")->click();
        auto *materialChoice = window.findChild<QComboBox *>("sheetMaterial");
        check(materialChoice && c.session().unfoldSettings().manualK == .50 &&
                  c.flatPattern() == first,
              "A catalog change implicitly replaced the applied rule");
        auto *factor = window.findChild<QLineEdit *>("partUnfoldK");
        check(factor && factor->isEnabled(), "No part coefficient editor");
        factor->setText(QStringLiteral("0,42"));
        window.findChild<QPushButton *>("partInputContinue")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!c.flatPattern() && !c.drawing() && !liveSave->isEnabled() &&
                  !livePdfSave->isEnabled() && c.session().unfoldSettings().manualK == .42,
              "Rule change left obsolete exports available");
        check(!liveDxf->findChild<QFileDialog *>("flatDxfFileDialog") &&
                  !livePdf->findChild<QFileDialog *>("drawingPdfFileDialog") &&
                  !QFile::exists(stalePath) && !QFile::exists(stalePdfPath),
              "Editing K did not dismiss both obsolete save dialogs");
        wait([&] { return c.flatPattern() && !c.hasWork(); });
        const auto updatedFlat = c.flatPattern();
        check(updatedFlat && updatedFlat != first &&
                  std::all_of(updatedFlat->info().bends.begin(), updatedFlat->info().bends.end(),
                              [](const auto &bend) { return bend.k == .42; }) &&
                  liveDxf->isVisible() && liveSave->isEnabled(),
              "The new flat and DXF availability did not reflect the part coefficient");
        livePdf->findChild<QPushButton *>("prepareDrawing")->click();
        wait([&] { return livePdfSave->isEnabled(); });
        check(drawingBuilder->prepared == updatedFlat, "PDF did not use the recalculated flat");
        liveSave->click();
        auto *newPicker = liveDxf->findChild<QFileDialog *>("flatDxfFileDialog");
        check(newPicker, "Cannot save the recalculated DXF");
        const auto newPath = exportDir.filePath("updated.dxf");
        newPicker->selectFile(newPath);
        QMetaObject::invokeMethod(newPicker, "accept", Qt::DirectConnection);
        check(QFile::exists(newPath) && liveDxf->isVisible(),
              "Saving the current DXF failed or closed the comparison window");
        const auto partRevision = c.session().revision();
        c.open(native(exportDir.filePath("missing.step")));
        wait([&] { return c.flatPattern() && !c.hasWork(); });
        check(c.session().error() && c.session().revision() == partRevision &&
                  window.findChild<QDialog *>("flatDxfDialog") == liveDxf &&
                  liveSave->isEnabled() && c.flatPattern() != updatedFlat &&
                  c.flatPattern()->info().settings.manualK == .42,
              "A failed import did not restore DXF for the still-open part");
        QTemporaryDir dir;
        check(dir.isValid(), "No temp folder");
        const auto path = native(dir.path()) / "material.bend";
        c.saveProject(path, files);
        wait([&] { return !c.hasWork(); });
        check(c.saveError().isEmpty(), "Save failed");
        c.closeDocument();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!window.findChild<QDialog *>("flatDxfDialog") &&
                  !window.findChild<QDialog *>("bendingDrawingDialog"),
              "Closed part left an export preview");
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().material == core::SheetMaterial::MildSteel &&
                  c.session().unfoldSettings().manualK == .42 && !c.session().modified(),
              "Material/K not portable");
        window.setAutomaticPartInputPrompt(false);
        // Malformed rule data is rejected before it can modify the session.
        QFile saved(qtPath(path));
        check(saved.open(QIODevice::ReadOnly), "Cannot read saved project");
        auto root = project_fixture::legacyRoot(saved.readAll());
        saved.close();
        // An explicitly saved material-table mode must not become the new default constant.
        auto tableRoot = root;
        auto tableParams = tableRoot.value("parameters").toObject();
        auto tableRule = tableParams.value("unfold").toObject();
        tableRule.insert("manualK", QJsonValue::Null);
        tableParams.insert("unfold", tableRule);
        tableRoot.insert("parameters", tableParams);
        const auto tablePath = native(dir.path()) / "table.bend";
        QFile tableFile(qtPath(tablePath));
        check(tableFile.open(QIODevice::WriteOnly), "Cannot write table-mode project");
        tableFile.write(QJsonDocument(tableRoot).toJson());
        tableFile.close();
        c.open(tablePath);
        wait([&] { return !c.hasWork(); });
        window.findChild<QPushButton *>("editPartInputs")->click();
        check(!c.session().unfoldSettings().manualK && !c.session().modified(),
              "Saved table mode was silently replaced by a constant");
        window.findChild<QPushButton *>("partInputViewOnly")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().manualK == .42 && !c.session().modified(),
              "Saved explicit K was replaced by the new default");
        auto params = root.value("parameters").toObject();
        auto rule = params.value("unfold").toObject();
        rule.insert("referenceVersion", 99);
        params.insert("unfold", rule);
        root.insert("parameters", params);
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write test project");
        saved.write(QJsonDocument(root).toJson());
        saved.close();
        const auto original = c.session().document();
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(c.session().document() == original && c.session().error(),
              "Invalid material rule replaced document");
        // Legacy projects have no material; they must not inherit the previous document's rule.
        root.insert("version", 5);
        auto legacyParameters = root.value("parameters").toObject();
        legacyParameters.remove("unfold");
        root.insert("parameters", legacyParameters);
        auto legacyBends = root.value("bends").toArray();
        for (qsizetype i = 0; i < legacyBends.size(); ++i) {
            auto bend = legacyBends[i].toObject();
            bend.remove("workpieceReversed"); // v14 field is invalid in a v5 fixture.
            bend.remove("workpieceOffsetMm");
            legacyBends[i] = bend;
        }
        root.insert("bends", legacyBends);
        check(saved.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "Cannot write legacy project");
        saved.write(QJsonDocument(root).toJson());
        saved.close();
        c.open(path);
        wait([&] { return !c.hasWork(); });
        check(!c.session().error() &&
                  c.session().unfoldSettings().material == core::SheetMaterial::Unknown,
              "Legacy project inherited material");
        // A builder that returns after cancellation must never publish its old result.
        auto slow = std::make_shared<LateBuilder>();
        check(c.prepareFlatPattern(slow), "Slow flat not started");
        wait([&] { return slow->entered.load(); });
        c.closeDocument();
        slow->release = true;
        wait([&] { return !c.hasWork(); });
        check(!c.flatPattern() && !c.session().document(), "Late flat result survived close");
        c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "bend-90-6mm.step");
        wait([&] { return !c.hasWork(); });
        check(c.session().unfoldSettings().material == core::SheetMaterial::Unknown &&
                  !c.session().unfoldSettings().manualK,
              "Material leaked to another STEP");
        window.findChild<QPushButton *>("editPartInputs")->click();
        window.findChild<QComboBox *>("sheetMaterial")->setCurrentIndex(1);
        window.findChild<QPushButton *>("partInputContinue")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(c.session().unfoldSettings().manualK == .41,
              "A fresh STEP did not pick up the latest material preset");
        c.closeDocument();
        c.open(std::filesystem::path(BENDING_EXAMPLE_DIR) / "bend-90-6mm.step");
        wait([&] { return !c.hasWork(); });
        window.findChild<QPushButton *>("editPartInputs")->click();
        window.findChild<QPushButton *>("partInputViewOnly")->click();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(!c.session().unfoldSettings().manualK && !c.session().modified() &&
                  !c.session().hasPartInputs(),
              "Cancelling the default applied unconfirmed calculation inputs");
        c.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
