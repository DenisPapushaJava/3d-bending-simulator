#include "BendFixtures.h"
#include "PreviewRigidScene.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include "bending/viewer/ViewerWidget.h"
#include <AIS_ListOfInteractive.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <STEPControl_Writer.hxx>
#include <V3d_Viewer.hxx>
#include <algorithm>
#include <ctime>
#include <gp_Ax1.hxx>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace bending;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void events() {
    QApplication::processEvents();
}
bool sameImage(const QImage &a, const QImage &b) {
    if (a.isNull() || a.size() != b.size())
        return false;
    // Reinserted objects can change the depth tie at a handful of coincident edge pixels.
    // A stale segment color or missing/misplaced body changes whole areas, not 24 pixels.
    int different = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y) && ++different > 24)
                return false;
    return true;
}
QAction *action(viewer::ViewerWidget &widget, const char *name) {
    auto *result = widget.findChild<QAction *>(QString::fromLatin1(name));
    require(result != nullptr, "Missing viewport action");
    return result;
}
void verifyReuse(const cad::PreviewFrame &frame) {
    Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(new Aspect_DisplayConnection);
    Handle(V3d_Viewer) view = new V3d_Viewer(driver);
    Handle(AIS_InteractiveContext) context = new AIS_InteractiveContext(view);
    const auto objects = [&] {
        AIS_ListOfInteractive result;
        context->ObjectsInside(result);
        return result;
    };
    {
        viewer::PreviewRigidScene scene(context);
        viewer::RigidDisplayBody body{"body", frame.machine.front().shape,
                                      Quantity_Color(Quantity_NOC_BLUE), .4, true};
        body.colors.push_back(
            {body.shape.Located(TopLoc_Location()), Quantity_Color(Quantity_NOC_GREEN)});
        scene.update({body}, AIS_Shaded, true);
        auto first = objects().First();
        const auto origin = body.shape.Location().Transformation().TranslationPart();
        for (int i = 0; i < 100; ++i) {
            gp_Trsf shift;
            shift.SetTranslation(gp_Vec(0, i, 0));
            auto moved = body;
            moved.shape.Move(TopLoc_Location(shift));
            scene.update({moved}, AIS_Shaded, true);
            require(objects().Size() == 1 && objects().First() == first &&
                        first->LocalTransformation().TranslationPart().Y() == origin.Y() + i,
                    "Rigid motion rebuilt a presentation, accumulated objects or lost placement");
        }
        gp_Trsf turn;
        turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), std::numbers::pi);
        auto reversed = body;
        reversed.shape.Move(TopLoc_Location(turn));
        scene.update({reversed}, AIS_Shaded, true);
        require(objects().First() == first, "Rigid turn rebuilt the geometry");
        for (int row = 1; row <= 3; ++row)
            for (int column = 1; column <= 4; ++column)
                require(first->LocalTransformation().Value(row, column) ==
                            reversed.shape.Location().Transformation().Value(row, column),
                        "Rigid turn lost a rotation/translation component");
        body.visible = false;
        scene.update({body}, AIS_Shaded, true);
        require(!context->IsDisplayed(first), "Hidden body remained visible");
        scene.setStyle(AIS_WireFrame, false);
        body.visible = true;
        scene.update({body}, AIS_WireFrame, false);
        require(objects().First() == first && context->IsDisplayed(first) &&
                    first->DisplayMode() == AIS_WireFrame &&
                    !first->Attributes()->FaceBoundaryDraw(),
                "Hidden style update lost reuse or restored stale aspects");
        body.colors.front().color = Quantity_Color(Quantity_NOC_BLUE);
        scene.update({body}, AIS_Shaded, true);
        require(objects().Size() == 1 && objects().First() != first,
                "Changed segment palette retained stale aspects");
        first = objects().First();
        body.color = Quantity_Color(Quantity_NOC_RED);
        scene.update({body}, AIS_Shaded, true);
        require(objects().Size() == 1 && objects().First() != first,
                "Appearance change retained stale colors or leaked objects");
        first = objects().First();
        body.shape = frame.machine.back().shape;
        body.colors.clear();
        scene.update({body}, AIS_Shaded, true);
        require(objects().Size() == 1 && objects().First() != first,
                "Matching ID reused different topology");
        first = objects().First();
        body.shape.Reverse();
        scene.update({body}, AIS_Shaded, true);
        require(objects().Size() == 1 && objects().First() != first,
                "Reversed topology retained the old orientation");
        scene.update({}, AIS_Shaded, true);
        require(objects().IsEmpty(), "Removed bodies stayed in the context");
        scene.update({body}, AIS_Shaded, true);
    }
    require(objects().IsEmpty(), "Scene destruction retained objects in the context");
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir folder;
        require(folder.isValid(), "Temporary folder failed");
        const auto path = folder.path() + "/part.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(), STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(path.toUtf8().constData()) == IFSelect_RetDone,
                "STEP fixture failed");
        const auto loaded = cad::StepImporter{}.load(path.toStdString(), {}, {});
        require(std::holds_alternative<application::DocumentPtr>(loaded), "Import failed");
        const auto document = std::get<application::DocumentPtr>(loaded);
        const auto &bend = document->metadata().bendAnalysis.bends.at(0);
        auto tools = tool_fixture::tangPair();
        tools.machineModel = core::MachineModel::DemoPressV3;
        if (app.arguments().contains("--segmented")) {
            tools.punch.segments = std::vector<core::ToolSegment>{{10, 8}};
            tools.die.segments = std::vector<core::ToolSegment>{{10, 8}};
            tools.punchAssembly = core::ToolAssembly{std::vector<std::size_t>(8, 0), 0};
            tools.dieAssembly = core::ToolAssembly{std::vector<std::size_t>(8, 0), 0};
        }
        const auto built = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
            document, 0, {bend.angleRadians, bend.innerRadiusMm}, tools, {}, 3);
        if (const auto *error = std::get_if<application::BendPreviewError>(&built))
            throw std::runtime_error(error->detail);
        const auto preview = std::get<application::BendPreviewPtr>(built);
        require(preview->frameCount() > 40 &&
                    !cad::OcctPreviewAccess::frame(*preview, 0).machine.empty() &&
                    cad::OcctPreviewAccess::frame(*preview, 0).floor.has_value(),
                "Fixture needs prepared machine, floor and motion");
        verifyReuse(cad::OcctPreviewAccess::frame(*preview, 0));

        viewer::ViewerWidget widget;
        widget.resize(1000, 750);
        widget.show();
        QElapsedTimer wait;
        wait.start();
        while (!widget.isReady() && widget.errorString().isEmpty() && wait.elapsed() < 10000) {
            events();
            QThread::msleep(10);
        }
        require(widget.isReady(), "Native graphics unavailable");
        require(widget.presentDocument(document, 1).isEmpty(), "Cannot present STEP");
        auto present = [&](std::size_t frame) {
            const auto error = widget.presentBendPreview(preview, frame, 1);
            if (!error.isEmpty())
                throw std::runtime_error(error.toStdString());
        };
        present(0);
        widget.fitAll();
        events();
        const auto output = qEnvironmentVariable("BENDING_PREVIEW_AUDIT");
        if (!output.isEmpty())
            require(QDir().mkpath(output), "Cannot create audit folder");
        auto snapshot = [&](const QString &name) {
            const auto image = widget.captureImage();
            require(!image.isNull(), "Framebuffer lost");
            if (!output.isEmpty())
                require(image.save(output + "/" + name + ".png"), "Cannot save framebuffer");
            return image;
        };

        // Warm the same prepared frames, including driver/shader resources. Geometry preparation
        // and framebuffer readback are outside the timed region. No search runs in this loop.
        for (std::size_t i = 0; i < preview->frameCount(); ++i)
            present(i);
        const int rounds = app.arguments().contains("--benchmark") ? 5 : 1;
        std::vector<double> times;
        const auto cpuStart = std::clock();
        for (int round = 0; round < rounds; ++round) {
            for (std::size_t i = 0; i < preview->frameCount(); ++i) {
                QElapsedTimer timer;
                timer.start();
                present(i);
                times.push_back(static_cast<double>(timer.nsecsElapsed()) / 1.e6);
            }
            events();
        }
        std::sort(times.begin(), times.end());
        QJsonObject metrics{{"frames", static_cast<int>(times.size())},
                            {"medianMs", times[times.size() / 2]},
                            {"p95Ms", times[times.size() * 95 / 100]},
                            {"maxMs", times.back()}};
