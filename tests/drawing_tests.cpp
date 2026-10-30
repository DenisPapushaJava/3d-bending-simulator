#include "BendFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/FlatPatternAccess.h"
#include "bending/ui/DrawingSheet.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/MainWindow.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Circ.hxx>
#include <iostream>
#include <numbers>
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
void requireCompactOrientation(const QImage &preview) {
    const double ratio = preview.width() / ui::DrawingSheet::widthMm;
    int pixels = 0;
    for (int y = static_cast<int>(259 * ratio); y < static_cast<int>(288 * ratio); ++y)
        for (int x = static_cast<int>(25 * ratio); x < static_cast<int>(220 * ratio); ++x)
            if (qGray(preview.pixel(x, y)) < 160)
                ++pixels;
    require(pixels > 100, "The sheet lost its compact isometric geometry below the caption");
}
void drawingMeasurements(const QString &folder) {
    const auto trimmedDoc = load(fixtures::cornerTrimmedBend(), folder + "/split-cap.step");
    require(trimmedDoc->metadata().bendAnalysis.bends.size() == 1,
            "A composite cap was not recognized");
    const auto trimmed = build(trimmedDoc);
    require(!trimmed->drawingOnlyRecognition && trimmed->bends.size() == 1 &&
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
    require(mixedDoc->metadata().bendAnalysis.bends.size() == 2, "Fixture lost a recognized bend");
    const auto mixed = build(mixedDoc);
    require(!mixed->drawingOnlyRecognition && mixed->bends.size() == 2 &&
                mixed->details.size() == 1 && mixed->details[0].fullProfile &&
                std::abs(mixed->bends[0].axialLengthMm -
                         mixedDoc->metadata().bendAnalysis.bends[0].lengthMm) < 1e-5 &&
                mixedDoc->metadata().bendAnalysis.bends.size() == 2,
            "Drawing-only additions lost or replaced a strictly recognized zone");
    const auto excessive = build(load(withVertexGap(.025), folder + "/excessive.step"));
    require(excessive->bends.empty() && !excessive->drawingOnlyRecognition &&
                excessive->bendRecognitionStatus ==
                    application::BendRecognitionStatus::Unsupported &&
                !ui::DrawingSheet::preview(*excessive, {}).isNull(),
            "Drawing measurements accepted excessive source tolerance");
}
void holePositions(const QString &folder) {
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(300, 120, 6).Shape();
    for (const double x : {150., 270.})
        plate = BRepAlgoAPI_Cut(plate, BRepPrimAPI_MakeCylinder(
                                           gp_Ax2(gp_Pnt(x, 40, -1), gp_Dir(0, 0, 1)), 4, 8))
                    .Shape();
    BRepBuilderAPI_MakeWire splitRim;
    const gp_Circ rim(gp_Ax2(gp_Pnt(30, 40, -1), gp_Dir(0, 0, 1)), 4);
    splitRim.Add(BRepBuilderAPI_MakeEdge(rim, 0, std::numbers::pi / 2));
    splitRim.Add(BRepBuilderAPI_MakeEdge(rim, std::numbers::pi / 2, 2 * std::numbers::pi));
    plate = BRepAlgoAPI_Cut(plate, BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(splitRim.Wire()),
                                                         gp_Vec(0, 0, 8)))
                .Shape();
    // Concentric counterbore, an open edge notch and a non-round internal cutout.
    plate = BRepAlgoAPI_Cut(
                plate, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(150, 40, 4), gp_Dir(0, 0, 1)), 7, 3))
                .Shape();
    plate = BRepAlgoAPI_Cut(
                plate, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 60, -1), gp_Dir(0, 0, 1)), 4, 8))
                .Shape();
    plate = BRepAlgoAPI_Cut(plate, BRepPrimAPI_MakeBox(gp_Pnt(80, 80, -1), 20, 15, 8)).Shape();
    const auto document = load(plate, folder + "/hole-coordinates.step");
    const auto drawing = build(document);
    const auto &dimensions = drawing->views[0].holeDimensions;
    require(drawing->classicalViews &&
                (*drawing->classicalViews)[0].holeDimensions.size() == dimensions.size() &&
                std::none_of((*drawing->classicalViews)[0].holeDimensions.begin(),
                             (*drawing->classicalViews)[0].holeDimensions.end(),
                             [](const auto &d) { return d.pitchIntervals != 0; }),
            "Unequal hole spacing was replaced by a regular pitch");
    require(dimensions.size() == 6 && drawing->views[0].openingAxes.size() == 1,
            "Shared coordinates repeated, rectangle omitted or open notch included");
    const std::array expectedX{30., 90., 150., 270.};
    for (std::size_t i = 0; i < expectedX.size(); ++i)
        require(std::abs(dimensions[i].valueMm - expectedX[i]) < 1e-4 &&
                    std::abs(dimensions[i].start.x - drawing->views[0].minimum.x) < 1e-5,
                "Hole axis was not measured from the projected overall datum");
    require((std::abs(dimensions[4].valueMm - 40) < 1e-4 &&
             std::abs(dimensions[5].valueMm - 87.5) < 1e-4) ||
                (std::abs(dimensions[4].valueMm - 32.5) < 1e-4 &&
                 std::abs(dimensions[5].valueMm - 80) < 1e-4),
            "Hole row or rectangle lost its transverse coordinate");
    require(drawing->views[1].holeDimensions.empty() && drawing->views[2].holeDimensions.empty() &&
                drawing->views[3].holeDimensions.empty(),
            "An oblique/edge-on projection received circular hole coordinates");
    require(ui::DrawingSheet::writePdf(folder + "/hole-coordinates.pdf", *drawing, {}).isEmpty(),
            "Hole dimensions cannot be placed with the existing overall dimensions");
    application::DrawingRequest request;
    request.reverseView = true;
    const auto reverse = build(document, request);
    require(reverse->views[0].holeDimensions.size() == 6 &&
                std::abs(reverse->views[0].holeDimensions[4].valueMm + dimensions[5].valueMm -
                         120) < 1e-4 &&
                std::abs(reverse->views[0].holeDimensions[5].valueMm + dimensions[4].valueMm -
                         120) < 1e-4,
            "Reverse view retained the old hole datum distance");
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .734);
    transform.SetTranslationPart(gp_Vec(117, -53, 82));
    const auto moved = build(load(BRepBuilderAPI_Transform(plate, transform, true).Shape(),
                                  folder + "/rotated-holes.step"));
    require(moved->views[0].holeDimensions.size() == dimensions.size(),
            "Rotated STEP lost circular hole positions");
    for (std::size_t i = 0; i < dimensions.size(); ++i)
        require(std::abs(moved->views[0].holeDimensions[i].valueMm - dimensions[i].valueMm) < 1e-4,
                "STEP world coordinates changed hole dimensions");
    for (int turn = 0; turn < 4; ++turn) {
        request.quarterTurns = turn;
        const auto turned = build(document, request);
        require(turned->views[0].holeDimensions.size() == dimensions.size() &&
                    !ui::DrawingSheet::preview(*turned, {}).isNull(),
                "A quarter-turn view cannot place hole coordinates");
    }
    const auto boss = build(load(BRepPrimAPI_MakeCylinder(10, 6).Shape(), folder + "/boss.step"));
    require(std::all_of(boss->views.begin(), boss->views.end(),
                        [](const auto &v) { return v.holeDimensions.empty(); }),
            "A round external boundary was treated as a hole");
    const auto &overall = boss->views[0].dimensions[0];
    require(overall.witnesses && std::abs(overall.valueMm - 20) < 1e-4 &&
                std::abs((*overall.witnesses)[0].x - overall.start.x) < 1e-4 &&
                std::abs((*overall.witnesses)[1].x - overall.end.x) < 1e-4 &&
                std::abs((*overall.witnesses)[0].y - (*overall.witnesses)[1].y) < 1e-4 &&
                std::abs((*overall.witnesses)[0].y - overall.start.y) > 9,
            "Overall extension lines start at empty bounding corners instead of the contour");
    TopoDS_Shape masked = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(100, 80, 6),
                                          BRepPrimAPI_MakeCylinder(
                                              gp_Ax2(gp_Pnt(50, 40, -1), gp_Dir(0, 0, 1)), 10, 8))
                              .Shape();
    masked = BRepAlgoAPI_Fuse(masked, BRepPrimAPI_MakeBox(gp_Pnt(65, 30, 6), 5, 20, 6)).Shape();
    masked = BRepAlgoAPI_Fuse(masked, BRepPrimAPI_MakeBox(gp_Pnt(50, 30, 10), 20, 20, 2)).Shape();
    const auto maskedDoc = load(masked, folder + "/partially-hidden-hole.step");
    const auto visible = build(maskedDoc);
    application::DrawingRequest otherSide;
    otherSide.reverseView = true;
    const auto obscured = build(maskedDoc, otherSide);
    require((visible->views[0].holeDimensions.size() == 2 &&
             obscured->views[0].holeDimensions.empty()) ||
                (obscured->views[0].holeDimensions.size() == 2 &&
                 visible->views[0].holeDimensions.empty()),
            "Overlapping visible arc fragments falsely completed a partially hidden circle");
    const auto bent = build(load(fixtures::profiledBend(), folder + "/perforated-bend.step"));
    require(!bent->views[0].holeDimensions.empty() &&
                !ui::DrawingSheet::preview(*bent, {}).isNull(),
            "Hole coordinates conflict with enlarged bend profiles");
    const auto output = qEnvironmentVariable("BENDING_DRAWING_HOLES_OUTPUT");
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output, *drawing, {}).isEmpty(),
                "Hole coordinates example PDF failed");
}
void ovalVisibility(const QString &folder) {
    const auto rectangular = [&](double x, const QString &name) {
        return build(load(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(100, 60, 4),
                                          BRepPrimAPI_MakeBox(gp_Pnt(x, 20, -1), 16, 8, 6))
                              .Shape(),
                          folder + name));
    };
    const auto rectangle = rectangular(20, "/rectangular-cutout.step");
    const auto shifted = rectangular(25, "/rectangular-shifted.step");
    require(rectangle->views[0].openingAxes.size() == 1 &&
                rectangle->views[0].holeDimensions.size() == 2 &&
                shifted->views[0].holeDimensions.size() == 2,
            "A rectangular cutout lost its center coordinates");
    std::vector<double> changes;
    for (std::size_t i = 0; i < 2; ++i)
        changes.push_back(std::abs(rectangle->views[0].holeDimensions[i].valueMm -
                                   shifted->views[0].holeDimensions[i].valueMm));
    std::sort(changes.begin(), changes.end());
    require(changes[0] < 1e-4 && std::abs(changes[1] - 5) < 1e-4,
            "A 5 mm cutout shift is invisible in the position dimensions");
    require(ui::DrawingSheet::writePdf(folder + "/rectangular.pdf", *rectangle, {}).isEmpty(),
            "Rectangular cutout coordinates cannot be laid out");
    const auto ellipse =
        build(load(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(80, 40, 4),
                                   BRepPrimAPI_MakeCylinder(
                                       gp_Ax2(gp_Pnt(20, 20, -1), gp_Dir(.25, 0, 1)), 4, 10))
                       .Shape(),
                   folder + "/elliptical-mouth.step"));
    require(ellipse->views[0].openingAxes.size() == 1 &&
                ellipse->views[0].holeDimensions.size() == 2,
            "A true planar ellipse lost its symmetry center");
    require(ui::DrawingSheet::writePdf(folder + "/ellipse.pdf", *ellipse, {}).isEmpty(),
            "Ellipse PDF failed");
    auto cutter = BRepAlgoAPI_Fuse(
                      BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(45, 40, -1), gp_Dir(0, 0, 1)), 5, 8),
                      BRepPrimAPI_MakeBox(gp_Pnt(45, 35, -1), 10, 10, 8))
                      .Shape();
    cutter = BRepAlgoAPI_Fuse(cutter, BRepPrimAPI_MakeCylinder(
                                          gp_Ax2(gp_Pnt(55, 40, -1), gp_Dir(0, 0, 1)), 5, 8))
                 .Shape();
    auto masked = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(100, 80, 6), cutter).Shape();
    masked = BRepAlgoAPI_Fuse(masked, BRepPrimAPI_MakeBox(gp_Pnt(65, 30, 6), 5, 20, 6)).Shape();
    masked = BRepAlgoAPI_Fuse(masked, BRepPrimAPI_MakeBox(gp_Pnt(50, 30, 10), 20, 20, 2)).Shape();
    const auto doc = load(masked, folder + "/masked-oval.step");
    application::DrawingRequest reverse;
    reverse.reverseView = true;
    const auto first = build(doc), second = build(doc, reverse);
    require(
        (first->views[0].visibleOpeningCount == 1 && second->views[0].visibleOpeningCount == 0) ||
            (second->views[0].visibleOpeningCount == 1 && first->views[0].visibleOpeningCount == 0),
        "Duplicate arc fragments completed a partly hidden oval");
    auto asymmetric = BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(
                                           gp_Ax2(gp_Pnt(30, 20, -1), gp_Dir(0, 0, 1)), 4, 8),
                                       BRepPrimAPI_MakeBox(gp_Pnt(30, 16, -1), 10, 8, 8))
                          .Shape();
    asymmetric =
        BRepAlgoAPI_Fuse(
            asymmetric, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(40, 20, -1), gp_Dir(0, 0, 1)), 6, 8))
            .Shape();
    const auto unknown =
        build(load(BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(80, 40, 6), asymmetric).Shape(),
                   folder + "/asymmetric-opening.step"));
    require(unknown->views[0].holeDimensions.empty(),
            "An arbitrary bounding-box midpoint was called an oval axis");
}

