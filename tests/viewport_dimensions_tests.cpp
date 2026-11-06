#include "Dimensions.h"

#include <QApplication>
#include <QLocale>
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
        const auto overview = viewer::flatPatternDimensions(info, 14, layout, {}, 1);
        check(overview.labels.size() == 34 && !overview.labels.join(' ').contains(" · L "),
              "Overview must have overall sizes and compact IDs, not full callouts");
        const auto selected = viewer::flatPatternDimensions(info, 14, layout, 7, 1).labels;
        check(
            selected.size() == 4 &&
                selected.join(' ').contains(QStringLiteral("B08 · ВНИЗ · 125° · R 3 · L %1 мм")
                                                .arg(QLocale::system().toString(1000.0, 'f', 0))) &&
                selected.join(' ').contains("Y 302 мм"),
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
        check(viewer::flatPatternDimensions(info, 14, layout, 7, 1).labels.size() == 3,
              "Oblique bend midpoint was mislabelled as an edge distance");
        // Screen-space pruning preserves the selected callout and text height;
        // it must neither change geometry nor invent a typical repeated feature.
        layout.viewport = {0, 0, 900, 700};
        layout.project = [](const gp_Pnt &) { return QPointF(400, 300); };
        const auto dense = viewer::flatPatternDimensions(info, 14, layout, {}, 1).labels;
        check(dense.size() == 3, "Overlapping compact IDs were not suppressed");
        layout.excluded = {{380, 275, 60, 60}};
        check(viewer::flatPatternDimensions(info, 14, layout, {}, 1).labels.size() == 2,
              "Flat markers overlapped a viewport control");
        layout.excluded.clear();
        layout.project = [](const gp_Pnt &) { return QPointF(-30, -30); };
        check(viewer::flatPatternDimensions(info, 14, layout, {}, 1).labels.size() == 2,
              "Clipped flat markers escaped the viewport");
        cad::PreviewFrame frame;
        frame.dimensions = cad::BendDimensionAnchors{{gp_Pnt(-50, 0, 0), gp_Pnt(0, 0, 0)},
                                                     {gp_Pnt(10, 0, 0), gp_Pnt(50, 40, 0)},
                                                     gp_Pnt(5, 0, 0),
                                                     gp_Vec(0, 0, 100),
                                                     55 * std::numbers::pi / 180,
                                                     3};
        simulation::SingleBend model{1.5, 3, 55 * std::numbers::pi / 180, 100, 50, 40};
        const auto paused = viewer::previewDimensions(frame, model, 14, layout, false).labels;
        const auto playing = viewer::previewDimensions(frame, model, 14, layout, true).labels;
        check(paused.size() == 4 && paused.join(' ').contains("П1 прям. 50 мм") &&
                  paused.join(' ').contains("Угол 125°"),
              "Paused dimensions lost straight tangent lengths or used turn angle");
        check(playing.size() == 1 && playing.front() == "Угол 125°",
              "Animation must keep only the current internal angle");
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
