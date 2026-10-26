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
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
