#include "RouteDraftGeometry.h"
#include "BendOperation.h"
#include "CadPreview.h"
#include "CollisionHighlight.h"
#include "RouteGeometry.h"
#include "SheetOrientation.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/MachineAssembly.h"
#include "bending/core/ToolGeometry.h"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
namespace bending::cad {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw application::BendRouteError{application::BendRouteErrorCode::Cancelled, "Cancelled"};
}
void require(bool ok, const char *detail) {
    if (!ok)
        throw application::BendRouteError{application::BendRouteErrorCode::Failed, detail};
}
struct Node {
    gp_Pnt point;
    std::uint32_t firstRegion, regionCount;
};
struct Face {
    Handle(Poly_Triangulation) mesh;
    TopAbs_Orientation orientation;
    std::vector<Node> nodes;
    std::size_t region;
    SheetSide side = SheetSide::Neutral;
};
TopoDS_Compound compound() {
    TopoDS_Compound c;
    BRep_Builder().MakeCompound(c);
    return c;
}
Bnd_Box box(const TopoDS_Shape &s) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, false, false);
    return b;
}
// Conservative broad phase followed by exact occupied-volume intersection.
double intersectionVolume(const TopoDS_Shape &a, const TopoDS_Shape &b, double tolerance,
                          std::stop_token stop, TopoDS_Shape *overlap = nullptr) {
    cancelled(stop);
    auto ba = box(a), bb = box(b);
    ba.Enlarge(tolerance);
    bb.Enlarge(tolerance);
    if (ba.IsOut(bb))
        return 0;
    BRepAlgoAPI_Common common;
    TopTools_ListOfShape first, second;
    first.Append(a);
    second.Append(b);
    common.SetArguments(first);
    common.SetTools(second);
    common.SetNonDestructive(true);
    common.SetRunParallel(false);
    common.SetFuzzyValue(tolerance);
    common.Build();
    cancelled(stop);
    require(common.IsDone() && !common.HasErrors() && BRepCheck_Analyzer(common.Shape()).IsValid(),
            "Draft collision intersection failed; position is not verified");
    GProp_GProps p;
    BRepGProp::VolumeProperties(common.Shape(), p);
    require(std::isfinite(p.Mass()), "Invalid draft collision volume");
    if (overlap && std::abs(p.Mass()) > 0)
        *overlap = common.Shape();
    return std::abs(p.Mass());
}
} // namespace
struct RouteDraftBasis::Impl {
    application::DocumentPtr source;
    application::FlatPatternPtr flat;
    std::vector<Face> faces;
    std::vector<std::uint32_t> nodeRegions;
    std::vector<bool> referenceReversed;
    std::optional<std::size_t> referenceWall;
};
RouteDraftBasis::~RouteDraftBasis() = default;
RouteDraftBasis::RouteDraftBasis(application::DocumentPtr source, application::FlatPatternPtr flat,
                                 std::stop_token stop)
    : impl_(std::make_unique<Impl>()) {
    impl_->source = std::move(source);
    impl_->flat = std::move(flat);
    const auto &layout = OcctFlatPatternAccess::layout(*impl_->flat);
    const auto &native = dynamic_cast<const CadDocument &>(*impl_->source);
    const double tolerance = native.metadata().bendAnalysis.lengthToleranceMm;
    impl_->referenceReversed.resize(native.metadata().bendAnalysis.bends.size());
    for (const auto &b : layout.bends)
        impl_->referenceReversed[b.bendIndex] =
            normalBendAxis(native, b.bendIndex).Dot(b.sectionAxes.Direction()) < 0;
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> classifiers;
    std::vector<Bnd_Box> boxes;
    const auto add = [&](const TopoDS_Shape &s) {
        classifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(s));
        auto b = box(s);
        b.Enlarge(tolerance * 10);
        boxes.push_back(b);
    };
    for (const auto &w : layout.walls)
        add(w.solid);
    for (const auto &b : layout.bends)
        add(b.solid);
    require(classifiers.size() <= std::numeric_limits<std::uint32_t>::max(),
            "Too many draft regions");
    const auto &orientation = OcctDocumentAccess::sheetOrientation(native);
    std::vector<TopoDS_Shape> wallShapes;
    for (const auto &wall : layout.walls)
        wallShapes.push_back(wall.solid);
    if (orientation.available)
        impl_->referenceWall = sheetReferenceRegion(orientation.reference, wallShapes, tolerance);
    BRepBuilderAPI_Copy copier(OcctDocumentAccess::shape(native), true, false);
    auto copy = copier.Shape();
    TopTools_IndexedMapOfShape sourceFaces, copiedFaces;
    TopExp::MapShapes(OcctDocumentAccess::shape(native), TopAbs_FACE, sourceFaces);
    TopExp::MapShapes(copy, TopAbs_FACE, copiedFaces);
    std::vector<SheetSide> sideLabels(static_cast<std::size_t>(copiedFaces.Extent()) + 1,
                                      SheetSide::Neutral);
    if (orientation.available)
        for (int i = 1; i <= sourceFaces.Extent(); ++i) {
            const auto index = copiedFaces.FindIndex(copier.ModifiedShape(sourceFaces(i)));
            if (index)
                sideLabels[index] = orientation.faces[i];
        }
    BRepMesh_IncrementalMesh mesher(copy, .03, false, .12, false);
    require(mesher.IsDone(), "Cannot mesh the original draft surface");
    for (TopExp_Explorer f(copy, TopAbs_FACE); f.More(); f.Next()) {
        cancelled(stop);
        const auto face = TopoDS::Face(f.Current());
        TopLoc_Location loc;
        const auto mesh = BRep_Tool::Triangulation(face, loc);
        require(!mesh.IsNull(), "Missing draft mesh");
        Face skin{mesh, face.Orientation(), {}, 0};
        skin.side = sideLabels[copiedFaces.FindIndex(face)];
        std::vector<std::size_t> votes(classifiers.size());
        for (int i = 1; i <= mesh->NbNodes(); ++i) {
            if ((i & 255) == 0)
                cancelled(stop);
            const auto p = mesh->Node(i).Transformed(loc.Transformation());
            const auto first = impl_->nodeRegions.size();
            for (std::size_t r = 0; r < classifiers.size(); ++r) {
                if (boxes[r].IsOut(p))
                    continue;
                classifiers[r]->Perform(p, tolerance * 10);
                if (classifiers[r]->State() == TopAbs_IN || classifiers[r]->State() == TopAbs_ON) {
                    impl_->nodeRegions.push_back(static_cast<std::uint32_t>(r));
                    ++votes[r];
                }
            }
            const auto count = impl_->nodeRegions.size() - first;
            require(count != 0, "Original surface is outside the draft decomposition");
            require(impl_->nodeRegions.size() <= std::numeric_limits<std::uint32_t>::max(),
                    "Draft membership storage limit reached");
            skin.nodes.push_back(
                {p, static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count)});
        }
        skin.region =
            static_cast<std::size_t>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        impl_->faces.push_back(std::move(skin));
    }
}
application::RouteDraftPtr RouteDraftBasis::build(application::RouteDraftRequest request,
                                                  bool reused, std::stop_token stop) const {
    using namespace application;
    cancelled(stop);
    const auto &layout = OcctFlatPatternAccess::layout(*impl_->flat);
    const auto &analysis = impl_->source->metadata().bendAnalysis;
    const auto n = request.operations.size();
    require(n && request.step <= n, "Invalid draft prefix");
    const auto &active = request.operations[request.step ? request.step - 1 : 0];
    const auto &bend = analysis.bends[active.bendIndex];
    const auto link = std::find_if(layout.bends.begin(), layout.bends.end(),
                                   [&](const auto &b) { return b.bendIndex == active.bendIndex; });
    require(link != layout.bends.end(), "Draft bend has no layout link");
    auto visual = std::make_shared<CadPreview>();
    visual->draft = true;
    visual->reversed = active.workpieceReversed;
    visual->otherBends = request.step ? request.step - 1 : 0;
    if (active.assignment)
        visual->gaps = {active.assignment->dieAssembly ? active.assignment->dieAssembly->gapMm : 0,
                        active.assignment->punchAssembly ? active.assignment->punchAssembly->gapMm
                                                         : 0};
    visual->geometry = {
        bend.thicknessMm, bend.innerRadiusMm, bend.angleRadians, bend.lengthMm, 1, 1};
    visual->filledCutouts =
        impl_->source->metadata().closedCutouts.status == ClosedCutoutStatus::Filled;
    std::vector<double> progress(n);
    for (std::size_t i = 0; i + 1 < request.step; ++i)
        progress[request.operations[i].bendIndex] = 1;
    auto result = std::make_shared<RouteDraft>();
    result->source = impl_->source;
    result->request = request;
    result->reusedBasis = reused;
    std::array<TopoDS_Shape, 2> tools;
    std::array<std::vector<ToolDisplaySegment>, 2> segments;
    std::optional<simulation::DieShoulders> shoulders;
    std::optional<MachineAssembly> machineAssembly;
    const bool withTools =
        request.withTools && active.tools && core::validPressTools(*active.tools);
    if (request.withTools && !withTools)
        result->toolNotice = "Инструмент этого гиба не задан; столкновения не проверены.";
    if (withTools) {
        const BendRouteToolLengthIssue lengths{bend.lengthMm, core::punchLengthMm(*active.tools),
                                               active.tools->lengthMm};
        if (!lengths.coversBend(analysis.lengthToleranceMm)) {
            result->toolLengths = lengths;
            result->toolNotice = "Инструмент короче линии гиба; полная длина не обеспечена.";
        }
        require(!active.assignment || core::toolParameters(*active.assignment) == active.tools,
                "Inconsistent draft tool assignment");
        tools = makeRouteDraftTools(*active.tools, active.assignment, stop, segments);
        if (active.assignment && active.assignment->useDrawingGeometry) {
            const auto geometry = core::drawingToolGeometry(*active.assignment);
            require(geometry.has_value(), "Unsupported draft drawing references");
            visual->realPunch = geometry->hasPunch;
            visual->realDie = geometry->hasDie;
            if (geometry->hasDie)
                shoulders = simulation::DieShoulders{
                    geometry->shoulderHalfSpanMm, geometry->shoulderRadiusMm,
                    geometry->leftSupportWidthMm, geometry->rightSupportWidthMm};
        }
        visual->setup = simulation::FlatToolSetup{*active.tools, 0, 0, 0, 0, 0};
        if (active.tools->machineModel != core::MachineModel::None)
            machineAssembly.emplace(*active.tools, tools, stop, active.assignment);
        visual->seatedMount = machineAssembly && machineAssembly->seatedPunchMount();
    }
    Bnd_Box envelope;
    const std::size_t count = request.step ? 21 : 1;
    for (std::size_t f = 0; f < count; ++f) {
        cancelled(stop);
        const double p = count == 1 ? 0 : static_cast<double>(f) / (count - 1);
        progress[active.bendIndex] = p;
        const auto placement = routePlacement(layout, progress);
        gp_Trsf machine;
        PreviewFrame frame;
        if (withTools) {
            const auto state =
                simulation::draftPressState(visual->geometry, *active.tools, p, shoulders);
            require(state.has_value(), "Cannot position the draft sheet on tools");
            visual->states.push_back(*state);
            const auto axes = link->sectionAxes.Transformed(placement.walls[link->parent]);
            const auto &section = state->section;
            const auto a = section.fixed[0], b = section.fixed[1];
            machine.SetDisplacement(
                axes, gp_Ax3(gp_Pnt(a.x, a.y, (active.tools->lengthMm - bend.lengthMm) / 2),
                             gp_Dir(0, 0, 1), gp_Dir(a.x - b.x, a.y - b.y, 0)));
            if (active.workpieceReversed != impl_->referenceReversed[active.bendIndex]) {
                gp_Trsf turn;
                turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, active.tools->lengthMm / 2), gp_Dir(0, 1, 0)),
                                 std::numbers::pi);
                machine = turn * machine;
            }
            frame.tools = tools;
            frame.toolSegments = segments;
            gp_Trsf lift;
            lift.SetTranslation(gp_Vec(0, state->punchTipYmm - active.tools->clearanceMm, 0));
            frame.tools[1] = tools[1].Moved(TopLoc_Location(lift));
            for (auto &s : frame.toolSegments[1])
                s.shape = s.shape.Moved(TopLoc_Location(lift));
            if (machineAssembly) {
                frame.machine = machineAssembly->at(state->punchTipYmm - active.tools->clearanceMm);
                frame.floor = machineAssembly->floor();
            }
        }
        std::vector<TopoDS_Shape> material;
        for (std::size_t w = 0; w < layout.walls.size(); ++w)
            material.push_back(
                layout.walls[w].solid.Moved(TopLoc_Location(machine * placement.walls[w])));
        for (const auto &b : layout.bends) {
            const double v = progress[b.bendIndex];
            TopoDS_Shape solid;
            gp_Trsf map;
            if (v == 0) {
                solid = b.flatSolid;
                map = placement.walls[b.parent] * layout.walls[b.parent].toFlat.Inverted();
            } else {
                solid = v == 1 ? b.solid : b.solidAtProgress(v);
                map = placement.walls[b.parent];
            }
            material.push_back(solid.Moved(TopLoc_Location(machine * map)));
        }
        auto sheet = compound();
        BRep_Builder builder;
        for (const auto &s : material)
            builder.Add(sheet, s);
        frame.shape = sheet;
        std::optional<ToolCollisionPair> collision;
        std::vector<CollisionEvidence> evidence;
        if (withTools) {
            const double budget =
                std::max(1.e-6, bend.lengthMm * bend.thicknessMm * analysis.lengthToleranceMm * 2);
            for (std::size_t t = 0; t < 2; ++t) {
                double overlap = 0;
                auto occupied = compound();
                for (const auto &s : material) {
                    TopoDS_Shape common;
                    overlap += intersectionVolume(s, frame.tools[t], analysis.lengthToleranceMm,
                                                  stop, &common);
                    if (!common.IsNull())
                        builder.Add(occupied, common);
                }
                if (overlap > budget) {
                    if (!collision)
                        collision =
                            t == 0 ? ToolCollisionPair::SheetDie : ToolCollisionPair::SheetPunch;
                    evidence.push_back({"sheet", t == 0 ? "die" : "punch", occupied});
                }
            }
            TopoDS_Shape toolOverlap;
            if (intersectionVolume(frame.tools[0], frame.tools[1], analysis.lengthToleranceMm, stop,
                                   &toolOverlap) > budget) {
                collision = ToolCollisionPair::DiePunch;
                evidence.push_back({"die", "punch", toolOverlap});
            }
            ++result->checkedToolPositions;
        }
        visual->frameConflicts.push_back(collision);
        const auto machineHits =
            MachineAssembly::collisions(frame, analysis.lengthToleranceMm, stop);
        const auto machineCollision = machineHits.empty() ? std::optional<MachineCollision>{}
                                                          : std::optional{machineHits.front().pair};
        visual->machineConflicts.push_back(machineCollision);
        for (const auto &hit : machineHits)
            evidence.push_back({hit.pair.firstId, hit.pair.secondId, hit.overlap});
        std::vector<std::function<gp_Pnt(gp_Pnt)>> maps;
        for (const auto &m : placement.walls)
            maps.push_back([m](gp_Pnt pnt) { return pnt.Transformed(m); });
        for (const auto &b : layout.bends)
            maps.push_back(placement.bends[b.bendIndex]);
        std::array<TopoDS_Compound, 3> regions{compound(), compound(), compound()};
        for (std::size_t i = 0; i < n; ++i)
            frame.bendZones.push_back(compound());
        for (const auto &face : impl_->faces) {
            cancelled(stop);
            Handle(Poly_Triangulation) mesh =
                new Poly_Triangulation(face.mesh->NbNodes(), face.mesh->NbTriangles(), false);
            for (std::size_t i = 0; i < face.nodes.size(); ++i) {
                const auto &node = face.nodes[i];
                std::optional<gp_Pnt> mapped;
                for (const auto r :
                     std::span(impl_->nodeRegions).subspan(node.firstRegion, node.regionCount)) {
                    const auto next = maps[r](node.point).Transformed(machine);
                    require(!mapped || mapped->Distance(next) <= .02,
                            "Inconsistent draft boundary mapping");
                    mapped = next;
                }
                mesh->SetNode(static_cast<int>(i + 1), *mapped);
                envelope.Add(*mapped);
            }
            for (int i = 1; i <= face.mesh->NbTriangles(); ++i)
                mesh->SetTriangle(i, face.mesh->Triangle(i));
            mesh->Deflection(.03);
            TopoDS_Face skin;
            builder.MakeFace(skin, mesh);
            skin.Orientation(face.orientation);
            const bool selected =
                request.step &&
                face.region ==
                    layout.walls.size() + static_cast<std::size_t>(link - layout.bends.begin());
            builder.Add(regions[selected ? 1 : 0], skin);
            if (face.side != SheetSide::Neutral)
                frame.orientation.sides[face.side == SheetSide::A ? 0 : 1].push_back(skin);
            if (face.region >= layout.walls.size())
                builder.Add(
                    frame.bendZones[layout.bends[face.region - layout.walls.size()].bendIndex],
                    skin);
        }
        auto display = compound();
        for (std::size_t i = 0; i < 3; ++i) {
            frame.displayRegions[i] = regions[i];
            builder.Add(display, regions[i]);
        }
        for (const auto &t : frame.tools)
            if (!t.IsNull()) {
                builder.Add(display, t);
                envelope.Add(box(t));
            }
        frame.displayShape = display;
        if (impl_->referenceWall) {
            const auto &reference = OcctDocumentAccess::sheetOrientation(
                                        dynamic_cast<const CadDocument &>(*impl_->source))
                                        .reference;
            frame.orientation.edges =
                movedSheetReference(reference, machine * placement.walls[*impl_->referenceWall])
                    .edges;
        }
        buildCollisionHighlights(frame, evidence, bend.thicknessMm, stop);
        cancelled(stop);
        visual->frames.push_back(std::move(frame));
    }
    require(!envelope.IsVoid(), "Empty draft display");
    const auto lo = envelope.CornerMin(), hi = envelope.CornerMax();
    visual->envelope = {{lo.X(), lo.Y(), lo.Z()}, {hi.X(), hi.Y(), hi.Z()}};
    result->visual = visual;
    cancelled(stop);
    return result;
}
} // namespace bending::cad
