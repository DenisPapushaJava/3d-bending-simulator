#include "bending/application/BendingDrawing.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

int main() {
    using namespace bending::application;
    try {
        const auto require = [](bool v) {
            if (!v)
                throw std::runtime_error("Drawing layout invariant failed");
        };
        require(standardDrawingScale(.31) == .25 && standardDrawingScale(.45) == .4);
        require(standardDrawingScale(1.9) == 1 && !standardDrawingScale(.00001));
        require(!standardDrawingScale(std::numeric_limits<double>::quiet_NaN()));
        BendingDrawing d;
        for (auto &v : d.views) {
            v.minimum = {0, 0};
            v.maximum = {100, 40};
            v.paths = {{{{0, 0}, {100, 40}}, false}};
        }
        require(drawingScale(d) == 1);
        for (auto &v : d.views)
            v.maximum.x = 2000;
        require(drawingScale(d) == .05);
        for (auto &v : d.views)
            v.omittedX = std::array<double, 2>{50, 1950};
        require(drawingScale(d) == .5);
        d.views[0].omittedX = std::array<double, 2>{-1, 100};
        require(!drawingScale(d));
        BendingDrawing grouped;
        grouped.bends.resize(2);
        grouped.details.resize(1);
        grouped.details[0].view.bendNotes = {{{0, 0}, 0}};
        require(!drawingProfilesCoverBends(grouped));
        grouped.details[0].view.bendNotes.push_back({{1, 1}, 1});
        require(drawingProfilesCoverBends(grouped));
        auto coverage = drawingMeasurementCoverage(grouped);
        require(coverage.total == 2 && coverage.dimensioned == 0 && coverage.missing.size() == 2);
        grouped.profiles.resize(1);
        grouped.profiles[0].view.dimensionedBends = {0, 0};
        grouped.details[0].view.dimensionedBends = {1};
        coverage = drawingMeasurementCoverage(grouped);
        require(coverage.total == 2 && coverage.dimensioned == 2 && coverage.missing.empty());
        coverage = drawingMeasurementCoverage(grouped, true);
        require(coverage.total == 2 && coverage.dimensioned == 1 &&
                coverage.missing == std::vector<std::size_t>{0});
        grouped.sections.resize(1);
        grouped.sections[0].view.dimensionedBends = {0};
        coverage = drawingMeasurementCoverage(grouped, true);
        require(coverage.dimensioned == 2 && coverage.missing.empty());
        grouped.bends[1].diagnostic = true;
        grouped.details[0].view.dimensionedBends.clear();
        coverage = drawingMeasurementCoverage(grouped);
        require(coverage.total == 1 && coverage.dimensioned == 1 && coverage.missing.empty());
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
