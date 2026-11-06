#include "ToolFixtures.h"
#include "bending/application/BendRouteAcceptance.h"
#include "bending/application/SingleBendPreview.h"

#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace bending;
using namespace bending::application;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
class TestDocument final : public Document {
  public:
    TestDocument() {
        data.bendAnalysis.status = BendRecognitionStatus::Completed;
        for (int i = 0; i < 3; ++i) {
            const double x = 100. * i;
            data.bendAnalysis.bends.push_back({{x, 0, 0},
                                               {x, 0, 80},
                                               std::numbers::pi / 2,
                                               4,
                                               2,
                                               80,
                                               1,
                                               2,
                                               {3, 4, 5, 6},
                                               false,
                                               80});
        }
    }
    const DocumentMetadata &metadata() const noexcept override {
        return data;
    }
    const std::vector<FaceInfo> &faces() const noexcept override {
        return faces_;
    }
    DocumentMetadata data{};
    std::vector<FaceInfo> faces_;
};
// Numerical published-result fixture: no Qt/OCCT, geometry work or worker threads.
class TestPreview final : public SingleBendPreview {
  public:
    explicit TestPreview(std::size_t formed) : formed_(formed) {}
    const simulation::SingleBend &model() const noexcept override {
        return model_;
    }
    std::size_t frameCount() const noexcept override {
        return 3;
    }
    BoundsMm bounds() const noexcept override {
        return {};
    }
    std::size_t activeBendCount() const noexcept override {
        return active;
    }
    std::size_t otherFormedBends() const noexcept override {
        return formed_;
    }
    bool workpieceReversed() const noexcept override {
        return reversed;
    }
    bool unverifiedDraft() const noexcept override {
        return draft;
    }
    bool usesDrawingTools() const noexcept override {
        return drawing;
    }
    std::optional<ToolCollisionPair> collision() const noexcept override {
        return conflict;
    }
    core::MachineModel machineModel() const noexcept override {
        return setup.tools.machineModel;
    }
    bool floorChecked() const noexcept override {
        return floor;
    }
    std::optional<MachineCollision> machineCollisionAtFrame(std::size_t frame) const override {
        return machineConflict && frame == 1 ? std::optional{MachineCollision{}} : std::nullopt;
    }
    const simulation::FlatToolSetup *toolSetup() const noexcept override {
        return tools ? &setup : nullptr;
    }
    const simulation::PressMotion *pressMotion() const noexcept override {
        return tools ? &motion : nullptr;
    }
    const simulation::PressState *pressState(std::size_t frame) const noexcept override {
        return tools ? (frame == 0 ? &approach : &returned) : nullptr;
    }
    const PunchRetractionCheck *punchRetraction() const noexcept override {
        return tools ? &retraction : nullptr;
    }
    const BackgaugeReport *backgauge() const noexcept override {
        return &gauge;
    }
    std::span<const BendSupportInterval> endSupportIntervals() const noexcept override {
        return tools ? std::span<const BendSupportInterval>(support)
                     : std::span<const BendSupportInterval>{};
    }
    std::optional<double> endSupportLimitMm() const noexcept override {
        return supportLimit;
    }
    bool tools = false, drawing = false, draft = false, reversed = false;
    bool machineConflict = false, floor = false;
    std::size_t active = 1;
    std::optional<ToolCollisionPair> conflict;
    std::optional<double> supportLimit = 0;
    std::vector<BendSupportInterval> support{{0, {80, 0, 80}}};
    simulation::FlatToolSetup setup{};
    simulation::PressMotion motion{};
    simulation::PressState approach{{}, simulation::PressPhase::Approach, 0, 0, 0};
    simulation::PressState returned{{}, simulation::PressPhase::Retracting, 0, 0, 0};
    PunchRetractionCheck retraction{10, 5};
    BackgaugeReport gauge;

