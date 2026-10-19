#include "bending/core/ToolCatalog.h"
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
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
