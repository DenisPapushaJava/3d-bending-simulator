#include "ToolFixtures.h"
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
        const auto drawn = tool_fixture::pair();
        auto horn = drawn;
        horn.punch.segments = std::vector<ToolSegment>{
            {100, 1, SegmentShape::LeftHorn, HornRelief{27, 27, 9}, "left"},
            {200, 1},
            {100, 1, SegmentShape::RightHorn, HornRelief{27, 27, 9}, "right"},
            {100, 1, SegmentShape::LeftHorn}};
        horn.punchAssembly = ToolAssembly{{0, 1, 2}, 0, {}};
        require(validToolPreset(horn.punch) && toolParameters(horn) &&
                    toolLength(horn.punch, horn.punchAssembly) == 400 &&
                    hornRootHeight({27, 27, 9}) == 36 && hornRootHeight({30, 20, 9}) == 29,
                "Configured horn rejected or root height incorrect");
        const auto &stock = *horn.punch.segments;
        require(!assemblyLength(stock, {{3}, 0, {}}) && !assemblyLength(stock, {{1, 0}, 0, {}}) &&
                    !assemblyLength(stock, {{2, 1}, 0, {}}) &&
                    !assemblyLength(stock, {{0, 0, 2}, 0, {}}) &&
                    assemblyLength(stock, {{0}, 0, {}}) == 100 &&
                    planToolSegments(stock, 300).status == SegmentPlanStatus::InsufficientStock,
                "Unknown horns, wrong ends or automatic horn assembly admitted");
        auto distinct = stock;
        distinct[3].horn = HornRelief{26, 27, 9};
        distinct[3].variantId = "left-other";
        require(validToolSegments(distinct), "Different horn shapes with equal length merged");
        distinct[3].horn = distinct[0].horn;
        require(!validToolSegments(distinct), "Duplicate horn shape counted twice");
        for (int test = 0; test < 7; ++test) {
            auto invalid = horn;
            auto &s = invalid.punch.segments->front();
            if (test == 0)
                s.horn->projectionMm = 100;
            if (test == 1)
                s.horn->radiusMm = std::numeric_limits<double>::quiet_NaN();
            if (test == 2)
                s.horn->tipHeightMm = 0;
            if (test == 3)
                s.horn->radiusMm = 100000;
            if (test == 4)
                s.variantId.clear();
            if (test == 5)
                s.variantId = "bad/id";
            if (test == 6)
                s.shape = SegmentShape::Straight;
            require(!validToolPreset(invalid.punch), "Invalid horn dimensions/identity accepted");
        }
        auto idealizedHorn = horn;
        idealizedHorn.useDrawingGeometry = false;
        require(!toolParameters(idealizedHorn), "Horn silently replaced by idealized punch");
        auto dieHorn = horn.punch;
        dieHorn.kind = ToolKind::Die;
        require(!validToolPreset(dieHorn), "Punch horn template accepted for die");
        require(hasCompleteDrawingToolGeometry(drawn), "Explicit tool bodies were rejected");
        require(hasLibraryProfile(drawn.punch), "Captured DXF missing from workshop policy");
        auto missingProfile = drawn.punch;
        missingProfile.drawing.reset();
        require(!hasLibraryProfile(missingProfile), "Parameter-only record admitted to workshop");
        const auto measured = detectPunchProfile(*drawn.punch.drawing);
        const auto size = toolProfileSize(*drawn.punch.drawing);
        require(measured && size &&
                    std::abs(measured->angleRadians - drawn.punch.angleRadians) < 1.e-8 &&
                    std::abs(measured->noseRadiusMm - *drawn.punch.noseRadiusMm) < 1.e-8 &&
                    std::abs(size->heightMm - 45) < 1.e-8,
                "DXF nose/dimensions differ from simulation references");
        for (double degrees : {26., 60., 90., 120.}) {
            const auto profile =
                tool_fixture::explicitPair(
                    {48, std::numbers::pi / 2, degrees * std::numbers::pi / 180, 2.5, 80, 10})
                    .punch;
            const auto m = detectPunchProfile(*profile.drawing);
            require(m && std::abs(m->angleRadians - profile.angleRadians) < 1.e-8 &&
                        std::abs(m->noseRadiusMm - 2.5) < 1.e-8,
                    "Nose detection depends on a particular angle or radius");
        }
        auto unsupported = *drawn.punch.drawing;
        for (auto &edge : unsupported.edges) {
            edge.center.reset();
            edge.sweepRadians = 0;
        }
        require(validToolDrawing(unsupported) && toolProfileSize(unsupported) &&
                    !detectPunchProfile(unsupported),
                "Sharp/unsupported profile invented a rounded nose");
        require(!detectPunchProfile(rotateToolDrawing(*drawn.punch.drawing, std::numbers::pi)) &&
                    !hasLibraryProfile(missingProfile),
                "Upside-down DXF invented an upright nose");
        auto incomplete = drawn;
        incomplete.useDrawingGeometry = false;
        require(!hasCompleteDrawingToolGeometry(incomplete),
                "Legacy idealized assignment passed real-body policy");
        incomplete = drawn;
        incomplete.die.drawing.reset();
        require(!hasCompleteDrawingToolGeometry(incomplete),
                "Partial DXF pair passed real-body policy");
        incomplete = drawn;
        incomplete.punch.teaching = true;
        require(!hasCompleteDrawingToolGeometry(incomplete),
                "Teaching profile passed real-body policy");
        require(!defaultPunchOrientation(drawn.punch).has_value(),
                "Symmetric punch invented an operator-side preference");
        require(defaultPunchOrientation(tool_fixture::offsetPunch()) == true,
                "Default punch relief is not toward the operator/open throat");
        require(defaultPunchOrientation(tool_fixture::offsetPunch(true)) == false,
                "Mirrored DXF lost the same physical default");
        require(!defaultPunchOrientation(drawn.die), "A die invented a punch orientation");
        auto tang = tool_fixture::tangPair();
        const auto mount = punchMountReference(tang);
        require(mount && std::abs(mount->heightMm - 30) < 1.e-6 &&
                    std::abs(mount->shoulderY - 45) < 1.e-6 && std::abs(mount->centreX - 1) < 1.e-6,
                "Top tang/shoulder mount not recognized");
        require(!punchMountReference(drawn), "Plain profile invented a tang mount");
        tang.punchReversed = true;
        const auto reversedMount = punchMountReference(tang);
        require(reversedMount && std::abs(reversedMount->centreX + 1) < 1.e-6 &&
                    toolParameters(tang)->punchReversed,
                "Orientation lost mount or assignment");
        auto machine = drawn;
        machine.machineModel = MachineModel::DemoPressV1;
        require(toolParameters(machine)->machineModel == MachineModel::DemoPressV1,
                "Assignment lost machine scope");
        machine.machineModel = static_cast<MachineModel>(999);
        require(!toolParameters(machine), "Unknown machine model accepted");
        const auto multi = tool_fixture::fourWayDie();
        require(validToolPreset(multi) && multi.grooveSettings.size() == 4,
                "Four-way die not recognized");
        for (std::size_t g = 0; g < 4; ++g) {
            const ToolAssignment a{drawn.punch, multi, g, 3, "", true};
            require(toolParameters(a).has_value() && drawingToolGeometry(a).has_value(),
                    "Quarter-turn groove not recognized");
            require(hasCompleteDrawingToolGeometry(a),
                    "Multi-V physical body lost its working side");
            require(std::abs(toolParameters(a)->dieAngleRadians -
                             (60 + 10 * static_cast<double>(g)) * std::numbers::pi / 180) < 1.e-10,
                    "Global angle leaked into active groove");
        }
        auto wrongMulti = multi;
        wrongMulti.grooveSettings.pop_back();
        require(!validToolPreset(wrongMulti), "Unbound groove rotation accepted");
        wrongMulti = multi;
        wrongMulti.grooveSettings[0].rotationRadians = std::numeric_limits<double>::quiet_NaN();
        require(!validToolPreset(wrongMulti), "NaN rotation accepted");
        const ToolSelectionRequest angleRequest{1, 95 * std::numbers::pi / 180, 8, 40};
        require(screenTools(angleRequest, drawn.punch, multi, 0) == ToolRejection::None &&
                    screenTools(angleRequest, drawn.punch, multi, 3) == ToolRejection::Angle,
                "Selection ignored per-groove angle");
        const auto geometry = drawingToolGeometry(drawn);
        require(geometry && toolParameters(drawn) &&
                    std::abs(geometry->shoulderHalfSpanMm - (6 + .8 / std::sqrt(3.))) < 1.e-8 &&
                    std::abs(geometry->punchTip.y) < 1.e-8,
                "Round shoulder references not recognized");
        require(!standardAirBendOpening(6, 12) && standardAirBendOpening(6, 50) &&
                    minimumStandardDieOpeningMm(6) == 30 && referenceDieOpeningMm(6) == 48,
                "Opening guard or 8t reference changed");
        auto v20 = tool_fixture::pair(20), v22 = tool_fixture::pair(22);
        v20.die.id = "die-v20";
        v22.die.id = "die-v22";
        const ToolSelectionRequest threeMm{3, std::numbers::pi / 2, 4, 40};
        require(minimumStandardDieOpeningMm(3) == 15 && referenceDieOpeningMm(3) == 24 &&
                    screenTools(threeMm, v20.punch, v20.die, 0) == ToolRejection::None &&
                    screenTools(threeMm, v22.punch, v22.die, 0) == ToolRejection::None,
                "8t recommendation became a hard minimum for 3 mm on V20/V22");
        const auto belowReference = selectTools({v20.punch, v20.die, v22.die}, threeMm);
        require(belowReference.candidates.size() == 2 && belowReference.candidates.front().die == 2,
                "Selection lost valid openings below the 8t preference");
        auto wide = tool_fixture::pair(50);
        wide.die.id = "wide";
        const auto choice =
            selectTools({drawn.punch, drawn.die, wide.die}, {6, std::numbers::pi / 2, 8, 40});
        require(choice.candidates.size() == 1 && choice.candidates[0].die == 2,
                "6 mm selection did not exclude V12/prefer available V50");
        auto partial = drawn;
        partial.die.drawing.reset();
        auto mixed = drawingToolGeometry(partial);
        require(mixed && mixed->hasPunch && !mixed->hasDie && toolParameters(partial),
                "Known punch depends on missing die drawing");
        partial = drawn;
        partial.punch.drawing.reset();
        mixed = drawingToolGeometry(partial);
        require(mixed && !mixed->hasPunch && mixed->hasDie && toolParameters(partial),
                "Known die depends on missing punch drawing");
        partial.die.drawing.reset();
        require(!drawingToolGeometry(partial) && !toolParameters(partial),
                "DXF mode without any contour was accepted");
        auto wrong = drawn;
        wrong.die.drawing->millimeters = false;
        require(!drawingToolGeometry(wrong) && !toolParameters(wrong),
                "Unknown units admitted real geometry");
        wrong = drawn;
        wrong.die.openingsMm[0] = 13;
        require(!drawingToolGeometry(wrong), "V mismatch was silently normalized");
        wrong = drawn;
        wrong.punch.noseRadiusMm = 1;
        require(!drawingToolGeometry(wrong), "Nose mismatch accepted");
        wrong = drawn;
        wrong.punch.drawing->edges[0].center->x += 1;
        require(!drawingToolGeometry(wrong), "Broken arc accepted");
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
        require(validToolCatalog({}) && !validToolCatalog({punch, punch}),
                "Empty workshop rejected or duplicate catalogue accepted");
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
