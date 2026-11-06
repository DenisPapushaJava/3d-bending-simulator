#pragma once
#include "bending/application/Document.h"
#include "bending/cad/occt/SheetOrientation.h"
#include "bending/core/BendTarget.h"
#include "bending/simulation/SingleBend.h"
#include <Poly_Triangulation.hxx>
#include <TopoDS_Shape.hxx>
#include <array>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bending::cad {
struct FrameBasis {
    gp_Pnt origin;
    gp_Vec x, y, z;
};
struct SkinNode {
    gp_Pnt point;
    unsigned regions = 0;
};
struct SkinFace {
    Handle(Poly_Triangulation) original;
    TopAbs_Orientation orientation;
    std::vector<SkinNode> nodes;
    std::size_t region = 0;
    SheetSide side = SheetSide::Neutral;
};
using WholePartSkin = std::vector<SkinFace>;
inline constexpr auto intrinsicSectionPoints =
    std::tuple_size_v<decltype(simulation::BendSection::fixed)> +
    std::tuple_size_v<decltype(simulation::BendSection::moving)> +
    std::tuple_size_v<decltype(simulation::BendSection::innerArc)> +
    std::tuple_size_v<decltype(simulation::BendSection::outerArc)>;
using IntrinsicSectionKey = std::array<double, 2 * intrinsicSectionPoints + 3 + 12>;
struct BendWorkpieceDisplay {
    std::array<std::vector<SheetSide>, 2> flangeSides;
    SheetSide innerSide;
    std::optional<std::size_t> referenceFlange;
    std::vector<SheetSide> displaySides;
    mutable std::shared_ptr<const WholePartSkin> skin;
};
// Worker-only immutable geometry. No tool, contact, support, machine or sweep verdicts.
struct PreparedBendWorkpiece {
    application::DocumentPtr document;
    std::size_t bendIndex;
    core::BendTarget target;
    bool requestedReversed;
    std::optional<gp_Dir> referenceAxis;
    std::vector<std::size_t> jointIds;
    TopoDS_Shape source, sourceSector, displaySource;
    simulation::SingleBend model;
    FrameBasis basis, sourceBasis;
    simulation::BendSection sourceSection;
    std::vector<application::RecognizedBend> jointZones;
    std::vector<TopoDS_Shape> jointSectors;
    std::vector<std::pair<double, double>> supportIntervals;
    bool nonRadialEnds;
    std::array<TopoDS_Shape, 2> flanges;
    double expectedVolume, budget, collisionBudget, rigidVolume;
    mutable std::shared_ptr<const BendWorkpieceDisplay> display;
    // Successful intrinsic checks only, exact complete section+basis, at most 128.
    // Access is serialized by the enclosing route worker's CacheData mutex.
    mutable std::vector<IntrinsicSectionKey> checkedSections;
};
} // namespace bending::cad
