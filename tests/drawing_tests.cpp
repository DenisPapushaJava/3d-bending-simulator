#include "BendFixtures.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/ui/DrawingSheet.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(condition(), "Drawing operation timed out");
}
std::filesystem::path native(const QString &p) {
#ifdef _WIN32
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(p.toUtf8().constData());
#endif
}
application::DocumentPtr load(const TopoDS_Shape &shape, const QString &path) {
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone, "STEP transfer failed");
    require(writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
            "STEP fixture write failed");
    auto loaded = cad::StepImporter{}.load(native(path), {}, {});
    const auto result = application::resultDocument(loaded);
    require(result != nullptr, "Fixture import failed");
    return result;
}
application::BendingDrawingPtr build(application::DocumentPtr document,
                                     application::DrawingRequest request = {}) {
    const auto result = cad::BendingDrawingBuilder{}.build(document, request, {});
    if (const auto *e = std::get_if<application::DrawingError>(&result))
        throw std::runtime_error(e->detail);
    return std::get<application::BendingDrawingPtr>(result);
}
TopoDS_Shape withVertexGap(double gap) {
    const auto shape = fixtures::bentStrip(8, 6);
    for (TopExp_Explorer vertex(shape, TopAbs_VERTEX); vertex.More(); vertex.Next()) {
        const auto v = TopoDS::Vertex(vertex.Current());
        const auto p = BRep_Tool::Pnt(v);
        if (p.Y() < -29 && p.Z() < .1) {
            BRep_Builder{}.UpdateVertex(v, p.Translated(gp_Vec(0, 0, gap)), gap * 1.5);
            return shape;
        }
    }
    throw std::runtime_error("Fixture has no free flange vertex");
}
void drawingMeasurements(const QString &folder) {
    const auto trimmedDoc = load(fixtures::cornerTrimmedBend(), folder + "/split-cap.step");
    require(trimmedDoc->metadata().bendAnalysis.bends.empty(),
            "Drawing cap support changed strict simulation recognition");
    const auto trimmed = build(trimmedDoc);
    require(trimmed->drawingOnlyRecognition && trimmed->bends.size() == 1 &&
                trimmed->bends[0].trimmedEnds && trimmed->bends[0].interiorAngleRadians &&
                std::abs(*trimmed->bends[0].interiorAngleRadians - std::numbers::pi / 2) < 1e-5 &&
                std::abs(trimmed->bends[0].thicknessMm - 2) < 1e-5,
            "A piecewise cap lost its drawing measurements");
    const auto tolerantDoc = load(withVertexGap(.004), folder + "/tolerant.step");
    require(tolerantDoc->metadata().maximumToleranceMm > .001 &&
                tolerantDoc->metadata().maximumToleranceMm <= .01 &&
                tolerantDoc->metadata().bendAnalysis.status ==
                    application::BendRecognitionStatus::Unsupported,
            "Fixture did not preserve its STEP boundary gap");
    const auto tolerant = build(tolerantDoc);
    require(tolerant->drawingOnlyRecognition && tolerant->bends.size() == 1 &&
                tolerant->bendRecognitionStatus == application::BendRecognitionStatus::Completed &&
                tolerant->thicknessMm && std::abs(*tolerant->thicknessMm - 6) < 1e-5 &&
                tolerantDoc->metadata().bendAnalysis.bends.empty(),
            "Drawing tolerance changed the simulation document or lost dimensions");
    require(ui::DrawingSheet::writePdf(folder + "/tolerant.pdf", *tolerant, {}).isEmpty(),
            "Drawing-only measurement PDF failed");
    const auto mixedDoc = load(fixtures::cornerTrimmedChannel(), folder + "/mixed.step");
    require(mixedDoc->metadata().bendAnalysis.bends.size() == 1,
            "Fixture lost its one strictly supported bend");
    const auto mixed = build(mixedDoc);
    require(mixed->drawingOnlyRecognition && mixed->bends.size() == 2 &&
                mixed->details.size() == 1 && mixed->details[0].fullProfile &&
                std::abs(mixed->bends[0].axialLengthMm -
                         mixedDoc->metadata().bendAnalysis.bends[0].lengthMm) < 1e-5 &&
                mixedDoc->metadata().bendAnalysis.bends.size() == 1,
            "Drawing-only additions lost or replaced a strictly recognized zone");
    const auto excessive = build(load(withVertexGap(.025), folder + "/excessive.step"));
    require(excessive->bends.empty() && !excessive->drawingOnlyRecognition &&
                excessive->bendRecognitionStatus ==
                    application::BendRecognitionStatus::Unsupported &&
                !ui::DrawingSheet::preview(*excessive, {}).isNull(),
            "Drawing measurements accepted excessive source tolerance");
}
void geometry(const QString &folder) {
    const auto source = fixtures::bentStrip(8, 6);
    const auto document = load(source, folder + "/base.step");
    const auto drawing = build(document);
    require(drawing->bends.size() == 1 && drawing->thicknessMm &&
                std::abs(*drawing->thicknessMm - 6) < 1e-5,
            "Bend facts lost");
    const auto &b = drawing->bends[0];
    require(b.interiorAngleRadians &&
                std::abs(*b.interiorAngleRadians - std::numbers::pi / 2) < 1e-5 &&
                drawing->views[2].angles.size() == 1,
            "A right-angle profile lost its angular dimension");
    require(drawing->views[2].dimensions.size() == 3 &&
                std::any_of(
                    drawing->views[2].dimensions.begin(), drawing->views[2].dimensions.end(),
                    [](const auto &d) { return d.label == "t" && std::abs(d.valueMm - 6) < 1e-5; }),
            "Profile view lost flange/thickness dimensions");
    require(b.outerFlangeMm[0] && b.outerFlangeMm[1], "Outer flange sizes missing");
    require(std::abs(*b.outerFlangeMm[0] - 44) < 1e-4 && std::abs(*b.outerFlangeMm[1] - 44) < 1e-4,
            "Outer height confused with 30 mm straight flange");
    for (const auto &v : drawing->views)
        require(!v.paths.empty(), "Missing projected geometry");
    gp_Trsf rotate;
    rotate.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .734);
    rotate.SetTranslationPart(gp_Vec(117, -53, 82));
    const auto moved =
        load(BRepBuilderAPI_Transform(source, rotate, true).Shape(), folder + "/rotated.step");
    const auto rotated = build(moved);
    for (int i = 0; i < 3; ++i) {
        const auto &a = drawing->views[i];
        const auto &v = rotated->views[i];
        require(std::abs((a.maximum.x - a.minimum.x) - (v.maximum.x - v.minimum.x)) < 1e-3 &&
                    std::abs((a.maximum.y - a.minimum.y) - (v.maximum.y - v.minimum.y)) < 1e-3,
                "STEP world axes changed drawing dimensions");
    }
    require(application::drawingScale(*drawing) == application::drawingScale(*rotated),
            "Rotated STEP changed scale");
    const auto longDoc =
        load(fixtures::bentStrip(8, 6, std::numbers::pi / 2, 2000), folder + "/long.step");
    const auto shortened = build(longDoc);
    require(shortened->views[0].omittedX && shortened->views[1].omittedX &&
                !shortened->views[2].omittedX && !shortened->views[3].omittedX,
            "Uniform long section did not get consistent breaks");
    require(std::abs(shortened->views[0].dimensions[0].valueMm - 2000) < 1e-3,
            "Break changed full dimension");
    auto disabled = application::DrawingRequest{};
    disabled.allowBreaks = false;
    require(!build(longDoc, disabled)->views[0].omittedX, "Break opt-out ignored");
    const auto acute = build(
        load(fixtures::bentStrip(4, 2, 2 * std::numbers::pi / 3, 2000), folder + "/acute.step"),
        disabled);
    require(acute->bends.size() == 1 && acute->bends[0].interiorAngleRadians &&
                std::abs(*acute->bends[0].interiorAngleRadians - std::numbers::pi / 3) < 1e-5 &&
                std::abs(acute->bends[0].turnRadians - 2 * std::numbers::pi / 3) < 1e-5,
            "A 120 degree bend turn was confused with the 60 degree interior angle");
    require(acute->details.size() == 1 && !acute->details[0].view.sectionLoops.empty() &&
                acute->details[0].view.angles.size() == 1 &&
                ui::DrawingSheet::pageCount(*acute) == 1,
            "A small bend in a large part has no enlarged section");
    require(!ui::DrawingSheet::preview(*acute, {}, 0).isNull(),
            "Enlarged non-right-angle section cannot be laid out");
    require(ui::DrawingSheet::writePdf(folder + "/acute.pdf", *acute, {}).isEmpty(),
            "Enlarged section PDF failed");
    const auto wider =
        build(load(fixtures::bentStrip(4, 2, 2 * std::numbers::pi / 3, 2000, true, 100),
                   folder + "/wide-flanges.step"),
              disabled);
    require(wider->details.size() == 1 && wider->details[0].view.dimensions.size() == 3 &&
                wider->details[0].view.bendNotes.size() == 1,
            "The detail crop dropped valid flange or thickness dimensions");
    const auto holed = BRepAlgoAPI_Cut(fixtures::bentStrip(8, 6, std::numbers::pi / 2, 2000),
                                       BRepPrimAPI_MakeCylinder(
                                           gp_Ax2(gp_Pnt(-20, 7, 1000), gp_Dir(0, 1, 0)), 2, 8))
                           .Shape();
    const auto withHole = build(load(holed, folder + "/hole.step"));
    require(!withHole->views[0].omittedX, "Break hid a hole");
    require(std::any_of(withHole->views.begin(), withHole->views.end(),
                        [](const auto &v) { return !v.circleCenters.empty(); }),
            "A full circular hole lost its centerlines");
    require(std::any_of(drawing->views.begin(), drawing->views.end(),
                        [](const auto &v) {
                            return std::any_of(v.paths.begin(), v.paths.end(),
                                               [](const auto &p) { return p.hidden; });
                        }),
            "Hidden contours were not classified");
    application::DrawingRequest bad;
    bad.baseFace = 100000;
    require(std::holds_alternative<application::DrawingError>(
                cad::BendingDrawingBuilder{}.build(document, bad, {})),
            "Foreign face accepted");
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = cad::BendingDrawingBuilder{}.build(document, {}, stop.get_token());
    require(std::get<application::DrawingError>(cancelled).cancelled, "Cancellation ignored");
    const auto reverse = [&] {
        application::DrawingRequest r;
        r.reverseView = true;
        return build(document, r);
    }();
    require(reverse->reverseView && reverse->bends[0].outerFlangeMm == b.outerFlangeMm,
            "Changing viewing side changed measured geometry");
    for (int turn = 0; turn < 4; ++turn) {
        application::DrawingRequest r;
        r.quarterTurns = turn;
        const auto turned = build(document, r);
        require(!ui::DrawingSheet::preview(*turned, {}).isNull(),
                "Quarter-turn orientation cannot place dimensions");
    }
    application::DrawingRequest manual;
    manual.manualThicknessMm = 5;
    const auto conflict = build(document, manual);
    require(conflict->declaredThicknessMm == 5 && conflict->thicknessMm &&
                std::abs(*conflict->thicknessMm - 6) < 1e-5 && !conflict->manualThickness,
            "Manual thickness replaced measured CAD thickness");
    ui::DrawingTitle title{QStringLiteral("Длинная деталь"),
                           QStringLiteral("ПР-001"),
                           {},
                           QStringLiteral("Исполнение Л"),
                           37};
    const auto path = qEnvironmentVariable("BENDING_DRAWING_LONG_OUTPUT");
    if (!path.isEmpty())
        require(ui::DrawingSheet::writePdf(path, *shortened, title).isEmpty(),
                "Long demo PDF failed");
    const auto channel = build(load(fixtures::channel(), folder + "/channel.step"));
    require(channel->bends.size() == 2 && channel->details.size() == 1 &&
                channel->details[0].fullProfile && channel->details[0].view.angles.size() == 2 &&
                channel->views[0].sectionMarkers.size() == 1 &&
                std::any_of(channel->details[0].view.dimensions.begin(),
                            channel->details[0].view.dimensions.end(),
                            [](const auto &d) { return std::abs(d.valueMm - 92) < 1e-4; }) &&
                ui::DrawingSheet::pageCount(*channel) == 1 &&
                !ui::DrawingSheet::preview(*channel, {}).isNull(),
            "Connected bends lost the whole profile or corner-to-corner dimension");
    const auto longChannel = build(load(fixtures::channel(2000), folder + "/long-channel.step"));
    require(longChannel->details.size() == 1 && longChannel->details[0].fullProfile &&
                !longChannel->views[0].omittedX && ui::DrawingSheet::pageCount(*longChannel) == 1 &&
                !ui::DrawingSheet::preview(*longChannel, {}).isNull(),
            "A whole section was marked inside an omitted interval");
    const auto movedChannel =
        load(BRepBuilderAPI_Transform(fixtures::channel(), rotate, true).Shape(),
             folder + "/channel-rotated.step");
    for (int turn = 0; turn < 4; ++turn) {
        application::DrawingRequest r;
        r.quarterTurns = turn;
        r.reverseView = turn % 2 != 0;
        const auto changed = build(movedChannel, r);
        require(changed->details.size() == 1 && changed->details[0].fullProfile &&
                    !ui::DrawingSheet::preview(*changed, {}).isNull(),
                "A whole section cannot be laid out after changing the viewing direction");
    }
    auto many = *acute;
    for (std::size_t i = 1; i < 31; ++i) {
        many.bends.push_back(acute->bends[0]);
        auto detail = acute->details[0];
        detail.bendIndex = i;
        detail.designation = "А" + std::to_string(i + 1);
        for (auto &[point, index] : detail.view.bendNotes)
            index = i;
        many.details.push_back(std::move(detail));
    }
    require(ui::DrawingSheet::pageCount(many) == 8, "Pagination lost enlarged profiles");
    require(ui::DrawingSheet::writePdf(folder + "/many.pdf", many, title).isEmpty(),
            "Multi-page PDF failed");
    title.quantity = 0;
    require(!ui::DrawingSheet::writePdf(folder + "/bad.pdf", *drawing, title).isEmpty() &&
                !QFile::exists(folder + "/bad.pdf"),
            "Invalid quantity wrote PDF");
    const auto privatePart = qEnvironmentVariable("BENDING_TEST_DRAWING_PART");
    if (!privatePart.isEmpty()) {
        const auto imported = cad::StepImporter{}.load(native(privatePart), {}, {});
        const auto part = application::resultDocument(imported);
        require(part != nullptr, "Private drawing fixture import failed");
        const auto result = build(part);
        title.quantity = 12;
        require(ui::DrawingSheet::writePdf(folder + "/private.pdf", *result, title).isEmpty(),
                "Private part drawing failed");
    }
}
void workflow(const QString &folder) {
    auto importer = std::make_shared<cad::StepImporter>();
    ui::MainWindow window({"test", "test"}, importer);
    auto builder = std::make_shared<cad::BendingDrawingBuilder>();
    window.setDrawingBuilder(builder);
    window.show();
    auto &c = window.importController();
    window.openStep(QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step");
    wait([&] { return !c.hasWork(); });
    require(c.session().document() != nullptr, "Workflow import failed");
    const auto revision = c.session().editRevision();
    auto *action = window.findChild<QAction *>("bendingDrawingAction");
    require(action && action->isEnabled(), "Drawing action missing");
    bool inspected = false, pdf = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "bendingDrawingDialog" || c.hasWork())
            return;
        auto *exportButton = dialog->findChild<QPushButton *>("exportDrawingPdf");
        if (!exportButton || !exportButton->isEnabled())
            return;
        timer.stop();
        auto *qty = dialog->findChild<QSpinBox *>("drawingQuantity");
        qty->setValue(37);
        dialog->findChild<QLineEdit *>("drawingExecution")->setText(QStringLiteral("Исполнение Л"));
        inspected = !dialog->findChild<QLabel *>("drawingPreviewImage")->pixmap().isNull() &&
                    c.session().editRevision() == revision;
        const auto snapshot = qEnvironmentVariable("BENDING_DRAWING_DIALOG_SNAPSHOT");
        if (!snapshot.isEmpty())
            inspected = inspected && dialog->grab().save(snapshot);
        const auto output = qEnvironmentVariable("BENDING_DRAWING_OUTPUT");
        const auto target = output.isEmpty() ? folder + "/ui.pdf" : output;
        QTimer::singleShot(30, dialog, [target] {
            auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!file)
                return;
            file->setOption(QFileDialog::DontConfirmOverwrite, true);
            file->selectFile(target);
            if (auto *input = file->findChild<QLineEdit *>("fileNameEdit"))
                input->setText(target);
            QTimer::singleShot(0, file, [file] {
                QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection);
            });
        });
        exportButton->click();
        pdf = QFile::exists(target);
        dialog->reject();
    });
    timer.start(20);
    QTimer::singleShot(30000, &window, [&] {
        if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            d->reject();
    });
    action->trigger();
    require(inspected && pdf, "Dialog/quantity/PDF workflow failed");
    {
        const auto output = qEnvironmentVariable("BENDING_DRAWING_OUTPUT");
        QFile file(output.isEmpty() ? folder + "/ui.pdf" : output);
        require(file.open(QIODevice::ReadOnly), "Cannot reopen exported PDF");
        const auto bytes = file.readAll();
        const auto match =
            QRegularExpression(
                QStringLiteral("/MediaBox\\s*\\[\\s*0\\s+0\\s+([0-9.]+)\\s+([0-9.]+)\\s*\\]"))
                .match(QString::fromLatin1(bytes));
        require(bytes.startsWith("%PDF-") && bytes.contains("/FontFile2") && match.hasMatch() &&
                    std::abs(match.captured(1).toDouble() * 25.4 / 72 - 420) < .5 &&
                    std::abs(match.captured(2).toDouble() * 25.4 / 72 - 297) < .5,
                "PDF lost A3 landscape or embedded TrueType font");
    }
    require(c.session().editRevision() == revision, "Drawing metadata edited project");
    const auto old = c.drawing();
    require(c.prepareDrawing(builder, {}) && !c.hasWork() && c.drawing() == old,
            "Reopening recomputed current drawing");
    application::DrawingRequest another;
    another.reverseView = true;
    require(c.prepareDrawing(builder, another), "Second drawing did not start");
    c.closeDocument();
    wait([&] { return !c.hasWork(); });
    require(!c.drawing() && !c.session().document(), "Stale drawing published after close");
    window.close();
}
} // namespace
int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir temp;
    try {
        drawingMeasurements(temp.path());
        geometry(temp.path());
        workflow(temp.path());
        std::cout << "Drawing geometry, layout and PDF workflow passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
