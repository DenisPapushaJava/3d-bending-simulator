#pragma once
#include "bending/cad/occt/PreviewAccess.h"
#include <algorithm>
namespace bending::cad {
using namespace application;
class CadPreview : public SingleBendPreview {
  public:
    bool draft = false;
    bool reversed = false;
    bool workpieceReversed() const noexcept override {
        return reversed;
    }
    bool floorChecked() const noexcept override {
        return machineModel() == core::MachineModel::DemoPressV3 && !frames.empty() &&
               std::all_of(frames.begin(), frames.end(),
                           [](const auto &f) { return f.floor.has_value(); });
    }
    bool seatedMount = false;
    bool seatedPunchMount() const noexcept override {
        return seatedMount;
    }
    std::vector<std::optional<MachineCollision>> machineConflicts;
    core::MachineModel machineModel() const noexcept override {
        return setup ? setup->tools.machineModel : core::MachineModel::None;
    }
    std::optional<MachineCollision> machineCollisionAtFrame(std::size_t i) const override {
        return i < machineConflicts.size() ? machineConflicts[i] : std::nullopt;
    }
    bool unverifiedDraft() const noexcept override {
        return draft;
    }
    bool filledCutouts = false;
    bool realPunch = false, realDie = false;
    std::array<double, 2> gaps{};
    std::optional<BendEndSupport> endSupport;
    const BendEndSupport *partialEndSupport() const noexcept override {
        return endSupport ? &*endSupport : nullptr;
    }
    std::size_t otherBends = 0;
    std::size_t otherFormedBends() const noexcept override {
        return otherBends;
    }
    std::array<double, 2> toolSegmentGaps() const noexcept override {
        return gaps;
    }
    std::optional<ToolCollisionPair> conflict;
    std::vector<std::optional<ToolCollisionPair>> frameConflicts;
    std::optional<ToolCollisionPair> collisionAtFrame(std::size_t i) const noexcept override {
        return i < frameConflicts.size() ? frameConflicts[i] : conflict;
    }
    bool usesDrawingTools() const noexcept override {
        return realPunch || realDie;
    }
    bool usesDrawingPunch() const noexcept override {
        return realPunch;
    }
    bool usesDrawingDie() const noexcept override {
        return realDie;
    }
    std::optional<ToolCollisionPair> collision() const noexcept override {
        return conflict;
    }
    bool ignoresClosedCutouts() const noexcept override {
        return filledCutouts;
    }
    simulation::SingleBend geometry{};
    BoundsMm envelope{};
    std::vector<PreviewFrame> frames;
    std::array<TopoDS_Shape, 2> components;
    std::optional<simulation::FlatToolSetup> setup;
    std::optional<simulation::PressMotion> motion;
    std::vector<simulation::PressState> states;
    std::optional<PunchRetractionCheck> retraction;
    TopoDS_Shape sweep;
    const PunchRetractionCheck *punchRetraction() const noexcept override {
        return retraction ? &*retraction : nullptr;
    }
    const simulation::PressMotion *pressMotion() const noexcept override {
        return motion ? &*motion : nullptr;
    }
    const simulation::PressState *pressState(std::size_t i) const noexcept override {
        return i < states.size() ? &states[i] : nullptr;
    }
    const simulation::FlatToolSetup *toolSetup() const noexcept override {
        return setup ? &*setup : nullptr;
    }
    const simulation::SingleBend &model() const noexcept override {
        return geometry;
    }
    std::size_t frameCount() const noexcept override {
        return frames.size();
    }
    BoundsMm bounds() const noexcept override {
        return envelope;
    }
};
} // namespace bending::cad
