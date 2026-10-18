#include "bending/simulation/FlatToolSetup.h"
#include "bending/simulation/SingleBend.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void close(double actual, double expected, const char *message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1.0e-8, message);
}
} // namespace
int main() {
    using namespace bending::simulation;
    try {
        const SingleBend model{2, 4, std::numbers::pi / 2, 80, 30, 40};
        close(blankLengthMm(model), 70 + 5 * std::numbers::pi / 2, "Wrong developed length");
        const auto flat = singleBendSection(model, 0).value();
        require(!flat.innerRadiusMm, "Flat state has a finite radius");
        close(flat.innerArc[2].x, 5 * std::numbers::pi / 2, "Wrong flat bend interval");
        close(flat.innerArc[2].y, 0, "Flat inner surface not planar");
        close(flat.outerArc[2].y, -2, "Flat sheet changed thickness");
        const auto final = singleBendSection(model, 1).value();
        close(final.innerArc[2].x, 4, "Wrong final inner X");
        close(final.innerArc[2].y, 4, "Wrong final inner Y");
        close(final.outerArc[2].x, 6, "Wrong final outer X");
        close(final.outerArc[2].y, 4, "Wrong final outer Y");
        close(*singleBendSection(model, 0.5)->innerRadiusMm, 9, "Wrong intermediate radius");
        for (double progress : {0.0, std::numeric_limits<double>::denorm_min(), 1.0e-12, 1.0e-8,
                                0.05, 0.25, 0.5, 0.75, 1.0}) {
            const auto frame = singleBendSection(model, progress).value();
            for (std::size_t i = 0; i < 4; ++i) {
                close(frame.fixed[i].x, flat.fixed[i].x, "Fixed flange moved in X");
                close(frame.fixed[i].y, flat.fixed[i].y, "Fixed flange moved in Y");
            }
            for (std::size_t i = 0; i < 3; ++i) {
                close(std::hypot(frame.innerArc[i].x - frame.outerArc[i].x,
                                 frame.innerArc[i].y - frame.outerArc[i].y),
                      2, "Bend changed sheet thickness");
            }
            close(std::hypot(frame.moving[3].x - frame.moving[0].x,
                             frame.moving[3].y - frame.moving[0].y),
                  40, "Moving flange changed length");
            if (frame.innerRadiusMm) {
                close((*frame.innerRadiusMm + 1) * frame.angleRadians, bendAllowanceMm(model),
                      "Middle surface changed arc length");
            }
        }
        for (double progress : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()}) {
            require(!singleBendSection(model, progress), "Invalid progress accepted");
        }
        for (double angle : {0.0, std::numbers::pi, std::numeric_limits<double>::infinity()}) {
            auto invalid = model;
            invalid.angleRadians = angle;
            require(!singleBendSection(invalid, 0.5), "Unsupported angle accepted");
        }
        const bending::core::PressTools tools{
            48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
        const auto setup = std::get<FlatToolSetup>(flatToolSetup(model, tools, 1e-5));
        close(setup.grooveDepthMm, 24, "Wrong V depth");
        close(setup.sheetLeftMm, -30 - 5 * std::numbers::pi / 4, "Bend zone not centred");
        close(setup.leftOverlapMm, 6 + 5 * std::numbers::pi / 4, "Wrong left support");
        close(setup.rightOverlapMm, 16 + 5 * std::numbers::pi / 4, "Wrong right support");
        auto tooShort = tools;
        tooShort.lengthMm = 79;
        require(std::get<ToolSetupIssue>(flatToolSetup(model, tooShort, 1e-5)) ==
                    ToolSetupIssue::ToolTooShort,
                "Short tools accepted");
        auto wide = tools;
        wide.openingMm = 80;
        require(std::get<ToolSetupIssue>(flatToolSetup(model, wide, 1e-5)) ==
                    ToolSetupIssue::MissingLeftSupport,
                "Missing left support accepted");
        auto flipped = model;
        std::swap(flipped.fixedFlangeMm, flipped.movingFlangeMm);
        require(std::get<ToolSetupIssue>(flatToolSetup(flipped, wide, 1e-5)) ==
                    ToolSetupIssue::MissingRightSupport,
                "Missing right support accepted");
        wide.openingMm = -2 * setup.sheetLeftMm;
        require(std::get<ToolSetupIssue>(flatToolSetup(model, wide, 1e-5)) ==
                    ToolSetupIssue::MissingLeftSupport,
                "Zero support overlap accepted");
        for (const double bad : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
            auto invalidTools = tools;
            invalidTools.clearanceMm = bad;
            require(!bending::core::validPressTools(invalidTools),
                    "Invalid tool clearance accepted");
        }
        auto invalidTools = tools;
        invalidTools.noseRadiusMm = 13;
        require(!bending::core::validPressTools(invalidTools), "Oversized tool nose accepted");
        require(std::get<ToolSetupIssue>(flatToolSetup(model, tools, 0.1)) ==
                    ToolSetupIssue::InvalidParameters,
                "Excessive tolerance accepted");
        auto invalid = model;
        invalid.thicknessMm = -2;
        require(!singleBendSection(invalid, 0.5), "Negative thickness accepted");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout
        << "Single bend: endpoints, fixed side, thickness, neutral length and limits passed\n";
}
