#include "bending/core/ToolCatalog.h"
#include "bending/core/ToolSelection.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main() {
    using namespace bending::core;
    try {
        const ToolPreset punch{"p",
                               "test",
                               "P26",
                               ToolKind::Punch,
                               26 * std::numbers::pi / 180,
                               525,
                               145,
                               .8,
                               {},
                               "own fixture",
                               "",
                               false};
        const ToolPreset die{"d", "test", "4V", ToolKind::Die,    std::numbers::pi / 3,
                             835, 60,     {},   {10, 16, 22, 35}, "own fixture",
                             "",  false};
        require(validToolCatalog({punch, die}), "Valid catalogue rejected");
        require(!validToolCatalog({}) && !validToolCatalog({punch, punch}),
                "Empty/duplicate catalogue accepted");
        for (std::size_t i = 0; i < die.openingsMm.size(); ++i) {
            const auto t = toolParameters({punch, die, i, 10, "Test machine"});
            require(t && t->openingMm == die.openingsMm[i] && t->lengthMm == 835 &&
                        punchLengthMm(*t) == 525 && t->noseRadiusMm == .8,
                    "Groove or independent length lost");
        }
        require(!toolParameters({punch, die, 4, 10, ""}) &&
                    !toolParameters({die, punch, 0, 10, ""}),
                "Bad assignment accepted");
        auto bad = die;
        bad.openingsMm = {10, 10};
        require(!validToolPreset(bad), "Duplicate groove accepted");
        bad = punch;
        bad.kind = static_cast<ToolKind>(55);
        require(!validToolPreset(bad), "Unknown kind accepted");
        bad = punch;
        bad.source.clear();
        require(!validToolPreset(bad), "Unattributed preset accepted");
        for (const double v : {0., -1., std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
            bad = punch;
            bad.lengthMm = v;
            require(!validToolPreset(bad), "Invalid length accepted");
            bad = punch;
            bad.noseRadiusMm = v;
            require(!validToolPreset(bad), "Invalid radius accepted");
            require(!toolParameters({punch, die, 0, v, ""}), "Invalid clearance accepted");
        }
        PressTools legacy{48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
        require(punchLengthMm(legacy) == 100 && validPressTools(legacy), "Legacy lengths changed");
        legacy.punchLengthMm = std::numeric_limits<double>::infinity();
        require(!validPressTools(legacy), "Invalid upper length accepted");
        const ToolSelectionRequest request{2, std::numbers::pi / 2, 3, 100};
        const auto result = selectTools({punch, die}, request);
        require(result.validRequest && result.candidates.size() == 4 &&
                    result.candidates.front().groove == 1 &&
                    result.candidates.front().openingThicknessRatio == 8,
                "Selection did not rank V16 for 2 mm first");
        require(screenTools({4, std::numbers::pi / 2, 3, 100}, punch, die, 1) ==
                    ToolRejection::NarrowOpening,
                "Out-of-scope V/t offered");
        require(screenTools({2, std::numbers::pi / 2, .8, 100}, punch, die, 1) ==
                    ToolRejection::Radius,
                "Nose radius equality silently certified");
        require(screenTools({2, std::numbers::pi * .8, 3, 100}, punch, die, 1) ==
                    ToolRejection::Angle,
                "Internal angle and bend turn confused");
        require(screenTools({2, std::numbers::pi / 2, 3, 600}, punch, die, 1) ==
                    ToolRejection::TooShort,
                "Short punch offered");
        bad = punch;
        bad.parametersConfirmed = false;
        bad.lengthMm = 0;
        require(validToolPreset(bad) && !toolParameters({bad, die, 0, 10, ""}) &&
                    selectTools({bad, die}, request).candidates.empty() &&
                    selectTools({bad, die}, request).missingData == 1,
                "Incomplete inventory became executable");
        bad = punch;
        bad.teaching = true;
        require(selectTools({bad, die}, request).candidates.empty(),
                "Teaching tool recommended as workshop stock");
        require(!selectTools({punch, die}, {0, 1, 3, 100}).validRequest &&
                    selectTools({punch, die}, {2, 1, 3, 10000}).candidates.empty(),
                "Bad input or impossible length accepted");
        const ToolDrawing drawing{"own.dxf",
                                  std::string(64, 'a'),
                                  true,
                                  {{{0, 0}, {2, 0}, {}, 0},
                                   {{2, 0}, {0, 2}, DrawingPoint{0, 0}, std::numbers::pi / 2},
                                   {{0, 2}, {0, 0}, {}, 0}}};
        require(validToolDrawing(drawing), "Analytic contour rejected");
        auto broken = drawing;
        broken.edges[1].sweepRadians = -std::numbers::pi / 2;
        require(!validToolDrawing(broken), "Wrong arc sweep accepted");
        broken = drawing;
        broken.edges.pop_back();
        require(!validToolDrawing(broken), "Open contour accepted");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
