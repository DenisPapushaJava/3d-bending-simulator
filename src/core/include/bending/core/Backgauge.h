#pragma once

#include <array>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace bending::core {
// Own simplified revision 1. X is the first nose, R the bottom of the finger;
// Z limits are finger centres relative to the centre of the tool. Not CNC datums.
struct BackgaugeSetup {
    double xMinMm = 0, xMaxMm = 800;
    double rMinMm = -125, rMaxMm = 125;
    double zMinMm = -1400, zMaxMm = 1400;
    double fingerWidthMm = 100.3;
    double minimumSpacingMm = 0; // Additional minimum between physical contact pads.
    double retractMm = 10;
    bool allowOffsetContacts = true;
    bool operator==(const BackgaugeSetup &) const = default;
};
inline constexpr std::array<double, 3> backgaugeOffsetsMm{0, 28.8, 65.9};
inline constexpr std::array<double, 4> backgaugeHeightsMm{0, 10.5, 21, 25.5};
inline constexpr double backgaugeEdgeClearanceMm = .1;
inline constexpr double backgaugeMinimumEdgeThicknessMm = .3;
enum class GaugeReferenceKind { StraightEdge, RightAngleWall };
struct GaugePointYZ {
    double y, z;
};
// A physical, outward +X planar triangle in the pre-bend source skin.
struct GaugeContactPatch {
    double xMm;
    std::array<GaugePointYZ, 3> vertices;
    GaugeReferenceKind kind;
};
struct BackgaugePlacement {
    GaugeReferenceKind kind;
    std::size_t level;
    double xMm, rMm;
    std::array<double, 2> zMm;
    double contactXmm, contactYmm;
    std::array<double, 2> contactZmm;
    bool operator==(const BackgaugePlacement &) const = default;
};
[[nodiscard]] bool validBackgaugeSetup(const BackgaugeSetup &) noexcept;
// Necessary axial span only; not a sufficient contact or collision test.
[[nodiscard]] double minimumGaugeSpanMm(const BackgaugeSetup &) noexcept;
// Conservative contact pads contained in physical triangles. A miss is not a
// proof of impossibility. Bounded shortlist; no 3D collision claim here.
[[nodiscard]] std::vector<BackgaugePlacement>
backgaugeCandidates(std::span<const GaugeContactPatch>, const BackgaugeSetup &, double thicknessMm,
                    double toolCentreZmm, std::stop_token = {});
} // namespace bending::core