  private:
    std::size_t formed_;
    simulation::SingleBend model_{2, 4, std::numbers::pi / 2, 80, 30, 30};
};
struct Fixture {
    DocumentSession session;
    BendRouteRequest request;
    std::shared_ptr<BendRoute> route = std::make_shared<BendRoute>();
    std::vector<std::shared_ptr<TestPreview>> previews;
    explicit Fixture(bool tools = false) {
        auto document = std::make_shared<TestDocument>();
        require(session.complete(session.beginImport("part.step"), DocumentPtr(document)),
                "Import failed");
        route->source = document;
        route->withTools = request.withTools = tools;
        for (const auto index : session.orderedBends()) {
            const auto *entry = session.bend(index);
            request.operations.push_back({entry->id, index, *session.bendTarget(index), {}, {}});
            if (tools)
                request.operations.back().tools =
                    core::PressTools{16, std::numbers::pi / 3, std::numbers::pi / 3, .8, 80, 3};
        }
        require(prepareBendRouteRequest(session, request), "Request rejected");
        for (std::size_t formed = 0; formed < 3; ++formed) {
            const auto index = 2 - formed;
            auto p = std::make_shared<TestPreview>(formed);
            p->tools = tools;
            if (tools)
                p->setup.tools = *request.operations[index].tools;
            previews.push_back(p);
            route->steps.push_back({request.operations[index].id, index, false, p});
        }
    }
    void reject(const char *message) {
        const auto before = session;
        require(!applyBendRouteResult(session, request, route), message);
        require(session.document() == before.document() &&
                    session.orderedBends() == before.orderedBends() &&
                    session.editRevision() == before.editRevision() &&
                    session.modified() == before.modified() &&
                    session.revision() == before.revision() &&
                    session.manualThicknessMm() == before.manualThicknessMm(),
                "Rejected route changed session");
        for (const auto index : before.orderedBends()) {
            const auto &a = *session.bend(index), &b = *before.bend(index);
            require(a.id == b.id && a.review == b.review && a.target == b.target &&
                        a.tools == b.tools && a.catalogTools == b.catalogTools &&
                        a.workpieceReversed == b.workpieceReversed &&
                        a.workpieceOffsetMm == b.workpieceOffsetMm,
                    "Rejected route changed bend input");
        }
    }
};
void successfulRoutes() {
    for (bool tools : {false, true}) {
        Fixture f(tools);
        require(applyBendRouteResult(f.session, f.request, f.route), "Valid route rejected");
        require(f.session.orderedBends() == std::vector<std::size_t>{2, 1, 0} &&
                    f.session.modified(),
                "Route order not applied");
        for (const auto &op : f.request.operations)
            require(f.session.bend(op.bendIndex)->tools == op.tools, "Tool input not applied");
    }
    Fixture grouped;
    grouped.route->steps.pop_back();
    grouped.route->steps.back().additionalBends.push_back(grouped.request.operations[0]);
    grouped.previews[1]->active = 2;
    require(applyBendRouteResult(grouped.session, grouped.request, grouped.route),
            "Joint coverage rejected");
    require(grouped.session.orderedBends() == std::vector<std::size_t>{2, 1, 0}, "Joint IDs lost");

    Fixture selected(true);
    selected.request.automatic = true;
    const auto assignment = tool_fixture::pair(22);
    selected.request.toolVariants.push_back({assignment, {}});
    require(prepareBendRouteRequest(selected.session, selected.request),
            "Variant preflight failed");
    selected.route->selectedToolVariant = 0;
    for (auto &p : selected.previews) {
        p->setup.tools = *core::toolParameters(assignment);
        p->drawing = true;
    }
    require(applyBendRouteResult(selected.session, selected.request, selected.route),
            "Selected variant rejected");
    for (const auto &entry : selected.session.bends())
        require(entry.catalogTools == assignment && entry.tools == core::toolParameters(assignment),
                "Selected DXF input not committed");
}
void rejectedRoutes() {
    {
        Fixture f;
        f.route.reset();
        f.reject("Null accepted");
    }
    {
        Fixture f;
        f.route->source = std::make_shared<TestDocument>();
        f.reject("Foreign source accepted");
    }
    {
        Fixture f;
        f.route->steps.pop_back();
        f.reject("Incomplete route accepted");
    }
    {
        Fixture f;
        f.route->steps.back() = f.route->steps.front();
        f.reject("Repeated zone accepted");
    }
    {
        Fixture f;
        f.previews.back()->draft = true;
        f.reject("Draft accepted");
    }
    {
        Fixture f;
        f.previews.back()->conflict = ToolCollisionPair::SheetPunch;
        f.reject("Collision accepted");
    }
    {
        Fixture f;
        f.previews.back()->machineConflict = true;
        f.reject("Intermediate machine collision accepted");
    }
    {
        Fixture f;
        f.request.operations[0].target.innerRadiusMm += 1;
        f.reject("Stale target accepted");
    }
    {
        Fixture f;
        require(f.session.setManualThickness(2) != ThicknessValidation::Invalid,
                "Thickness fixture failed");
        f.reject("Stale thickness accepted");
    }
    {
        Fixture f;
        (void)f.session.beginImport("new.step");
        f.reject("Active import accepted");
    }
    {
        Fixture f;
        f.route->steps[0].additionalBends.push_back(f.request.operations[0]);
        f.previews[0]->active = 2;
        f.reject("Repeated joint member accepted");
    }
    {
        Fixture f(true);
        f.request.operations[0].tools->lengthMm = 60;
        f.previews.back()->setup.tools = *f.request.operations[0].tools;
        f.reject("Short die accepted");
    }
    {
        Fixture f(true);
        f.previews.back()->retraction.finalClearanceMm = 0;
        f.reject("Unproven return accepted");
    }
    {
        Fixture f(true);
        f.previews.back()->support[0].material.supportedFromMm = 1;
        f.reject("Unsupported end loss accepted");
    }
    {
        Fixture f(true);
        f.request.operations[0].tools->backgauge = core::BackgaugeSetup{};
        f.previews.back()->setup.tools = *f.request.operations[0].tools;
        f.reject("Unconfirmed backgauge accepted");
    }
    {
        Fixture f(true);
        f.request.operations[0].tools->machineModel = core::MachineModel::DemoPressV3;
        f.previews.back()->setup.tools = *f.request.operations[0].tools;
        f.reject("Missing floor check accepted");
    }
    {
        Fixture f(true);
        f.request.toolVariants.push_back({tool_fixture::pair(22), {}});
        f.route->selectedToolVariant = 2;
        f.reject("Unknown selected variant accepted");
    }
}
void requestBoundary() {
    Fixture f;
    const auto before = f.session.editRevision();
    require(prepareBendRouteRequest(f.session, f.request) && f.session.editRevision() == before,
            "Preflight edited project");
    f.request.operations.back() = f.request.operations.front();
    require(!prepareBendRouteRequest(f.session, f.request), "Duplicate input accepted");
    Fixture fixed(true);
    fixed.request.allowAxialPlacement = true;
    require(!prepareBendRouteRequest(fixed.session, fixed.request),
            "Manual axial search permission accepted");
}
} // namespace
int main() {
    try {
        successfulRoutes();
        rejectedRoutes();
        requestBoundary();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