#ifndef _WIN32
        // On Windows std::clock measures wall time; do not label it process CPU there.
        metrics.insert("processCpuMsPerFrame",
                       1000. * (std::clock() - cpuStart) / CLOCKS_PER_SEC / times.size());
#else
        (void)cpuStart;
#endif
        QJsonArray samples;
        for (auto time : times)
            samples.append(time);
        metrics.insert("sortedFrameMs", samples);
        if (!output.isEmpty()) {
            QFile file(output + "/timings.json");
            require(file.open(QIODevice::WriteOnly), "Cannot save timings");
            file.write(QJsonDocument(metrics).toJson());
        }
        metrics.remove("sortedFrameMs");
        std::cout << QJsonDocument(metrics).toJson(QJsonDocument::Compact).constData() << '\n';

        present(0);
        const auto initial = snapshot("initial");
        present(preview->frameCount() / 2);
        const auto forming = snapshot("forming");
        require(initial != forming, "Motion did not update the framebuffer");
        action(widget, "displayWireframe")->trigger();
        snapshot("wireframe");
        action(widget, "displayEdges")->trigger();
        require(widget.captureImage() == forming, "Display style did not restore");
        const auto before = widget.captureImage();
        action(widget, "showMachine")->setChecked(false);
        const auto withoutMachine = snapshot("machine-hidden");
        require(withoutMachine != before, "Machine visibility had no effect");
        action(widget, "displayShaded")->trigger();
        present(preview->frameCount() - 1);
        action(widget, "showMachine")->setChecked(true);
        snapshot("machine-restored-shaded");
        action(widget, "displayEdges")->trigger();
        present(preview->frameCount() / 2);
        require(widget.captureImage() == before,
                "Hidden motion/style changes failed to restore the machine");
        const auto &segments = cad::OcctPreviewAccess::frame(*preview, 0).toolSegments[0];
        require(!segments.empty(), "Fixture lost segment color metadata");
        const auto key = viewer::toolSegmentColorKey(segments.front().lengthMm);
        require(key.has_value(), "Invalid segment color key");
        widget.setToolSegmentColors({{*key, QColor("#DDC060")}});
        require(widget.captureImage() != before, "New segment color was not applied");
        snapshot("palette-changed");
        widget.setToolSegmentColors({});
        const auto reset = snapshot("palette-reset");
        require(sameImage(reset, before), "Segment color reset retained stale aspects");
        widget.sheetOrientationAction()->setChecked(true);
        snapshot("sheet-sides");
        widget.sheetOrientationAction()->setChecked(false);
        require(sameImage(widget.captureImage(), before), "Sheet sides changed the base scene");
        widget.refreshScene();
        require(sameImage(widget.captureImage(), before), "F5 changed geometry or camera");
        auto *canvas = widget.findChild<QWidget *>("cadCanvas");
        require(canvas, "Missing canvas");
        QEvent changed(QEvent::WinIdChange);
        QApplication::sendEvent(canvas, &changed);
        events();
        require(widget.isReady(), "Context recreation failed");
        snapshot("recreated");
        widget.setOrientation(viewer::ViewerWidget::Orientation::Isometric);
        action(widget, "fitMachine")->trigger();
        const auto whole = snapshot("whole-machine");
        action(widget, "showFloor")->setChecked(false);
        require(widget.captureImage() != whole, "Floor visibility had no effect");
        action(widget, "showFloor")->setChecked(true);
        require(sameImage(widget.captureImage(), whole),
                "Floor did not return to the same position");
        require(widget.endBendPreview().isEmpty(), "Cannot return to source CAD");
        snapshot("source");
        present(0);
        widget.clearDocument(2);
        require(widget.presentBendPreview(preview, 0, 1).size() > 0,
                "Cleared revision accepted stale preview");
        std::cout
            << "Prepared preview motion, display mode, F5, context recreation and clear: OK\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
