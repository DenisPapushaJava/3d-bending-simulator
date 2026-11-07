#include "Dimensions.h"
#include <PrsDim_LengthDimension.hxx>

#include <QApplication>
#include <QLocale>
#include <QRegularExpression>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace bending;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        viewer::DimensionLayout layout{gp_Vec(50, 0, 0), gp_Vec(0, 50, 0), {}, {}, {}};
        application::DocumentMetadata metadata{};
        metadata.bounds.maximum = {100, 100, 30};
        application::ModelMeasurements measurements;
        measurements.flanges = {{{0, 0, 0}, {20, 0, 0}, 20, {1, 2}},
                                {{0, 50, 0}, {20, 50, 0}, 20, {1, 2}},
                                {{0, 0, 30}, {20, 0, 30}, 20, {3, 4}},
                                {{0, 0, 0}, {100, 0, 0}, 100, {5, 6}}};
        const auto source = viewer::documentDimensions(metadata, {}, {}, 14, layout, &measurements);
        check(source.labels.filter(QStringLiteral("Полки 20 мм · 2 шт.")).size() == 1 &&
                  source.labels.size() == 4 && source.labels.join(' ').contains("X 100 мм") &&
                  source.labels.join(' ').contains("Y 100 мм"),
              "Repeated flanges or a covered extent were duplicated, or different axes collapsed");
        application::MeasuredOpening hole;
        hole.center = {20, 20, 0};
        hole.normal = {0, 0, 1};
        hole.lengthMm = hole.widthMm = 10;
        hole.circular = true;
        hole.hostFaces = {1};
        measurements.openings = {hole};
        application::MeasuredFace host;
        host.id = 1;
        host.outerEdges = {1, 3};
        measurements.faces = {host};
        for (std::size_t id = 1; id <= 3; ++id) {
            application::MeasuredEdge base;
            base.id = id;
            base.straight = true;
            base.first = id == 3 ? application::PointMm{100, 100, 0}
                                 : application::PointMm{0, (id - 1) * 10., 0};
            base.second = {110, base.first.y, 0};
            base.faces = {1};
            measurements.edges.push_back(base);
            const auto labels =
                viewer::documentDimensions(metadata, {}, {}, 14, layout, &measurements,
                                           {viewer::MeasurementKind::Opening, 0, {id}})
                    .labels;
            check(labels.filter(QStringLiteral("До базы")).size() == (id == 1 ? 1 : 0),
                  "Manual datum accepted an inner wire or extended a finite external edge");
        }
        application::FlatPatternInfo info;
        info.bounds.maximum = {1000, 1500, 1.5};
        info.thicknessMm = 1.5;
        for (std::size_t i = 0; i < 32; ++i)
            info.bends.push_back({i,
                                  {0, 20.0 + i * 40, 0},
                                  {1000, 20.0 + i * 40, 0},
                                  {0, 4, 0},
                                  55 * std::numbers::pi / 180,
                                  i % 2 ? -1 : 1,
                                  3,
                                  .45,
                                  4});
        auto stockMeasures = std::make_shared<application::ModelMeasurements>();
        application::MeasuredEdge stockEdge;
        stockEdge.id = 100;
        stockEdge.straight = true;
        stockEdge.first = {0, 0, 0};
        stockEdge.second = {1000, 0, 0};
        stockMeasures->edges.push_back(stockEdge);
        stockMeasures->datumEdges = {100};
        info.measurements = stockMeasures;
        const auto overview = viewer::flatPatternDimensions(info, 14, layout, {}, 1);
        check(overview.labels.filter(QRegularExpression(QStringLiteral("^B[0-9]+$"))).size() ==
                      32 &&
                  !overview.labels.join(' ').contains(" · Y ") &&
                  !overview.labels.join(' ').contains(" · L "),
              "Overview must have overall sizes and compact IDs, not full callouts");
        const auto selectedAnnotations = viewer::flatPatternDimensions(info, 14, layout, 7, 1);
        const auto selected = selectedAnnotations.labels;
        const auto dimensions = [](const viewer::DimensionAnnotations &annotations) {
            std::vector<Handle(PrsDim_LengthDimension)> result;
            for (const auto &object : annotations.objects)
                if (const auto dimension = Handle(PrsDim_LengthDimension)::DownCast(object);
                    !dimension.IsNull())
                    result.push_back(dimension);
            return result;
        };
        const auto selectedDimensions = dimensions(selectedAnnotations);
        check(selectedDimensions.size() == 3 &&
                  std::abs(selectedDimensions.back()->FirstPoint().Y()) < 1.e-6 &&
                  std::abs(selectedDimensions.back()->SecondPoint().Y() - 302) < 1.e-6,
              "Selected bend lacks a dimension between the real edge and its centre line");
        check(
            !selected.join(' ').contains(" · Y ") &&
                selected.join(' ').contains(QStringLiteral("B08 · ВНИЗ · 125° · R 3 · L %1 мм")
                                                .arg(QLocale::system().toString(1000.0, 'f', 0))) &&
                selected.contains(QStringLiteral("302 мм")),
            "Selected dimensions lost internal angle/radius/length/coordinate or used step ID");
        const auto reverse = viewer::flatPatternDimensions(info, 14, layout, 7, -1).labels;
        check(reverse.size() == selected.size() && reverse.join(' ').contains("B08 · ВВЕРХ · 125°"),
              "Directions must reverse with the shown physical side");
        check(viewer::flatPatternDimensions(info, 14, layout, 7, 0).labels.size() == 2,
              "Edge-on flat retained ambiguous detailed dimensions");
        layout.project = [](const gp_Pnt &p) { return QPointF(p.X(), p.Y()); };
        check(viewer::flatBendAt(info, layout, {500, 302}, 8) == 7 &&
                  !viewer::flatBendAt(info, layout, {1020, 302}, 8) &&
                  !viewer::flatBendAt(info, layout, {500, 319}, 8),
              "Flat picking lost the prepared source ID or extended a line beyond its endpoint");
        info.bends[7].end.y += 50;
        check(dimensions(viewer::flatPatternDimensions(info, 14, layout, 7, 1)).size() == 2,
              "Oblique line without a parallel exterior datum acquired a fictitious dimension");
        // A subtracted contour can have an empty bounding corner. Use a finite
        // actual edge and reject a nearer edge outside the bend's axial interval.
        auto relieved = info;
        relieved.bounds.maximum = {100, 100, 1.5};
        relieved.bends = {{0, {20, 40, 0}, {80, 40, 0}, {}, std::numbers::pi / 2, 1, 3, .45, 4}};
        auto relievedMeasures = std::make_shared<application::ModelMeasurements>();
        auto realEdge = stockEdge;
        realEdge.first = {0, 10, 0};
        realEdge.second = {100, 10, 0};
        auto shortEdge = realEdge;
        shortEdge.id = 101;
        shortEdge.first = {110, 39, 0};
        shortEdge.second = {130, 39, 0};
        auto innerEdge = realEdge;
        innerEdge.id = 102;
        innerEdge.first.y = innerEdge.second.y = 39;
        relievedMeasures->edges = {realEdge, shortEdge, innerEdge};
        relievedMeasures->datumEdges = {100, 101};
        relieved.measurements = relievedMeasures;
        const auto actual = dimensions(viewer::flatPatternDimensions(relieved, 14, layout, 0, 1));
        check(actual.size() == 3 && std::abs(actual.back()->FirstPoint().Y() - 10) < 1.e-6 &&
                  std::abs(actual.back()->SecondPoint().Y() - 40) < 1.e-6,
              "Bend dimension used a bounding corner, an inner contour or a prolonged edge");
        // The same finite-edge rule works for a rotated profile, without X/Y guessing.
        relieved.bends.front().start = {20, 20, 0};
        relieved.bends.front().end = {80, 80, 0};
        relievedMeasures->edges.front().first = {0, 10, 0};
        relievedMeasures->edges.front().second = {50, 60, 0};
        const auto oblique = dimensions(viewer::flatPatternDimensions(relieved, 14, layout, 0, -1));
        check(oblique.size() == 3 &&
                  std::abs(oblique.back()->FirstPoint().Distance(oblique.back()->SecondPoint()) -
                           std::sqrt(50.)) < 1.e-6,
              "Rotated bend lost its true perpendicular distance or changed with physical side");
        // Collinear split zones share an ID marker, not a coordinate or merged operation.
        auto split = relieved;
        split.bends = {{0, {0, 40, 0}, {20, 40, 0}, {}, std::numbers::pi / 2, 1, 3, .45, 4},
                       {1, {30, 40, 0}, {50, 40, 0}, {}, std::numbers::pi / 2, 1, 3, .45, 4}};
        layout.project = {};
        const auto splitLabels = viewer::flatPatternDimensions(split, 14, layout, {}, 1).labels;
        check(splitLabels.contains(QStringLiteral("B01/B02")) && splitLabels.size() == 3,
              "Split lines lost source IDs or kept a permanent Y coordinate");
        // Screen-space pruning preserves the selected callout and text height;
        // it must neither change geometry nor invent a typical repeated feature.
        layout.viewport = {0, 0, 900, 700};
        layout.project = [](const gp_Pnt &) { return QPointF(400, 300); };
        const auto dense = viewer::flatPatternDimensions(info, 14, layout, {}, 1).labels;
        check(dense.filter(QRegularExpression(QStringLiteral("^B[0-9]+$"))).size() <= 1,
              "Overlapping compact IDs were not suppressed");
        layout.excluded = {{380, 275, 60, 60}};
        check(viewer::flatPatternDimensions(info, 14, layout, {}, 1)
                  .labels.filter(QRegularExpression(QStringLiteral("^B[0-9]+$")))
                  .empty(),
              "Flat markers overlapped a viewport control");
        layout.excluded.clear();
        layout.project = [](const gp_Pnt &) { return QPointF(-30, -30); };
        check(viewer::flatPatternDimensions(info, 14, layout, {}, 1)
                  .labels.filter(QRegularExpression(QStringLiteral("^B[0-9]+$")))
                  .empty(),
              "Clipped flat markers escaped the viewport");
        cad::PreviewFrame frame;
        frame.dimensions = cad::BendDimensionAnchors{{gp_Pnt(-50, 0, 0), gp_Pnt(0, 0, 0)},
                                                     {gp_Pnt(10, 0, 0), gp_Pnt(50, 40, 0)},
                                                     gp_Pnt(5, 0, 0),
                                                     gp_Vec(0, 0, 100),
                                                     55 * std::numbers::pi / 180,
                                                     3};
        simulation::SingleBend model{1.5, 3, 55 * std::numbers::pi / 180, 100, 50, 40};
        const auto section = *simulation::singleBendSection(model, 1);
        const auto outer = *simulation::outsideFlangePoints(section);
        frame.dimensions->outsideFlanges = {gp_Pnt(outer[0].x, outer[0].y, 0),
                                            gp_Pnt(outer[1].x, outer[1].y, 0),
                                            gp_Pnt(outer[2].x, outer[2].y, 0)};
        const auto paused = viewer::previewDimensions(frame, model, 14, layout, false).labels;
        const auto playing = viewer::previewDimensions(frame, model, 14, layout, true).labels;
        check(paused.size() == 4 && paused.join(' ').contains("Полка 1") &&
                  paused.join(' ').contains("Угол 125°"),
              "Paused dimensions lost straight tangent lengths or used turn angle");
        check(playing.size() == 4 && playing.join(' ').contains("Угол 125°") &&
                  playing.join(' ').contains("Полка 1"),
              "Animation lost persistent outside dimensions");
        frame.dimensions->angleRadians = 0;
        frame.dimensions->innerRadiusMm.reset();
        check(
            viewer::previewDimensions(frame, model, 14, layout).labels.join(' ').contains("плоско"),
            "Flat frame retained a finite bend radius");
        std::cout
            << "Contextual dimensions: 32 bends, selection, side, clipping and playback passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
