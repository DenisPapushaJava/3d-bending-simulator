#include "bending/core/Backgauge.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace bending::core;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void rectangle(std::vector<GaugeContactPatch> &p, double x, double ya, double yb, double za,
               double zb, GaugeReferenceKind kind = GaugeReferenceKind::StraightEdge) {
    p.push_back({x, {{{ya, za}, {yb, za}, {yb, zb}}}, kind});
    p.push_back({x, {{{ya, za}, {yb, zb}, {ya, zb}}}, kind});
}
} // namespace
int main() {
    try {
        BackgaugeSetup s;
        require(validBackgaugeSetup(s), "Default setup invalid");
        auto invalid = s;
        invalid.rMaxMm = std::numeric_limits<double>::quiet_NaN();
        require(!validBackgaugeSetup(invalid), "NaN range accepted");
        invalid = s;
        invalid.minimumSpacingMm = 1;
        require(!validBackgaugeSetup(invalid), "Overlapping fingers accepted");
        invalid = s;
        invalid.retractMm = -1;
        require(!validBackgaugeSetup(invalid), "Negative withdrawal accepted");
        std::vector<GaugeContactPatch> patches;
        rectangle(patches, 80, 0, 2, 0, 300);
        auto candidates = backgaugeCandidates(patches, s, 2, 150);
        require(!candidates.empty() && candidates.front().level == 1 &&
                    candidates.front().zMm[1] - candidates.front().zMm[0] >= 110,
                "Straight-edge contacts missing");
        require(std::abs(candidates.front().xMm - 51.2) < 1.e-6,
                "Step offset did not affect shared X");
        s.xMaxMm = 10;
        require(backgaugeCandidates(patches, s, 2, 150).empty(), "X range ignored");
        s = {};
        s.rMinMm = 40;
        s.rMaxMm = 60;
        require(backgaugeCandidates(patches, s, 2, 150).empty(), "R range ignored");
        s = {};
        std::vector<GaugeContactPatch> narrow;
        rectangle(narrow, 80, 0, 2, 0, 80);
        require(backgaugeCandidates(narrow, s, 2, 40).empty(), "One finger became two contacts");
        std::vector<GaugeContactPatch> cutout;
        rectangle(cutout, 80, 0, 2, 0, 60);
        rectangle(cutout, 80, 0, 2, 240, 300);
        candidates = backgaugeCandidates(cutout, s, 2, 150);
        require(!candidates.empty(), "Separated physical patches not supported");
        for (const auto &p : candidates)
            for (const auto z : p.zMm)
                require(z < 60 || z > 240, "Contact placed into a cutout");
        rectangle(patches, 60, 10, 40, 0, 300, GaugeReferenceKind::RightAngleWall);
        require(backgaugeCandidates(patches, s, 2, 150).front().kind ==
                    GaugeReferenceKind::StraightEdge,
                "Wall ranked before straight edge");
        std::vector<GaugeContactPatch> wall;
        rectangle(wall, 60, 10, 40, 0, 300, GaugeReferenceKind::RightAngleWall);
        require(!backgaugeCandidates(wall, s, 2, 150).empty(), "90-degree wall missing");
        std::stop_source stopped;
        stopped.request_stop();
        require(backgaugeCandidates(patches, s, 2, 150, stopped.get_token()).empty(),
                "Cancelled contact search continued");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
