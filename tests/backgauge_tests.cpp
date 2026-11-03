#include "bending/core/Backgauge.h"
#include <algorithm>
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
        invalid.minimumSpacingMm = -1;
        require(!validBackgaugeSetup(invalid), "Negative contact spacing accepted");
        invalid = s;
        invalid.retractMm = -1;
        require(!validBackgaugeSetup(invalid), "Negative withdrawal accepted");
        std::vector<GaugeContactPatch> patches;
        rectangle(patches, 80, 0, 2, 0, 300);
        auto candidates = backgaugeCandidates(patches, s, 2, 150);
        require(!candidates.empty() && candidates.front().level == 1 &&
                    candidates.front().zMm[1] - candidates.front().zMm[0] >= s.fingerWidthMm,
                "Straight-edge contacts missing");
        require(std::abs(candidates.front().xMm - 51.2) < 1.e-6,
                "Step offset did not affect shared X");
        for (double thickness : {.3, .6, 1.5, 3., 6., 12.}) {
            std::vector<GaugeContactPatch> edge;
            rectangle(edge, 80, 0, thickness, -150, 150);
            const auto raised = backgaugeCandidates(edge, s, thickness, 0);
            require(!raised.empty(), "Raised contact missing at a supported thickness");
            const double hy = std::min(thickness / 4, .5);
            for (const auto &p : raised) {
                const double bottom = p.rMm + backgaugeHeightsMm[p.level];
                const double top = p.rMm + backgaugeHeightsMm[p.level + 1];
                require(std::abs(bottom - .1) < 1.e-9 && p.contactYmm - hy >= bottom - 1.e-9 &&
                            p.contactYmm + hy <= top + 1.e-9 && p.contactYmm - hy >= 0 &&
                            p.contactYmm + hy <= thickness,
                        "Finite contact left the raised finger or sheet thickness");
            }
        }
        std::vector<GaugeContactPatch> tooThin;
        rectangle(tooThin, 80, 0, .2, 0, 300);
        require(backgaugeCandidates(tooThin, s, .2, 150).empty(), "Unsupported thin edge accepted");
        s.xMaxMm = 10;
        require(backgaugeCandidates(patches, s, 2, 150).empty(), "X range ignored");
        s = {};
        s.rMinMm = 40;
        s.rMaxMm = 60;
        require(backgaugeCandidates(patches, s, 2, 150).empty(), "R range ignored");
        s = {};
        std::vector<GaugeContactPatch> narrow;
        rectangle(narrow, 80, 0, 2, 0, 80);
        candidates = backgaugeCandidates(narrow, s, 2, 40);
        require(!candidates.empty() && std::abs(candidates.front().zMm[1] -
                                                candidates.front().zMm[0]) >= s.fingerWidthMm,
                "Short edge did not separate contacts from non-overlapping finger centres");
        require(candidates.front().contactZmm[1] > candidates.front().contactZmm[0] &&
                    candidates.front().contactZmm[1] - candidates.front().contactZmm[0] <
                        s.fingerWidthMm,
                "Two distinct off-centre contacts missing");
        for (const auto &p : candidates)
            for (std::size_t i = 0; i < 2; ++i)
                require(std::abs(p.contactZmm[i] - p.zMm[i]) + 1 <= s.fingerWidthMm / 2 &&
                            p.contactZmm[i] >= 1 && p.contactZmm[i] <= 79,
                        "Contact pad left its physical finger face or source edge");
        s.minimumSpacingMm = 110;
        require(backgaugeCandidates(narrow, s, 2, 40).empty(), "Explicit contact spacing ignored");
        s.minimumSpacingMm = 0;
        s.allowOffsetContacts = false;
        require(backgaugeCandidates(narrow, s, 2, 40).empty(), "Strict two-finger policy ignored");
        s = {};
        std::vector<GaugeContactPatch> cutout;
        rectangle(cutout, 80, 0, 2, 0, 60);
        rectangle(cutout, 80, 0, 2, 240, 300);
        candidates = backgaugeCandidates(cutout, s, 2, 150);
        require(!candidates.empty(), "Separated physical patches not supported");
        for (const auto &p : candidates)
            for (const auto z : p.contactZmm)
                require(z < 60 || z > 240, "Contact placed into a cutout");
        rectangle(patches, 60, 10, 40, 0, 300, GaugeReferenceKind::RightAngleWall);
        require(backgaugeCandidates(patches, s, 2, 150).front().kind ==
                    GaugeReferenceKind::StraightEdge,
                "Wall ranked before straight edge");
        std::vector<GaugeContactPatch> wall;
        rectangle(wall, 60, 10, 40, 0, 300, GaugeReferenceKind::RightAngleWall);
        require(!backgaugeCandidates(wall, s, 2, 150).empty(), "90-degree wall missing");
        for (const auto &p : backgaugeCandidates(wall, s, 2, 150))
            require(std::abs(p.rMm +
                             (backgaugeHeightsMm[p.level] + backgaugeHeightsMm[p.level + 1]) / 2 -
                             p.contactYmm) < 1.e-9,
                    "Die-referenced edge policy moved a 90-degree wall contact");
        std::stop_source stopped;
        stopped.request_stop();
        require(backgaugeCandidates(patches, s, 2, 150, stopped.get_token()).empty(),
                "Cancelled contact search continued");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