TopoDS_Shape taperedChannel() {
    const double pi = std::numbers::pi, angle = pi / 60, extent = 400;
    const auto part = [&](bool right) {
        BRepBuilderAPI_MakeWire wire;
        const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b)); };
        const auto arc = [&](double x, double y, double r, double a, double b) {
            wire.Add(
                BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(fixtures::polar(r, a, x, y),
                                                           fixtures::polar(r, (a + b) / 2, x, y),
                                                           fixtures::polar(r, b, x, y))
                                            .Value()));
        };
        if (right) {
            arc(80, 6, 6, 1.5 * pi, 2 * pi);
            line({86, 6, 0}, {86, 40, 0});
            arc(80, 40, 6, 0, pi / 2);
            line({80, 46, 0}, {60, 46, 0});
            line({60, 46, 0}, {60, 44, 0});
            line({60, 44, 0}, {80, 44, 0});
            arc(80, 40, 4, pi / 2, 0);
            line({84, 40, 0}, {84, 6, 0});
            arc(80, 6, 4, 0, -pi / 2);
            line({80, 2, 0}, {80, 0, 0});
        } else {
            line({-6, 40, 0}, {-6, 6, 0});
            arc(0, 6, 6, pi, 1.5 * pi);
            line({0, 0, 0}, {0, 2, 0});
            arc(0, 6, 4, 1.5 * pi, pi);
            line({-4, 6, 0}, {-4, 40, 0});
            line({-4, 40, 0}, {-6, 40, 0});
        }
        return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()),
                                     gp_Vec(0, 0, right ? extent * std::cos(angle) : extent))
            .Shape();
    };
    gp_Trsf tilt;
    tilt.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), angle);
    const auto left = BRepBuilderAPI_Transform(part(false), tilt, true).Shape();
    BRepBuilderAPI_MakeWire panel;
    const std::array<gp_Pnt, 4> points{{{0, 0, 0},
                                        {80, 0, 0},
                                        {80, 0, extent * std::cos(angle)},
                                        {extent * std::sin(angle), 0, extent * std::cos(angle)}}};
    for (std::size_t i = 0; i < points.size(); ++i)
        panel.Add(BRepBuilderAPI_MakeEdge(points[i], points[(i + 1) % points.size()]));
    const auto middle =
        BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(panel.Wire()), gp_Vec(0, 2, 0)).Shape();
    auto shape = BRepAlgoAPI_Fuse(BRepAlgoAPI_Fuse(left, middle).Shape(), part(true)).Shape();
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, true);
    unify.Build();
    shape = unify.Shape();
    for (double z : {100., 250.}) {
        auto cutter = BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(
                                           gp_Ax2(gp_Pnt(83, 20, z - 4), gp_Dir(1, 0, 0)), 3, 4),
                                       BRepPrimAPI_MakeBox(gp_Pnt(83, 17, z - 4), 4, 6, 8))
                          .Shape();
        cutter = BRepAlgoAPI_Fuse(cutter, BRepPrimAPI_MakeCylinder(
                                              gp_Ax2(gp_Pnt(83, 20, z + 4), gp_Dir(1, 0, 0)), 3, 4))
                     .Shape();
        if (z == 100)
            cutter =
                BRepAlgoAPI_Fuse(cutter, BRepPrimAPI_MakeCylinder(
                                             gp_Ax2(gp_Pnt(83, 20, z), gp_Dir(1, 0, 0)), 3.5, 4))
                    .Shape();
        shape = BRepAlgoAPI_Cut(shape, cutter).Shape();
    }
    return shape;
}
void variableProfile(const QString &folder) {
    const auto source = taperedChannel();
    const auto document = load(source, folder + "/variable-channel.step");
    require(document->metadata().bendAnalysis.bends.size() == 3,
            "Variable channel lost its three analytic bends");
    const auto drawing = build(document);
    require(drawing->variableProfile == application::DrawingVariableProfile::Measured,
            "A measured variable profile is reported as incomplete");
    require(drawing->details.size() == 2 && drawing->details[0].fullProfile &&
                drawing->details[1].fullProfile && drawing->views[0].sectionMarkers.size() == 2 &&
                drawing->views[0].sectionDimensions.size() == 2 &&
                drawing->views[0].directionNotes.size() == 1,
            "Variable wall has no two positioned true sections");
    require(std::abs(drawing->views[0].directionNotes[0].radians - std::numbers::pi / 60) < 1e-6,
            "Nonparallel bend directions were silently merged");
    double first = 0;
    for (const auto &d : drawing->details[0].view.dimensions)
        if (d.label.starts_with("H"))
            first = std::max(first, d.valueMm);
    require(drawing->details[1].view.dimensions.size() == 1,
            "Second section repeats all unchanged dimensions");
    const double difference = std::abs(first - drawing->details[1].view.dimensions[0].valueMm);
    const auto &positions = drawing->views[0].sectionDimensions;
    const double separation = std::abs(positions[0].valueMm - positions[1].valueMm);
    require(std::abs(difference / separation - std::tan(std::numbers::pi / 60)) < 1e-5,
            "Variable dimensions are global extrema rather than station measurements");
    require(drawing->views[1].openingAxes.size() == 2 &&
                drawing->views[1].holeDimensions.size() == 3 &&
                drawing->unrecognizedCylindricalFaces == 0,
            "Oval cutouts lost axes or masquerade as unrecognized bend walls");
    require(ui::DrawingSheet::pageCount(*drawing) == 1 &&
                ui::DrawingSheet::writePdf(folder + "/variable.pdf", *drawing, {}).isEmpty(),
            "Two sections, stations and oval coordinates do not fit A3");
    requireCompactOrientation(ui::DrawingSheet::preview(*drawing, {}, 0, 1680));
    const auto output = qEnvironmentVariable("BENDING_DRAWING_VARIABLE_OUTPUT");
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output, *drawing, {}).isEmpty(),
                "Variable example PDF failed");
    for (int turn = 0; turn < 4; ++turn) {
        application::DrawingRequest request;
        request.quarterTurns = turn;
        request.reverseView = turn % 2 != 0;
        const auto rotated = build(document, request);
        require(rotated->variableProfile == application::DrawingVariableProfile::Measured &&
                    rotated->views[0].sectionDimensions.size() == 2 &&
                    std::abs(rotated->views[0].sectionDimensions[0].valueMm -
                             rotated->views[0].sectionDimensions[1].valueMm) > 1,
                "Turning the drawing lost the variable section positions");
        require(!ui::DrawingSheet::preview(*rotated, {}).isNull(),
                "Turned variable drawing cannot be laid out");
    }
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .734);
    transform.SetTranslationPart(gp_Vec(117, -53, 82));
    const auto moved = build(load(BRepBuilderAPI_Transform(source, transform, true).Shape(),
                                  folder + "/variable-rotated.step"));
    require(moved->variableProfile == application::DrawingVariableProfile::Measured &&
                moved->views[0].sectionDimensions.size() == 2,
            "World rotation lost variable stations");
    const double movedSeparation = std::abs(moved->views[0].sectionDimensions[0].valueMm -
                                            moved->views[0].sectionDimensions[1].valueMm);
    require(std::abs(movedSeparation - separation) < 1e-4,
            "World rotation changed the measured interval");
    require(moved->views[1].visibleOpeningCount == 2, "World rotation hid oval centers");
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
    many.profiles.clear(); // Exercise pagination of legacy local sections independently.
    for (std::size_t i = 1; i < 31; ++i) {
        many.bends.push_back(acute->bends[0]);
        auto detail = acute->details[0];
        detail.bendIndex = i;
        detail.designation = "А" + std::to_string(i + 1);
        for (auto &[point, index] : detail.view.bendNotes)
            index = i;
        detail.view.dimensionedBends = {i};
        many.details.push_back(std::move(detail));
    }
    auto profileTitle = title;
    profileTitle.layout = ui::DrawingLayout::Profiles;
    require(ui::DrawingSheet::pageCount(many, profileTitle) == 8,
            "Pagination lost enlarged profiles");
    require(ui::DrawingSheet::writePdf(folder + "/many.pdf", many, profileTitle).isEmpty(),
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
// Independent analytic Z: outside dimensions 36 / 40 / 36, two opposite turns.
TopoDS_Shape reverseProfile(double axialLength = 80, double height = 40, double leftEnd = -30,
                            double rightEnd = 40) {
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    const auto arc = [&](double x, double y, double r, double from, double to) {
        wire.Add(
            BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(fixtures::polar(r, from, x, y),
                                                       fixtures::polar(r, (from + to) / 2, x, y),
                                                       fixtures::polar(r, to, x, y))
                                        .Value())
                .Edge());
    };
    constexpr double pi = std::numbers::pi;
    line({leftEnd, 0, 0}, {0, 0, 0});
    arc(0, 6, 6, -pi / 2, 0);
    line({6, 6, 0}, {6, height - 6, 0});
    arc(10, height - 6, 4, pi, pi / 2);
    line({10, height - 2, 0}, {rightEnd, height - 2, 0});
    line({rightEnd, height - 2, 0}, {rightEnd, height, 0});
    line({rightEnd, height, 0}, {10, height, 0});
    arc(10, height - 6, 6, pi / 2, pi);
    line({4, height - 6, 0}, {4, 6, 0});
    arc(0, 6, 4, 0, -pi / 2);
    line({0, 2, 0}, {leftEnd, 2, 0});
    line({leftEnd, 2, 0}, {leftEnd, 0, 0});
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, axialLength))
        .Shape();
}
TopoDS_Shape returnLipProfile() {
    BRepBuilderAPI_MakeWire wire;
    const auto line = [&](gp_Pnt a, gp_Pnt b) { wire.Add(BRepBuilderAPI_MakeEdge(a, b).Edge()); };
    const auto arc = [&](double x, double y, double r, double from, double to) {
        wire.Add(
            BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(fixtures::polar(r, from, x, y),
                                                       fixtures::polar(r, (from + to) / 2, x, y),
                                                       fixtures::polar(r, to, x, y))
                                        .Value())
                .Edge());
    };
    constexpr double pi = std::numbers::pi;
    line({-6, 40, 0}, {-6, 6, 0});
    arc(0, 6, 6, pi, 1.5 * pi);
    line({0, 0, 0}, {80, 0, 0});
    arc(80, 6, 6, 1.5 * pi, 2 * pi);
    line({86, 6, 0}, {86, 40, 0});
    arc(90, 40, 4, pi, pi / 2);
    line({90, 44, 0}, {120, 44, 0});
    line({120, 44, 0}, {120, 46, 0});
    line({120, 46, 0}, {90, 46, 0});
    arc(90, 40, 6, pi / 2, pi);
    line({84, 40, 0}, {84, 6, 0});
    arc(80, 6, 4, 0, -pi / 2);
    line({80, 2, 0}, {0, 2, 0});
    arc(0, 6, 4, -pi / 2, -pi);
    line({-4, 6, 0}, {-4, 40, 0});
    line({-4, 40, 0}, {-6, 40, 0});
    return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()), gp_Vec(0, 0, 80)).Shape();
}
// Two perpendicular bends share a flat base, with corner relief separating the zones.
TopoDS_Shape crossAxisPart() {
    auto first = fixtures::bentStrip(4, 2, std::numbers::pi / 2, 70, true, 80);
    gp_Trsf cyclic;
    cyclic.SetValues(0, 0, 1, 0, 1, 0, 0, 80, 0, 1, 0, 0);
    first = BRepBuilderAPI_Transform(first, cyclic, true).Shape();
    gp_Trsf turn;
    turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), -std::numbers::pi / 2);
    turn.SetTranslationPart(gp_Vec(0, 80, 0));
    const auto second = BRepBuilderAPI_Transform(first, turn, true).Shape();
    const auto joined = BRepAlgoAPI_Fuse(first, second).Shape();
    ShapeUpgrade_UnifySameDomain unify(joined, true, true, false);
    unify.Build();
    return unify.Shape();
}
void operatorProfiles(const QString &folder) {
    const auto check = [&](const application::BendingDrawing &d, std::vector<double> sizes,
                           bool reverse) {
        require(d.profiles.size() == 1, "An open parallel chain has no operator profile");
        const auto &p = d.profiles[0];
        require(p.segments.size() == sizes.size() && p.bends.size() + 1 == sizes.size() &&
                    p.view.profileStart && p.turnSigns.size() == p.bends.size(),
                "Profile order, endpoints or signed turns were lost");
        std::vector<double> actual;
        for (const auto &s : p.segments) {
            actual.push_back(s.dimension.valueMm);
            require(std::abs(std::hypot(s.dimension.end.x - s.dimension.start.x,
                                        s.dimension.end.y - s.dimension.start.y) -
                             s.dimension.valueMm) < 1e-4,
                    "A profile length is a bounding-box projection");
        }
        std::sort(actual.begin(), actual.end());
        std::sort(sizes.begin(), sizes.end());
        for (std::size_t i = 0; i < sizes.size(); ++i)
            require(std::abs(actual[i] - sizes[i]) < 1e-4, "Wrong outside profile dimension");
        if (p.turnSigns.size() == 2)
            require((p.turnSigns[0] != p.turnSigns[1]) == reverse,
                    "Reverse bend was confused with a U profile");
        const auto coverage = application::drawingMeasurementCoverage(d);
        require(coverage.dimensioned == p.bends.size() && coverage.total == p.bends.size() &&
                    coverage.missing.empty(),
                "A fully measured chain has incomplete coverage");
        ui::DrawingTitle title;
        title.layout = ui::DrawingLayout::Profiles;
        require(!ui::DrawingSheet::preview(d, title).isNull(),
                "Operator profile cannot be laid out");
    };
    check(*build(load(fixtures::bentStrip(8, 6), folder + "/operator-L.step")), {44, 44}, false);
    check(*build(load(fixtures::channel(), folder + "/operator-U.step")), {40, 92, 40}, false);
    const auto lip = build(load(returnLipProfile(), folder + "/operator-lip.step"));
    check(*lip, {40, 92, 46, 36}, true);
    require(
        std::count(lip->profiles[0].turnSigns.begin(), lip->profiles[0].turnSigns.end(), 1) != 0 &&
            std::count(lip->profiles[0].turnSigns.begin(), lip->profiles[0].turnSigns.end(), -1) !=
                0,
        "A three-bend return lip lost its change of bend direction");
    const auto source = reverseProfile();
    const auto document = load(source, folder + "/operator-Z.step");
    const std::string originalBytes(document->sourceStep());
    const auto drawing = build(document);
    check(*drawing, {36, 40, 36}, true);
    require(document->sourceStep() == originalBytes, "Drawing changed captured STEP bytes");
    require(ui::DrawingSheet::writePdf(folder + "/operator-Z.pdf", *drawing, {}).isEmpty(),
            "Reverse-bend PDF failed");
    const auto output = qEnvironmentVariable("BENDING_DRAWING_OPERATOR_OUTPUT");
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output, *drawing, {}).isEmpty(), "Example PDF failed");
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .734);
    transform.SetTranslationPart(gp_Vec(117, -53, 82));
    check(*build(load(BRepBuilderAPI_Transform(source, transform, true).Shape(),
                      folder + "/operator-Z-moved.step")),
          {36, 40, 36}, true);
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)));
    check(*build(load(BRepBuilderAPI_Transform(source, mirror, true).Shape(),
                      folder + "/operator-Z-mirror.step")),
          {36, 40, 36}, true);
    // A closed drilled hole intersects the profile station. It must remain on the views
    // without splitting the wall dimensions or altering the source body.
    const auto drilled =
        BRepAlgoAPI_Cut(
            source, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-15, -1, 40), gp_Dir(0, 1, 0)), 3, 4))
            .Shape();
    const auto holed = build(load(drilled, folder + "/operator-hole.step"));
    check(*holed, {36, 40, 36}, true);
    require(std::any_of(holed->views.begin(), holed->views.end(),
                        [](const auto &v) { return !v.circleCenters.empty(); }),
            "The schematic removed a source-view hole");
    const auto crossed = build(load(crossAxisPart(), folder + "/operator-crossed.step"));
    require(crossed->bends.size() == 2 && crossed->profiles.size() == 2,
            "Perpendicular bends were flattened into one guessed profile");
    const auto coverage = application::drawingMeasurementCoverage(*crossed);
    require(coverage.total == 2 && coverage.dimensioned == 2,
            "Separate profile groups lost coverage");
    require(ui::DrawingSheet::writePdf(folder + "/operator-crossed.pdf", *crossed, {}).isEmpty(),
            "Two profile groups cannot share a sheet");
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output + ".crossed.pdf", *crossed, {}).isEmpty(),
                "Crossed example PDF failed");
    const auto notched =
        BRepAlgoAPI_Cut(source, BRepPrimAPI_MakeBox(gp_Pnt(3, 18, 35), 4, 6, 46)).Shape();
    const auto interrupted = build(load(notched, folder + "/operator-notch.step"));
    require(interrupted->profiles.empty(), "A profile bridged an exterior notch between bends");
    const auto unknownEnd =
        BRepAlgoAPI_Cut(
            fixtures::bentStrip(),
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(5, -35, -1), gp_Dir(0, 0, 1)), 10, 82))
            .Shape();
    const auto unsupported = build(load(unknownEnd, folder + "/operator-cap.step"));
    require(unsupported->profiles.empty(),
            "An unknown cylindrical termination was labelled as a flat flange end");
    const auto longProfile =
        build(load(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 80, true, 2000),
                   folder + "/operator-long.step"));
    require(
        longProfile->profiles.size() == 1 && ui::DrawingSheet::pageCount(*longProfile) >= 2 &&
            ui::DrawingSheet::writePdf(folder + "/operator-long.pdf", *longProfile, {}).isEmpty(),
        "A small bend in a long profile lost its enlarged local view");
    auto partial = *drawing;
    partial.profiles.clear();
    partial.details.clear();
    partial.sections.clear();
    for (auto &v : partial.views)
        v.dimensionedBends.clear();
    partial.views[0].dimensionedBends = {0, 0};
    partial.bends.push_back(partial.bends[0]);
    partial.bends.back().diagnostic = true;
    const auto missing = application::drawingMeasurementCoverage(partial);
    require(missing.total == 2 && missing.dimensioned == 1 &&
                missing.missing == std::vector<std::size_t>{1},
            "Incomplete coverage counts notes or candidates");
}
// Independent three-station Z wall: 98 / 2 / 100 / 2 / 98, with the middle reversed.
TopoDS_Shape segmentedWall(bool holes = true) {
    const auto moved = [](const TopoDS_Shape &s, double z) {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(0, 0, z));
        return BRepBuilderAPI_Transform(s, t, true).Shape();
    };
    gp_Trsf mirror;
    mirror.SetMirror(gp_Ax2(gp_Pnt(5, 0, 0), gp_Dir(1, 0, 0)));
    auto shape =
        BRepAlgoAPI_Fuse(
            reverseProfile(98),
            moved(BRepBuilderAPI_Transform(reverseProfile(100), mirror, true).Shape(), 100))
            .Shape();
    shape = BRepAlgoAPI_Fuse(shape, moved(reverseProfile(98), 202)).Shape();
    shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(4, 6, 0), 2, 28, 300)).Shape();
    if (holes) {
        for (double z : {25., 75., 125., 175., 225., 275.})
            for (double y : {12., 28.})
                shape =
                    BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(
                                               gp_Ax2(gp_Pnt(3, y, z), gp_Dir(1, 0, 0)), 1.5, 4))
                        .Shape();
        for (const auto p : {gp_Pnt(-15, -1, 49), gp_Pnt(25, -1, 150), gp_Pnt(-15, -1, 251)})
            shape =
                BRepAlgoAPI_Cut(shape, BRepPrimAPI_MakeCylinder(gp_Ax2(p, gp_Dir(0, 1, 0)), 3, 4))
                    .Shape();
    }
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, false);
    unify.Build();
    return unify.Shape();
}
void classicalDrawing(const QString &folder) {
    const auto source = segmentedWall();
    const auto document = load(source, folder + "/classical-wall.step");
    const std::string bytes(document->sourceStep());
    const auto d = build(document);
    require(d->classicalViews && d->bends.size() == 6 && d->sections.size() == 2 &&
                d->auxiliaryViews.empty(),
            "Segmented wall lost classical sections or base views");
    std::vector<std::size_t> places;
    for (const auto &s : d->sections) {
        places.push_back(s.locations.size());
        require(s.view.sectionLoops.size() == 1 && s.view.profileStart == std::nullopt,
                "An actual section was replaced by the operator schematic");
        for (const auto &at : s.locations)
            require(at.sourceView == 0 && at.marker.designation == s.designation &&
                        at.bends.size() == 2,
                    "A section lost its source plane or bend binding");
    }
    std::sort(places.begin(), places.end());
    require(places == std::vector<std::size_t>{1, 2}, "Equal end sections were not grouped");
    const auto &views = *d->classicalViews;
    require(std::abs(views[0].minimum.x - views[1].minimum.x) < 1e-5 &&
                std::abs(views[0].maximum.x - views[1].maximum.x) < 1e-5 &&
                std::abs(views[0].minimum.y - views[2].minimum.y) < 1e-5 &&
                std::abs(views[0].maximum.y - views[2].maximum.y) < 1e-5 &&
                !views[1].reverseProjection && !views[2].reverseProjection,
            "Classical views lost their shared projection coordinates");
    require(views[0].sectionMarkers.size() == 3 && views[0].axialDimensions.size() == 5,
            "Section traces or axial gaps are missing");
    const std::array expected{98., 2., 100., 2., 98.};
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(std::abs(views[0].axialDimensions[i].valueMm - expected[i]) < 1e-5,
                "Axial dimensions do not describe the occupied intervals and gaps");
    require(std::count_if(views[0].holeDimensions.begin(), views[0].holeDimensions.end(),
                          [](const auto &x) {
                              return x.pitchIntervals == 5 && std::abs(x.pitchMm - 50) < 1e-5 &&
                                     std::abs(x.valueMm - 250) < 1e-5;
                          }) == 1 &&
                views[0].holeDiameters.size() == 12,
            "A verified hole pitch or diameter lost its measured source geometry");
    const auto coverage = ui::DrawingSheet::coverage(*d);
    require(coverage.total == 6 && coverage.dimensioned == 6 &&
                ui::DrawingSheet::pageCount(*d) == 1 &&
                ui::DrawingSheet::writePdf(folder + "/classical-wall.pdf", *d, {}).isEmpty(),
            "A complete six-zone classical sheet does not fit A3");
    const auto output = qEnvironmentVariable("BENDING_DRAWING_CLASSICAL_OUTPUT");
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output, *d, {}).isEmpty(),
                "Classical sample PDF failed");
    require(document->sourceStep() == bytes && document->metadata().bendAnalysis.bends.size() == 6,
            "Classical drawing changed the source or simulation analysis");
    gp_Trsf move;
    move.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .734);
    move.SetTranslationPart(gp_Vec(117, -53, 82));
    const auto rotated = build(load(BRepBuilderAPI_Transform(source, move, true).Shape(),
                                    folder + "/classical-rotated.step"));
    require(rotated->sections.size() == 2 &&
                ui::DrawingSheet::coverage(*rotated).dimensioned == 6 &&
                !ui::DrawingSheet::preview(*rotated, {}).isNull(),
            "A world rotation changed section grouping or completeness");
    const auto different =
        BRepAlgoAPI_Cut(segmentedWall(false), BRepPrimAPI_MakeBox(gp_Pnt(-22, -1, 1), 14, 4, 96))
            .Shape();
    const auto distinct = build(load(different, folder + "/classical-different-void.step"));
    require(distinct->sections.size() == 3,
            "Equal flange numbers hid a different material contour in a grouped section");
    const auto base = build(load(reverseProfile(), folder + "/classical-solid.step"));
    const auto slot = build(
        load(BRepAlgoAPI_Cut(reverseProfile(), BRepPrimAPI_MakeBox(gp_Pnt(-22, -1, 1), 14, 4, 78))
                 .Shape(),
             folder + "/classical-slot.step"));
    const auto area = [](const application::DrawingView &view) {
        double sum = 0;
        for (const auto &loop : view.sectionLoops) {
            double signedArea = 0;
            for (std::size_t i = 1; i < loop.size(); ++i)
                signedArea += loop[i - 1].x * loop[i].y - loop[i].x * loop[i - 1].y;
            sum += std::abs(signedArea) / 2;
        }
        return sum;
    };
    require(base->sections.size() == 1 && slot->sections.size() == 1 &&
                slot->sections[0].view.sectionLoops.size() == 2 &&
                std::abs(area(base->sections[0].view) - area(slot->sections[0].view) - 28) < .1,
            "A true section filled an internal cutout or lost the material boundary");
    auto compact = *base;
    compact.auxiliaryViews.push_back({(*base->classicalViews)[2], "Х", 2});
    const auto compactCoverage = ui::DrawingSheet::coverage(compact);
    require(
        ui::DrawingSheet::pageCount(compact) == 1 &&
            compactCoverage.dimensioned == ui::DrawingSheet::coverage(*base).dimensioned &&
            ui::DrawingSheet::writePdf(folder + "/classical-compact.pdf", compact, {}).isEmpty(),
        "A readable auxiliary view was unnecessarily moved to another sheet");
    requireCompactOrientation(ui::DrawingSheet::preview(compact, {}, 0, 1680));
    if (!output.isEmpty())
        require(ui::DrawingSheet::writePdf(output + ".compact.pdf", compact, {}).isEmpty(),
                "Compact layout sample export failed");
    auto crowded = compact;
    for (int i = 0; i < 12; ++i)
        crowded.auxiliaryViews.push_back(compact.auxiliaryViews.back());
    require(ui::DrawingSheet::pageCount(crowded) > 1 &&
                !ui::DrawingSheet::preview(crowded, {}, ui::DrawingSheet::pageCount(crowded) - 1)
                     .isNull(),
            "An overcrowded sheet lost its readable continuation views");
    requireCompactOrientation(
        ui::DrawingSheet::preview(crowded, {}, ui::DrawingSheet::pageCount(crowded) - 1, 1680));
    const auto occludedDocument = load(
        BRepAlgoAPI_Cut(fixtures::channel(),
                        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-7, 20, 40), gp_Dir(1, 0, 0)), 3, 4))
            .Shape(),
        folder + "/classical-opposite-view.step");
    application::DrawingRequest oppositeRequest;
    oppositeRequest.reverseView = true;
    const auto firstSide = build(occludedDocument);
    const auto otherSide = build(occludedDocument, oppositeRequest);
    const auto occluded = firstSide->auxiliaryViews.empty() ? otherSide : firstSide;
    require(!occluded->auxiliaryViews.empty() && occluded->classicalViews &&
                !(*occluded->classicalViews)[0].viewArrows.empty() &&
                std::any_of(occluded->auxiliaryViews.begin(), occluded->auxiliaryViews.end(),
                            [](const auto &v) {
                                return !v.view.holeDiameters.empty() && !v.designation.empty();
                            }) &&
                ui::DrawingSheet::writePdf(folder + "/classical-opposite-view.pdf", *occluded, {})
                    .isEmpty(),
            "A more visible reverse projection was lost or shown without a direction binding");
    const auto cornerBox = BRepPrimAPI_MakeBox(gp_Pnt(-30, -1, 0), 3, 4, 3).Shape();
    const auto cornerCylinder =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-27, -1, 3), gp_Dir(0, 1, 0)), 3, 4).Shape();
    const auto rounding =
        BRepAlgoAPI_Cut(reverseProfile(), BRepAlgoAPI_Cut(cornerBox, cornerCylinder).Shape())
            .Shape();
    const auto roundedDocument = load(rounding, folder + "/classical-rounded-contour.step");
    const auto rounded = build(roundedDocument);
    require(roundedDocument->metadata().bendAnalysis.unrecognizedCylindricalFaces > 0 &&
                rounded->unrecognizedCylindricalFaces == 0 && rounded->bends.size() == 2,
            "A contour rounding was reported as an unexplained bend or changed simulation data");
    auto smallPlate = BRepPrimAPI_MakeBox(60, 40, 4).Shape();
    for (const auto p :
         {gp_Pnt(10, 15, -1), gp_Pnt(24, 32, -1), gp_Pnt(36, 32, -1), gp_Pnt(50, 15, -1)})
        smallPlate = BRepAlgoAPI_Cut(smallPlate,
                                     BRepPrimAPI_MakeCylinder(gp_Ax2(p, gp_Dir(0, 0, 1)), 1.5, 6))
                         .Shape();
    const auto small = build(load(smallPlate, folder + "/classical-small-plate.step"));
    require(ui::DrawingSheet::writePdf(folder + "/classical-small-plate.pdf", *small, {}).isEmpty(),
            "A small plate with irregular hole coordinates cannot be placed");
    const auto tall = build(
        load(reverseProfile(3000, 1200.37, -24.17, 33.14), folder + "/classical-shallow-top.step"));
    require(ui::DrawingSheet::writePdf(folder + "/classical-shallow-top.pdf", *tall, {}).isEmpty(),
            "A shallow projection has no room for its outside dimension number");
}
void flatDrawing(const QString &folder) {
    const auto doc = load(fixtures::channel(), folder + "/unfold-channel.step");
    application::DrawingRequest request;
    request.unfoldSettings = core::UnfoldSettings{core::SheetMaterial::MildSteel, {}, 1};
    const auto result = build(doc, request);
    require(result->flatPattern && !result->flatPatternError, "Flat drawing snapshot missing");
    const auto &flat = *result->flatPattern;
    const auto preview = ui::DrawingSheet::preview(*result, {}, 0, 1680);
    requireCompactOrientation(preview);
    require(flat.info.bends.size() == 2 && !flat.view.paths.empty() &&
                flat.view.dimensions.size() == 2 && flat.view.sectionDimensions.size() == 2,
            "Blank contour, overall sizes or bend coordinates missing");
    const auto cachedResult =
        cad::FlatPatternBuilder{}.build(doc, {*request.unfoldSettings, {}}, {});
    const auto cached = std::get<application::FlatPatternPtr>(cachedResult);
    require(cad::OcctFlatPatternAccess::belongsTo(*cached, *doc), "Flat provenance lost");
    const auto uncachedRequest = request;
    request.preparedFlatPattern = cached;
    require(request == uncachedRequest, "A cache handle became a drawing geometry input");
    require(build(doc, request)->flatPattern->info.bounds.maximum.y == flat.info.bounds.maximum.y,
            "Prepared flat differs from drawing");
    const auto other = load(fixtures::bentStrip(), folder + "/different-unfold.step");
    require(!cad::OcctFlatPatternAccess::belongsTo(*cached, *other),
            "Foreign flat provenance accepted");
    const auto different = build(other, request);
    require(different->flatPattern && different->flatPattern->info.bends.size() == 1,
            "Foreign cached blank was used on another document");
    request.unfoldSettings->manualK = .5;
    const auto changed = build(doc, request);
    require(changed->flatPattern &&
                changed->flatPattern->info.bounds.maximum.y > flat.info.bounds.maximum.y,
            "Stale cached K was used for PDF");
    request.manualThicknessMm = 3;
    const auto conflict = build(doc, request);
    require(!conflict->flatPattern && conflict->flatPatternError &&
                conflict->flatPatternError->code ==
                    application::FlatPatternErrorCode::ThicknessConflict,
            "Cached blank ignored a conflicting manual thickness");
    request.manualThicknessMm.reset();
    request.unfoldSettings = core::UnfoldSettings{};
    const auto missing = build(doc, request);
    require(!missing->flatPattern && missing->flatPatternError &&
                missing->flatPatternError->code ==
                    application::FlatPatternErrorCode::MissingMaterial &&
                !missing->views[0].paths.empty(),
            "Missing material hid folded views or printed a blank");
    request.unfoldSettings = core::UnfoldSettings{core::SheetMaterial::MildSteel, {}, 1};
    const auto zoneOpening =
        build(load(fixtures::bendWithZoneOpening(), folder + "/unfold-zone-hole.step"), request);
    require(zoneOpening->flatPattern && zoneOpening->flatPattern->info.cutoutsIgnored &&
                zoneOpening->flatPattern->view.paths.size() >= 2 &&
                !zoneOpening->views[0].paths.empty(),
            "Bend-zone hole blocked the blank or disappeared from its drawing");
    const auto profiled =
        build(load(fixtures::profiledBend(), folder + "/unfold-profiled.step"), request);
    require(
        profiled->flatPattern && profiled->flatPattern->info.cutoutsIgnored &&
            profiled->flatPattern->view.paths.size() >= 4 &&
            ui::DrawingSheet::writePdf(folder + "/unfold-profiled.pdf", *profiled, {}).isEmpty(),
        "Countersink rims or hole are missing from the flat drawing/PDF");
    const auto unsupported =
        build(load(fixtures::cornerTrimmedBend(), folder + "/unfold-trimmed.step"), request);
    require(!unsupported->flatPattern && unsupported->flatPatternError &&
                !unsupported->views[0].paths.empty(),
            "Unsupported blank replaced the valid drawing");
    const auto branched = build(load(segmentedWall(), folder + "/unfold-branches.step"), request);
    require(
        branched->flatPattern && branched->flatPattern->info.bends.size() == 6 &&
            branched->flatPattern->view.sectionDimensions.size() == 2 &&
            branched->flatPattern->view.axialDimensions.size() == 5 &&
            ui::DrawingSheet::pageCount(*branched) == 1 &&
            ui::DrawingSheet::writePdf(folder + "/unfold-branches.pdf", *branched, {}).isEmpty(),
        "Branched blank lost grouped coordinates, axial gaps or one-sheet layout");
    const auto tray = build(load(fixtures::tray(), folder + "/unfold-tray.step"), request);
    require(tray->flatPattern && tray->flatPattern->info.bends.size() == 4 &&
                tray->flatPattern->view.paths.size() >= 5,
            "Tray blank lost bend directions or hole contours");
    const auto &trayFlat = *tray->flatPattern;
    for (const auto &bend : trayFlat.info.bends) {
        const auto line = bend.centreLine();
        const bool horizontal = std::abs(line[0].y - line[1].y) < 1.e-5;
        const auto &coordinates =
            horizontal ? trayFlat.view.sectionDimensions : trayFlat.view.axialDimensions;
        require(std::any_of(coordinates.begin(), coordinates.end(),
                            [&](const auto &dim) {
                                return horizontal ? std::abs(dim.valueMm - line[0].y) < 1.e-5
                                                  : (std::abs(dim.start.x - line[0].x) < 1.e-5 ||
                                                     std::abs(dim.end.x - line[0].x) < 1.e-5);
                            }),
                "A tray bend has no dimension in its own direction");
    }
    require(ui::DrawingSheet::writePdf(folder + "/unfold-tray.pdf", *tray, {}).isEmpty(),
            "Two-direction blank dimensions cannot be exported");
    const auto oblique =
        build(load(fixtures::obliqueTray(), folder + "/unfold-oblique.step"), request);
    require(oblique->flatPattern && !oblique->flatPattern->view.sectionDimensions.empty() &&
                !oblique->flatPattern->view.axialDimensions.empty() &&
                ui::DrawingSheet::writePdf(folder + "/unfold-oblique.pdf", *oblique, {}).isEmpty(),
            "Oblique bend axes lost coordinate dimensions or PDF layout");
    ui::DrawingTitle profileTitle;
    profileTitle.layout = ui::DrawingLayout::Profiles;
    auto withoutFlat = *result;
    withoutFlat.flatPattern.reset();
    require(ui::DrawingSheet::pageCount(*result, profileTitle) ==
                    ui::DrawingSheet::pageCount(withoutFlat, profileTitle) + 1 &&
                ui::DrawingSheet::writePdf(folder + "/unfold-separate.pdf", *result, profileTitle)
                    .isEmpty(),
            "Fallback flat page was lost or could not be exported");
    requireCompactOrientation(ui::DrawingSheet::preview(
        *result, profileTitle, ui::DrawingSheet::pageCount(*result, profileTitle) - 1, 1680));
    auto invalid = *result;
    invalid.flatPattern->info.bends.clear();
    const auto failurePath = folder + "/failed-flat.pdf";
    QFile previous(failurePath);
    require(previous.open(QIODevice::WriteOnly), "Cannot prepare previous PDF");
    previous.write("previous-pdf");
    previous.close();
    require(!ui::DrawingSheet::writePdf(failurePath, invalid, {}).isEmpty() &&
                previous.open(QIODevice::ReadOnly) && previous.readAll() == "previous-pdf",
            "A failed additional flat page overwrote the previous PDF");
    const auto output = qEnvironmentVariable("BENDING_FLAT_DRAWING_OUTPUT");
    require(ui::DrawingSheet::writePdf(output.isEmpty() ? folder + "/unfolded.pdf" : output,
                                       *result, {})
                .isEmpty(),
            "Combined blank and folded PDF failed");
    require(ui::DrawingSheet::pageCount(*result) == 1,
            "Simple blank was unnecessarily split from the folded shape");
}
void auditSample(const QString &path, const QString &reportPath, const QString &pdfPath) {
    QElapsedTimer timer;
    timer.start();
    QJsonObject report{{"file", path}};
    const auto loaded = cad::StepImporter{}.load(native(path), {}, {});
    const auto doc = application::resultDocument(loaded);
    report.insert("importOk", bool(doc));
    report.insert("importMs", static_cast<double>(timer.elapsed()));
    if (!doc) {
        if (const auto *error = std::get_if<application::ImportError>(&loaded))
            report.insert("importError", QString::fromStdString(error->detail));
    } else {
        const auto &m = doc->metadata();
        report.insert("faces", static_cast<int>(m.faceCount));
        report.insert("thicknessStatus", static_cast<int>(m.thickness.status));
        if (m.thickness.thicknessMm)
            report.insert("thickness", *m.thickness.thicknessMm);
        report.insert("recognitionStatus", static_cast<int>(m.bendAnalysis.status));
        report.insert("strictBends", static_cast<int>(m.bendAnalysis.bends.size()));
        report.insert("zoneOpenings",
                      static_cast<int>(m.bendAnalysis.interiorContourDiagnostics.size()));
        QJsonArray warnings, bends;
        for (const auto &warning : m.diagnostics)
            warnings.append(QString::fromStdString(warning.message));
        report.insert("importDiagnostics", warnings);
        for (const auto &b : m.bendAnalysis.bends)
            bends.append(
                QJsonObject{{"length", b.lengthMm},
                            {"t", b.thicknessMm},
                            {"radius", b.innerRadiusMm},
                            {"internalAngle", 180 - b.angleRadians * 180 / std::numbers::pi},
                            {"trimmedEnds", b.trimmedEnds}});
        report.insert("bends", bends);
        application::DrawingRequest request;
        request.unfoldSettings = core::UnfoldSettings{core::SheetMaterial::MildSteel, {}, 1};
        const auto result = cad::BendingDrawingBuilder{}.build(doc, request, {});
        if (const auto *error = std::get_if<application::DrawingError>(&result)) {
            report.insert("drawingOk", false);
            report.insert("drawingError", QString::fromStdString(error->detail));
        } else {
            const auto &d = *std::get<application::BendingDrawingPtr>(result);
            report.insert("drawingOk", true);
            const auto coverage = ui::DrawingSheet::coverage(d);
            report.insert("drawingBends", static_cast<int>(coverage.total));
            report.insert("dimensionedBends", static_cast<int>(coverage.dimensioned));
            report.insert("drawingOnlyRecognition", d.drawingOnlyRecognition);
            report.insert("unrecognizedCylinders",
                          static_cast<int>(d.unrecognizedCylindricalFaces));
            report.insert("sections", static_cast<int>(d.sections.size()));
            report.insert("flatOk", bool(d.flatPattern));
            if (d.flatPattern) {
                report.insert("flatWidth", d.flatPattern->info.bounds.maximum.x);
                report.insert("flatLength", d.flatPattern->info.bounds.maximum.y);
            }
            if (d.flatPatternError)
                report.insert("flatError", QString::fromStdString(d.flatPatternError->detail));
            QJsonArray missing;
            for (auto index : coverage.missing)
                missing.append(static_cast<int>(index + 1));
            report.insert("missingBends", missing);
            ui::DrawingTitle title;
            title.name = QFileInfo(path).completeBaseName();
            const auto exportError = ui::DrawingSheet::writePdf(pdfPath, d, title);
            report.insert("pdfOk", exportError.isEmpty());
            if (!exportError.isEmpty())
                report.insert("pdfError", exportError);
            report.insert("pages", ui::DrawingSheet::pageCount(d));
            QJsonArray views;
            const auto &projections =
                d.classicalViews
                    ? *d.classicalViews
                    : std::array<application::DrawingView, 3>{d.views[0], d.views[1], d.views[2]};
            for (const auto &v : projections) {
                QJsonArray sizes, holes, axial;
                for (const auto &dimension : v.dimensions)
                    sizes.append(dimension.valueMm);
                for (const auto &dimension : v.holeDimensions)
                    holes.append(dimension.valueMm);
                for (const auto &dimension : v.axialDimensions)
                    axial.append(dimension.valueMm);
                views.append(QJsonObject{{"width", v.maximum.x - v.minimum.x},
                                         {"height", v.maximum.y - v.minimum.y},
                                         {"dimensions", sizes},
                                         {"holeDimensions", holes},
                                         {"axialDimensions", axial}});
            }
            report.insert("views", views);
            const auto &iso = d.views[3];
            QImage shape(720, 520, QImage::Format_RGB32);
            shape.fill(Qt::white);
            QPainter painter(&shape);
            painter.setRenderHint(QPainter::Antialiasing);
            const double scale = std::min(660 / (iso.maximum.x - iso.minimum.x),
                                          460 / (iso.maximum.y - iso.minimum.y));
            const auto map = [&](application::DrawingPoint point) {
                return QPointF(360 + (point.x - (iso.minimum.x + iso.maximum.x) / 2) * scale,
                               260 - (point.y - (iso.minimum.y + iso.maximum.y) / 2) * scale);
            };
            for (bool hidden : {true, false}) {
                painter.setPen(QPen(hidden ? Qt::lightGray : Qt::black, hidden ? .7 : 1.2,
                                    hidden ? Qt::DashLine : Qt::SolidLine));
                for (const auto &path : iso.paths) {
                    if (path.hidden != hidden || path.points.empty())
                        continue;
                    QPainterPath outline;
                    outline.moveTo(map(path.points.front()));
                    for (std::size_t i = 1; i < path.points.size(); ++i)
                        outline.lineTo(map(path.points[i]));
                    painter.drawPath(outline);
                }
            }
            painter.end();
            require(shape.save(reportPath + ".png"), "Cannot save the audit projection");
        }
        if (m.bendAnalysis.bends.size() == 1 && m.bendAnalysis.interiorContourDiagnostics.empty()) {
            const auto &b = m.bendAnalysis.bends.front();
            const auto result = cad::SingleBendPreviewBuilder{}.build(
                doc, 0, {b.angleRadians, b.innerRadiusMm}, false, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&result)) {
                report.insert("kinematicOk", false);
                report.insert("kinematicError", QString::fromStdString(error->detail));
            } else {
                report.insert("kinematicOk", true);
                report.insert(
                    "kinematicFrames",
                    static_cast<int>(std::get<application::BendPreviewPtr>(result)->frameCount()));
            }
        }
    }
    report.insert("totalMs", static_cast<double>(timer.elapsed()));
    QFile output(reportPath);
    require(output.open(QIODevice::WriteOnly), "Cannot write sample audit");
    require(output.write(QJsonDocument(report).toJson()) > 0, "Cannot save sample audit");
}
void workflow(const QString &folder) {
    auto importer = std::make_shared<cad::StepImporter>();
    ui::MainWindow window({"test", "test"}, importer);
    window.setAutomaticPartInputPrompt(false);
    auto builder = std::make_shared<cad::BendingDrawingBuilder>();
    window.setDrawingBuilder(builder);
    window.show();
    auto &c = window.importController();
    const auto sample = qEnvironmentVariable("BENDING_DRAWING_UI_SAMPLE");
    window.openStep(sample.isEmpty() ? QString::fromUtf8(BENDING_EXAMPLE_DIR) + "/bend-90-6mm.step"
                                     : sample);
    wait([&] { return !c.hasWork(); });
    require(c.session().document() != nullptr, "Workflow import failed");
    require(c.setUnfoldSettings({core::SheetMaterial::MildSteel, {}, 1}),
            "Cannot select drawing material");
    const auto revision = c.session().editRevision();
    auto *action = window.findChild<QAction *>("bendingDrawingAction");
    require(action && action->isEnabled(), "Drawing action missing");
    auto *drawingEntry = window.findChild<QToolButton *>("drawingStageButton");
    require(drawingEntry && drawingEntry->isVisible() && drawingEntry->isEnabled() &&
                !drawingEntry->isCheckable() && !c.bendPreview() && !c.bendRoute(),
            "Drawing entry is missing or requires a bending result");
    bool inspected = false, pdf = false;
    int exportedPages = 0;
    drawingEntry->click();
    auto *dialog = window.findChild<QDialog *>("bendingDrawingDialog");
    require(dialog && dialog->isVisible() && !dialog->isModal() &&
                QApplication::activeModalWidget() == nullptr && window.isEnabled() &&
                (dialog->windowFlags() & Qt::WindowMinMaxButtonsHint),
            "Drawing window is modal or lacks normal window controls");
    auto *exportButton = dialog->findChild<QPushButton *>("exportDrawingPdf");
    require(exportButton, "Missing PDF action");
    wait([&] { return !c.hasWork() && exportButton->isEnabled(); });
    dialog->showMinimized();
    action->trigger();
    QApplication::processEvents();
    require(!dialog->isMinimized() &&
                window.findChildren<QDialog *>("bendingDrawingDialog").size() == 1,
            "Reopening did not restore the existing drawing window");
    dialog->showMaximized();
    QApplication::processEvents();
    require(dialog->isMaximized(), "Drawing window cannot be maximized");
    dialog->showNormal();
    dialog->resize(1200, 720);
    QApplication::processEvents();
    auto *qty = dialog->findChild<QSpinBox *>("drawingQuantity");
    qty->setValue(37);
    const auto oldDrawing = c.drawing();
    require(oldDrawing && oldDrawing->flatPattern,
            "Dialog did not automatically prepare the blank");
    auto *layout = dialog->findChild<QComboBox *>("drawingLayout");
    require(layout && layout->currentIndex() == 0, "Classical layout is not the default");
    auto *page = dialog->findChild<QSpinBox *>("drawingPage");
    auto *navigation = dialog->findChild<QWidget *>("drawingPageNavigation");
    auto *controls = dialog->findChild<QWidget *>("drawingPreviewControls");
    auto *previous = dialog->findChild<QToolButton *>("previousDrawingPageButton");
    auto *next = dialog->findChild<QToolButton *>("nextDrawingPageButton");
    auto *scroll = dialog->findChild<QScrollArea *>("drawingPreviewScroll");
    auto *preview = dialog->findChild<QLabel *>("drawingPreviewImage");
    require(page && navigation && controls && previous && next && scroll && preview,
            "Missing page navigation controls");
    const auto basePages = page->maximum();
    require(navigation->isVisible() == (basePages > 1),
            "Single-page navigation visibility is wrong");
    layout->setCurrentIndex(1);
    QApplication::processEvents();
    require(page->maximum() > 1 && navigation->isVisible() && page->value() == 1 &&
                !previous->isEnabled() && next->isEnabled(),
            "Multipage navigation or its first boundary is wrong");
    const auto firstPage = preview->pixmap().toImage();
    next->click();
    require(page->value() == 2 && previous->isEnabled() && preview->pixmap().toImage() != firstPage,
            "Next did not display the second page");
    page->setValue(page->maximum());
    require(!next->isEnabled(), "Next remains enabled on the final page");
    next->click();
    require(page->value() == page->maximum(), "Final-page click wrapped around");
    previous->click();
    require(page->value() == page->maximum() - 1, "Previous did not select the preceding page");
    const auto key = [&](int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                         const QString &text = QString{}) {
        QKeyEvent press(QEvent::KeyPress, code, modifiers, text);
        QApplication::sendEvent(page, &press);
        QKeyEvent release(QEvent::KeyRelease, code, modifiers, text);
        QApplication::sendEvent(page, &release);
    };
    page->setFocus();
    key(Qt::Key_Right, Qt::AltModifier);
    require(page->value() == page->maximum(), "Alt+Right did not use the next-page action");
    key(Qt::Key_Left, Qt::AltModifier);
    require(page->value() == page->maximum() - 1, "Alt+Left did not use the previous-page action");
    page->findChild<QLineEdit *>()->setText(QStringLiteral("1"));
    key(Qt::Key_Return);
    require(page->value() == 1 && !previous->isEnabled(), "Typed page number was not committed");
    const auto clearOfPaper = [&] {
        if (!controls->isVisible())
            return;
        const auto nav = controls->geometry();
        require(scroll->rect().contains(nav) && !nav.intersects(scroll->viewport()->geometry()),
                "Navigation overlaps the paper viewport or escapes its preview frame");
        for (auto *bar : {scroll->horizontalScrollBar(), scroll->verticalScrollBar()})
            if (bar->isVisible())
                require(!nav.intersects(QRect(bar->mapTo(scroll, QPoint{}), bar->size())),
                        "Navigation overlaps a scrollbar");
    };
    clearOfPaper();
    const auto normal = dialog->size();
    const auto snapshot = qEnvironmentVariable("BENDING_DRAWING_DIALOG_SNAPSHOT");
    const auto fittedPaper = [&] {
        QApplication::processEvents();
        wait([&] {
            return !preview->pixmap().isNull() &&
                   preview->pixmap().width() ==
                       std::clamp(qRound(preview->width() * preview->devicePixelRatioF()), 2, 4096);
        });
        require(scroll->viewport()->rect().contains(preview->geometry()),
                "Resized paper does not fit inside the preview viewport");
        require(std::abs(preview->width() * ui::DrawingSheet::heightMm -
                         preview->height() * ui::DrawingSheet::widthMm) <=
                    ui::DrawingSheet::widthMm,
                "Resizing distorted the A3 sheet proportions");
        require(!scroll->horizontalScrollBar()->isVisible() &&
                    !scroll->verticalScrollBar()->isVisible(),
                "Fitted paper still requires scrolling");
        clearOfPaper();
        return preview->size();
    };
    const auto originalPaper = fittedPaper();
    const auto selectedPage = page->value();
    const auto totalPages = page->maximum();
    dialog->resize(1500, 950);
    const auto expandedPaper = fittedPaper();
    require(expandedPaper.width() > originalPaper.width() &&
                expandedPaper.height() > originalPaper.height(),
            "Expanding the window did not enlarge the paper and its sharp raster");
    if (!snapshot.isEmpty())
        require(dialog->grab().save(snapshot + ".expanded.png"),
                "Cannot save the expanded preview snapshot");
    dialog->resize(900, 560);
    const auto compactPaper = fittedPaper();
    require(compactPaper.width() < originalPaper.width() &&
                compactPaper.height() < originalPaper.height(),
            "Shrinking the window did not reduce the paper and its sharp raster");
    require(page->value() == selectedPage && page->maximum() == totalPages,
            "Window resizing changed the selected page or page count");
    auto *name = dialog->findChild<QLineEdit *>("drawingName");
    auto *options = dialog->findChild<QScrollArea *>("drawingOptionsScroll");
    require(options && options->verticalScrollBar()->isVisible() &&
                name->height() >= name->minimumSizeHint().height() &&
                dialog->rect().contains(
                    QRect(exportButton->mapTo(dialog, QPoint{}), exportButton->size())),
            "Compact window crushed the input fields or hid the PDF action");
    if (!snapshot.isEmpty())
        require(dialog->grab().save(snapshot + ".compact.png"),
                "Cannot save the compact preview snapshot");
    next->click();
    fittedPaper();
    dialog->resize(normal);
    layout->setCurrentIndex(0);
    fittedPaper();
    require(page->maximum() == basePages && page->value() <= basePages &&
                navigation->isVisible() == (basePages > 1),
            "Page count reduction left a stale selector");
    if (basePages == 1)
        require(page->value() == 1 && !previous->isEnabled() && !next->isEnabled(),
                "Single-page layout retained active navigation actions");
    require(c.drawing() == oldDrawing && !c.hasWork() && exportButton->isEnabled(),
            "Changing presentation recomputed or invalidated the CAD snapshot");
    auto *fit = dialog->findChild<QToolButton *>("fitDrawingPageButton");
    auto *zoom = dialog->findChild<QLabel *>("drawingPreviewZoom");
    require(fit && zoom && fit->isChecked() && controls->isVisible(),
            "Missing zoom controls on a single-page or fitted sheet");
    const auto sharp = [&] {
        QApplication::processEvents();
        wait([&] {
            return preview->pixmap().width() ==
                   std::clamp(qRound(preview->width() * preview->devicePixelRatioF()), 2, 4096);
        });
    };
    const auto wheel = [&](QWidget *target, int delta, const QPoint &position,
                           bool pixels = false) {
        QWheelEvent event(QPointF(position), QPointF(target->mapToGlobal(position)),
                          pixels ? QPoint(0, delta) : QPoint{},
                          pixels ? QPoint{} : QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &event);
        QApplication::processEvents();
    };
    const auto anchor = scroll->viewport()->rect().center() + QPoint(40, 20);
    const auto point = preview->mapFrom(scroll->viewport(), anchor);
    const auto beforeZoom = preview->size();
    wheel(preview, 120, point);
    sharp();
    const auto after = preview->mapFrom(scroll->viewport(), anchor);
    require(
        preview->width() > beforeZoom.width() && !fit->isChecked() &&
            scroll->horizontalScrollBar()->isVisible() &&
            scroll->verticalScrollBar()->isVisible() &&
            std::abs(after.x() * double(beforeZoom.width()) / preview->width() - point.x()) < 2 &&
            std::abs(after.y() * double(beforeZoom.height()) / preview->height() - point.y()) < 2,
        "Wheel zoom failed to enlarge the sheet around the cursor");
    auto *horizontal = scroll->horizontalScrollBar();
    auto *vertical = scroll->verticalScrollBar();
    horizontal->setValue(horizontal->maximum() / 2);
    vertical->setValue(vertical->maximum() / 2);
    const QPoint oldScroll(horizontal->value(), vertical->value());
    const QPoint dragDelta(std::min(20, horizontal->maximum() / 4),
                           std::min(15, vertical->maximum() / 4));
    const auto mouse = [&](QEvent::Type type, const QPoint &position, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(position),
                          QPointF(scroll->viewport()->mapToGlobal(position)), button, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(scroll->viewport(), &event);
    };
    mouse(QEvent::MouseButtonPress, anchor, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, anchor - dragDelta, Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, anchor - dragDelta, Qt::LeftButton, Qt::NoButton);
    require(horizontal->value() == oldScroll.x() + dragDelta.x() &&
                vertical->value() == oldScroll.y() + dragDelta.y(),
            "Dragging did not move the enlarged sheet");
    const auto manualSize = preview->size();
    dialog->resize(normal + QSize(80, 60));
    QApplication::processEvents();
    require(preview->size() == manualSize, "Window resize overrode manual zoom");
    dialog->resize(normal);
    for (int i = 0; i < 8; ++i)
        wheel(scroll->viewport(), 480, anchor);
    sharp();
    require(zoom->text() == QStringLiteral("400%") && preview->pixmap().width() <= 4096,
            "Zoom exceeded its scale or raster memory limit");
    clearOfPaper();
    if (!snapshot.isEmpty())
        require(dialog->grab().save(snapshot + ".zoom.png"), "Cannot save zoomed preview");
    for (int i = 0; i < 10; ++i)
        wheel(scroll->viewport(), -480, anchor);
    sharp();
    require(zoom->text() == QStringLiteral("10%"), "Zoom-out did not stop at its minimum");
    const auto smallWidth = preview->width();
    wheel(scroll->viewport(), 25, anchor, true);
    require(preview->width() > smallWidth, "Pixel-delta touchpad gesture did not zoom");
    fit->click();
    fittedPaper();
    require(fit->isChecked() && c.drawing() == oldDrawing && !c.hasWork() &&
                c.session().editRevision() == revision,
            "Fit/zoom/pan changed CAD or the project");
    dialog->findChild<QLineEdit *>("drawingExecution")->setText(QStringLiteral("Исполнение Л"));
    inspected = !dialog->findChild<QLabel *>("drawingPreviewImage")->pixmap().isNull() &&
                c.session().editRevision() == revision;
    if (!snapshot.isEmpty()) {
        if (page->maximum() > 1)
            page->setValue(page->maximum());
        scroll->setFocus();
        QApplication::processEvents();
        inspected = inspected && dialog->grab().save(snapshot);
    }
    layout->setCurrentIndex(1);
    page->setValue(page->maximum());
    exportedPages = page->maximum();
    require(exportedPages > 1 && c.drawing() == oldDrawing && !c.hasWork() &&
                c.session().editRevision() == revision,
            "Page changes recomputed CAD or changed the project");
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
        QTimer::singleShot(
            0, file, [file] { QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection); });
    });
    exportButton->click();
    pdf = QFile::exists(target);
    dialog->findChild<QComboBox *>("drawingOrientation")->setCurrentIndex(1);
    require(!controls->isVisible() && !navigation->isVisible() && !previous->isEnabled() &&
                !next->isEnabled() && preview->pixmap().isNull() && !exportButton->isEnabled(),
            "Invalidated views retained an active page navigator or stale paper");
    QPointer<QDialog> closedDialog = dialog;
    dialog->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(closedDialog.isNull(), "Closed drawing window retained its widgets");
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
        const auto pages = QRegularExpression(QStringLiteral("/Count\\s+(\\d+)"))
                               .match(QString::fromLatin1(bytes));
        require(pages.hasMatch() && pages.captured(1).toInt() == exportedPages,
                "Export saved only the selected preview page");
    }
    require(c.session().editRevision() == revision, "Drawing metadata edited project");
    const auto old = c.drawing();
    require(c.prepareDrawing(builder, {}) && !c.hasWork() && c.drawing() == old,
            "Reopening recomputed current drawing");
    action->trigger();
    QPointer<QDialog> reopened = window.findChild<QDialog *>("bendingDrawingDialog");
    require(reopened && reopened->isVisible(), "Closed drawing window cannot be reopened");
    wait([&] {
        return !c.hasWork() && reopened->findChild<QPushButton *>("exportDrawingPdf")->isEnabled();
    });
    require(c.setUnfoldSettings({core::SheetMaterial::MildSteel, .5, 1}) && !c.drawing(),
            "Material rule change retained an obsolete PDF snapshot");
    require(reopened->findChild<QLabel *>("drawingPreviewImage")->pixmap().isNull() &&
                !reopened->findChild<QPushButton *>("exportDrawingPdf")->isEnabled(),
            "Main-window parameter change left an exportable obsolete sheet");
    reopened->findChild<QPushButton *>("prepareDrawing")->click();
    require(c.isPreparingDrawing(), "Second drawing did not start");
    reopened->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(reopened.isNull(), "Closing a busy preview did not delete the window");
    wait([&] { return !c.hasWork(); });
    require(!c.drawing(), "Closing a busy preview published its cancelled sheet");
    action->trigger();
    QPointer<QDialog> last = window.findChild<QDialog *>("bendingDrawingDialog");
    require(last, "Cannot reopen the cancelled preview");
    wait([&] {
        return !c.hasWork() && last->findChild<QPushButton *>("exportDrawingPdf")->isEnabled();
    });
    last->findChild<QComboBox *>("drawingOrientation")->setCurrentIndex(1);
    last->findChild<QPushButton *>("prepareDrawing")->click();
    require(c.isPreparingDrawing(), "Drawing did not start before closing the part");
    c.closeDocument();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(last.isNull(), "Closing the part left its drawing window open");
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
        if (app.arguments().contains(QStringLiteral("--workflow-only"))) {
            workflow(temp.path());
            return 0;
        }
        if (app.arguments().size() == 5 && app.arguments().at(1) == "--audit-sample") {
            auditSample(app.arguments().at(2), app.arguments().at(3), app.arguments().at(4));
            return 0;
        }
        if (app.arguments().size() == 4 && app.arguments().at(1) == "--flat-sample") {
            const auto doc = application::resultDocument(
                cad::StepImporter{}.load(native(app.arguments().at(2)), {}, {}));
            require(doc != nullptr, "Sample import failed");
            application::DrawingRequest request;
            request.unfoldSettings = core::UnfoldSettings{core::SheetMaterial::MildSteel, {}, 1};
            const auto d = build(doc, request);
            require(ui::DrawingSheet::writePdf(app.arguments().at(3), *d, {}).isEmpty(),
                    "Sample PDF failed");
            std::cout << "PAGES " << ui::DrawingSheet::pageCount(*d) << " FLAT "
                      << bool(d->flatPattern) << '\n';
            return 0;
        }
        flatDrawing(temp.path());
        drawingMeasurements(temp.path());
        variableProfile(temp.path());
        holePositions(temp.path());
        ovalVisibility(temp.path());
        geometry(temp.path());
        operatorProfiles(temp.path());
        classicalDrawing(temp.path());
        workflow(temp.path());
        std::cout << "Drawing geometry, layout and PDF workflow passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
