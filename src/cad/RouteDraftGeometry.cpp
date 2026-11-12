#include "RouteDraftGeometry.h"
#include "Backgauge.h"
#include "BendOperation.h"
#include "CadPreview.h"
#include "CollisionHighlight.h"
#include "CollisionReuse.h"
#include "FlatPatternDisplay.h"
#include "JointBends.h"
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
#include <Precision.hxx>
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
    TopoDS_Face source; // Retained while nodes can still require an exact owner.
    std::optional<std::size_t> owner;
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
Bnd_Box collisionBox(const TopoDS_Shape &shape, double tolerance) {
    auto bounds = box(shape);
    bounds.Enlarge(tolerance);
    return bounds;
}
// Conservative broad phase followed by exact occupied-volume intersection.
double intersectionVolume(const TopoDS_Shape &a, const TopoDS_Shape &b, const Bnd_Box &aBounds,
                          const Bnd_Box &bBounds, double tolerance, std::stop_token stop,
                          TopoDS_Shape *overlap) {
    cancelled(stop);
    if (aBounds.IsOut(bBounds))
        return 0;
    TopTools_ListOfShape first, second;
    first.Append(a);
    second.Append(b);
    const auto intersect = [&](double fuzzy) -> TopoDS_Shape {
        BRepAlgoAPI_Common common;
        common.SetArguments(first);
        common.SetTools(second);
        common.SetNonDestructive(true);
        common.SetFuzzyValue(fuzzy);
        common.Build();
        cancelled(stop);
        if (common.IsDone() && !common.HasErrors() && !common.Shape().IsNull() &&
            BRepCheck_Analyzer(common.Shape()).IsValid())
            return common.Shape();
        return {};
    };
    auto common = intersect(tolerance);
    // Near-tangent faces can merge at the extra fuzzy tolerance. Retry with
    // the native BRep tolerances before declaring the draft position unknown.
    if (common.IsNull() && tolerance > Precision::Confusion())
        common = intersect(0);
    require(!common.IsNull(), "Draft collision intersection failed; position is not verified");
    GProp_GProps p;
    BRepGProp::VolumeProperties(common, p);
    require(std::isfinite(p.Mass()), "Invalid draft collision volume");
    if (overlap && std::abs(p.Mass()) > 0)
        *overlap = common;
    return std::abs(p.Mass());
}
} // namespace
struct RouteDraftBasis::Impl {
    application::DocumentPtr source;
    application::FlatPatternPtr flat;
    std::vector<Face> faces;
    std::vector<std::uint32_t> nodeRegions;
    std::vector<TopoDS_Shape> regionSolids;
    std::vector<Bnd_Box> regionBoxes;
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
    auto &boxes = impl_->regionBoxes;
    const auto add = [&](const TopoDS_Shape &s) {
        classifiers.push_back(std::make_unique<BRepClass3d_SolidClassifier>(s));
        impl_->regionSolids.push_back(s);
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
        Face skin{mesh, face.Orientation(), {}, 0, sideLabels[copiedFaces.FindIndex(face)], {}, {}};
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
            if (count > 1 && skin.source.IsNull())
                skin.source = face;
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
application::RouteDraftPtr
RouteDraftBasis::build(application::RouteDraftRequest request, bool reused, std::stop_token stop,
                       const application::RouteDraftPoseObserver &onPose) const {
    using namespace application;
    cancelled(stop);
    const auto &layout = OcctFlatPatternAccess::layout(*impl_->flat);
    const auto &analysis = impl_->source->metadata().bendAnalysis;
    const auto n = request.operations.size();
    const auto strokes = routeDraftStrokes(request, analysis);
    require(!strokes.empty() && request.step <= strokes.size(), "Invalid draft prefix");
    const auto &stroke = strokes[request.step ? request.step - 1 : 0];
    const auto &active = request.operations[stroke.front()];
    BendStroke zones;
    for (const auto position : stroke)
        zones.push_back(request.operations[position].bendIndex);
    const auto joint = zones.size() > 1 ? jointBendEnvelope(analysis, zones) : std::nullopt;
    require(zones.size() == 1 || joint.has_value(), "Invalid shared draft stroke");
    const auto &bend = joint ? *joint : analysis.bends[active.bendIndex];
    const auto link = std::find_if(layout.bends.begin(), layout.bends.end(),
                                   [&](const auto &b) { return b.bendIndex == active.bendIndex; });
    require(link != layout.bends.end(), "Draft bend has no layout link");
    const bool flipped = active.workpieceReversed != impl_->referenceReversed[active.bendIndex];
    auto sectionAxes = link->sectionAxes;
    std::vector<std::array<double, 2>> intervals{{0, bend.lengthMm}};
    if (joint) {
        intervals.clear();
        const gp_Vec axis(sectionAxes.Direction());
        const auto projection = [&](PointMm p) {
            return gp_Vec(sectionAxes.Location(), gp_Pnt(p.x, p.y, p.z)).Dot(axis);
        };
        double first = std::numeric_limits<double>::infinity();
        for (const auto index : zones) {
            const auto &zone = analysis.bends[index];
            const auto a = projection(zone.axisStart), b = projection(zone.axisEnd);
            intervals.push_back({std::min(a, b), std::max(a, b)});
            first = std::min(first, intervals.back()[0]);
        }
        sectionAxes.SetLocation(sectionAxes.Location().Translated(axis * first));
        for (auto &interval : intervals) {
            interval[0] -= first;
            interval[1] -= first;
            if (flipped)
                interval = {bend.lengthMm - interval[1], bend.lengthMm - interval[0]};
        }
    }
    auto visual = std::make_shared<CadPreview>();
    visual->draft = true;
    visual->reversed = active.workpieceReversed;
    visual->activeBends = zones.size();
    if (active.assignment)
        visual->gaps = {active.assignment->dieAssembly
                            ? core::maximumAssemblyGap(*active.assignment->dieAssembly)
                            : 0,
                        active.assignment->punchAssembly
                            ? core::maximumAssemblyGap(*active.assignment->punchAssembly)
                            : 0};
    visual->geometry = {
        bend.thicknessMm, bend.innerRadiusMm, bend.angleRadians, bend.lengthMm, 1, 1};
    visual->filledCutouts =
        impl_->source->metadata().closedCutouts.status == ClosedCutoutStatus::Filled;
    std::vector<double> progress(n);
    for (std::size_t i = 0; i + 1 < request.step; ++i)
        for (const auto position : strokes[i]) {
            progress[request.operations[position].bendIndex] = 1;
            ++visual->otherBends;
        }
    auto result = std::make_shared<RouteDraft>();
    result->source = impl_->source;
    result->request = request;
    result->reusedBasis = reused;
    if (request.step > 1 && !joint && trimmedAxialCore(bend)) {
        const gp_Pnt start(bend.axisStart.x, bend.axisStart.y, bend.axisStart.z);
        const gp_Pnt end(bend.axisEnd.x, bend.axisEnd.y, bend.axisEnd.z);
        const gp_Dir axis(gp_Vec(start, end));
        const double middle = bend.lengthMm / 2;
        std::optional<double> left, right;
        for (const auto &other : layout.bends) {
            cancelled(stop);
            if (progress[other.bendIndex] != 1 ||
                axis.IsParallel(other.sectionAxes.Direction(), analysis.angleToleranceRadians))
                continue;
            if (const auto wall = adjacentSheetWall(*link, other))
                if (const auto span = projectedWallSpan(layout.walls[*wall].solid, start, axis)) {
                    if ((*span)[1] < middle)
                        left = std::max(left.value_or((*span)[1]), (*span)[1]);
                    else if ((*span)[0] > middle)
                        right = std::min(right.value_or((*span)[0]), (*span)[0]);
                }
        }
        if (left && right)
            result->centredPunchOpeningMm = 2 * std::min(middle - *left, *right - middle);
    }
    std::array<TopoDS_Shape, 2> tools;
    std::array<std::vector<ToolDisplaySegment>, 2> segments;
    std::optional<simulation::DieShoulders> shoulders;
    std::optional<MachineAssembly> machineAssembly;
    const bool withTools =
        request.withTools && active.tools && core::validPressTools(*active.tools);
    visual->axialOffset = withTools ? active.workpieceOffsetMm : 0;
    if (request.withTools && !withTools)
        result->toolNotice = "Инструмент этого гиба не задан; столкновения не проверены.";
    if (withTools) {
        const BendRouteToolLengthIssue lengths{bend.lengthMm, core::punchLengthMm(*active.tools),
                                               active.tools->lengthMm};
        const double tolerance = analysis.lengthToleranceMm;
        require(!active.assignment || core::toolParameters(*active.assignment) == active.tools,
                "Inconsistent draft tool assignment");
        const auto axialCoverage = compactDraftCoverage(request, analysis);
        if (!lengths.coversBend(tolerance)) {
            if (axialCoverage)
                result->toolNotice = "Оснастка перекрывает прямой участок гиба. Опора у торцов "
                                     "и отвод оцениваются при полной проверке.";
            else {
                result->toolLengths = lengths;
                result->toolNotice = "Инструмент короче линии гиба; рабочая длина не обеспечена.";
            }
        } else if (!toolCoversAxialPlacement(*active.tools, bend.lengthMm, active.workpieceOffsetMm,
                                             tolerance))
            throw BendRouteError{BendRouteErrorCode::InvalidInput,
                                 "Положение детали выходит за пределы инструмента",
                                 active.bendIndex, 0, BendPreviewErrorCode::ToolTooShort};
        if (active.assignment) {
            const double start =
                (active.tools->lengthMm - bend.lengthMm) / 2 + active.workpieceOffsetMm;
            const auto countMissing = [&](const core::ToolPreset &tool,
                                          const std::optional<core::ToolAssembly> &assembly,
                                          double origin) {
                if (!assembly || !tool.segments)
                    return 0.;
                double uncovered = 0;
                for (const auto &interval : intervals) {
                    double from = start - origin +
                                  (axialCoverage ? axialCoverage->supportedFromMm : interval[0]);
                    double to = start - origin +
                                (axialCoverage ? axialCoverage->supportedToMm : interval[1]);
                    if (tool.kind == core::ToolKind::Punch && active.tools->punchReversed) {
                        const double extent = core::punchLengthMm(*active.tools);
                        const double oldFrom = from;
                        from = extent - to;
                        to = extent - oldFrom;
                    }
                    uncovered +=
                        core::assemblyUncoveredLength(*tool.segments, *assembly, from, to).value();
                }
                return uncovered;
            };
            visual->uncoveredToolLength = {
                countMissing(active.assignment->die, active.assignment->dieAssembly, 0),
                countMissing(active.assignment->punch, active.assignment->punchAssembly,
                             (active.tools->lengthMm - core::punchLengthMm(*active.tools)) / 2)};
        }
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
    const double tolerance = analysis.lengthToleranceMm;
    const auto dieBounds = withTools ? collisionBox(tools[0], tolerance) : Bnd_Box{};
    std::vector<Bnd_Box> materialBounds;
    if (withTools)
        materialBounds.reserve(layout.walls.size() + layout.bends.size());
    RigidClearanceCache rigidClearances;
    std::size_t ownerQueries = 0;
    const std::size_t count = request.step ? 21 : 1;
    for (std::size_t f = 0; f < count; ++f) {
        cancelled(stop);
        const double p = count == 1 ? 0 : static_cast<double>(f) / (count - 1);
        for (const auto index : zones)
            progress[index] = p;
        const auto placement = routePlacement(layout, progress);
        gp_Trsf machine;
        PreviewFrame frame;
        if (withTools) {
            const auto state =
                simulation::draftPressState(visual->geometry, *active.tools, p, shoulders);
            require(state.has_value(), "Cannot position the draft sheet on tools");
            visual->states.push_back(*state);
            const auto axes = sectionAxes.Transformed(placement.walls[link->parent]);
            const auto &section = state->section;
            const auto a = section.fixed[0], b = section.fixed[1];
            machine.SetDisplacement(
                axes, gp_Ax3(gp_Pnt(a.x, a.y, (active.tools->lengthMm - bend.lengthMm) / 2),
                             gp_Dir(0, 0, 1), gp_Dir(a.x - b.x, a.y - b.y, 0)));
            if (flipped) {
                gp_Trsf turn;
                turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, active.tools->lengthMm / 2), gp_Dir(0, 1, 0)),
                                 std::numbers::pi);
                machine = turn * machine;
            }
            gp_Trsf axial;
            axial.SetTranslation(gp_Vec(0, 0, active.workpieceOffsetMm));
            machine = axial * machine;
            frame.tools = tools;
            frame.toolSegments = segments;
            gp_Trsf lift;
            lift.SetTranslation(gp_Vec(0, state->punchTipYmm - active.tools->clearanceMm, 0));
            const TopLoc_Location location(lift);
            frame.tools[1] = tools[1].Moved(location);
            for (auto &segment : frame.toolSegments[1])
                segment.shape = segment.shape.Moved(location);
            if (machineAssembly) {
                frame.machine = machineAssembly->at(state->punchTipYmm - active.tools->clearanceMm);
                frame.floor = machineAssembly->floor();
            }
        }
        if (impl_->referenceWall) {
            const auto &reference = OcctDocumentAccess::sheetOrientation(
                                        dynamic_cast<const CadDocument &>(*impl_->source))
                                        .reference;
            frame.orientation.edges =
                movedSheetReference(reference, machine * placement.walls[*impl_->referenceWall])
                    .edges;
            if (f == 0 && request.step > 0 && onPose && !frame.orientation.edges.empty()) {
                cancelled(stop);
                RouteDraftPose pose;
                for (const auto &edge : frame.orientation.edges) {
                    const auto point = [](const auto &p) { return PointMm{p.X(), p.Y(), p.Z()}; };
                    pose.edges.push_back(
                        {point(edge.points[0]), point(edge.points[1]), point(edge.normal)});
                }
                onPose(std::move(pose));
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
            const double budget = std::max(1.e-6, bend.lengthMm * bend.thicknessMm * tolerance * 2);
            const std::array toolBounds{dieBounds, collisionBox(frame.tools[1], tolerance)};
            materialBounds.clear();
            for (const auto &solid : material) {
                cancelled(stop);
                materialBounds.push_back(collisionBox(solid, tolerance));
            }
            for (std::size_t t = 0; t < 2; ++t) {
                double overlap = 0;
                auto occupied = compound();
                for (std::size_t i = 0; i < material.size(); ++i) {
                    TopoDS_Shape common;
                    overlap += intersectionVolume(material[i], frame.tools[t], materialBounds[i],
                                                  toolBounds[t], tolerance, stop, &common);
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
            if (intersectionVolume(frame.tools[0], frame.tools[1], toolBounds[0], toolBounds[1],
                                   tolerance, stop, &toolOverlap) > budget) {
                collision = ToolCollisionPair::DiePunch;
                evidence.push_back({"die", "punch", toolOverlap});
            }
            ++result->checkedToolPositions;
        }
        visual->frameConflicts.push_back(collision);
        const auto machineHits =
            MachineAssembly::collisions(frame, tolerance, stop, &rigidClearances);
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
        for (auto &face : impl_->faces) {
            cancelled(stop);
            Handle(Poly_Triangulation) mesh =
                new Poly_Triangulation(face.mesh->NbNodes(), face.mesh->NbTriangles(), false);
            const auto setNode = [&](std::size_t i, const gp_Pnt &point) {
                mesh->SetNode(static_cast<int>(i + 1), point);
                envelope.Add(point);
            };
            if (!face.source.IsNull()) {
                const auto envelopeBeforeFace = envelope;
                bool conflict = false;
                for (std::size_t i = 0; i < face.nodes.size() && !conflict; ++i) {
                    const auto &node = face.nodes[i];
                    std::optional<gp_Pnt> mapped;
                    for (const auto r : std::span(impl_->nodeRegions)
                                            .subspan(node.firstRegion, node.regionCount)) {
                        const auto next = maps[r](node.point).Transformed(machine);
                        if (mapped && mapped->Distance(next) > .02) {
                            conflict = true;
                            break;
                        }
                        mapped = next;
                    }
                    if (mapped)
                        setNode(i, *mapped);
                }
                if (conflict) {
                    envelope = envelopeBeforeFace;
                    const auto owner =
                        sourceFaceRegionOwner(face.source, impl_->regionSolids, impl_->regionBoxes,
                                              tolerance, ownerQueries, stop);
                    cancelled(stop);
                    require(owner.has_value(), "Inconsistent draft boundary mapping");
                    for (const auto &node : face.nodes) {
                        const auto candidates = std::span(impl_->nodeRegions)
                                                    .subspan(node.firstRegion, node.regionCount);
                        require(std::find(candidates.begin(), candidates.end(), *owner) !=
                                    candidates.end(),
                                "Source face owner disagrees with the draft boundary");
                    }
                    face.owner = owner;
                    face.source.Nullify();
                }
            }
            // With one candidate per vertex there is no ambiguity to resolve. Most of a
            // perforated sheet's faces use this direct path at every frame.
            if (face.source.IsNull()) {
                for (std::size_t i = 0; i < face.nodes.size(); ++i) {
                    const auto &node = face.nodes[i];
                    const auto region = face.owner.value_or(impl_->nodeRegions[node.firstRegion]);
                    setNode(i, maps[region](node.point).Transformed(machine));
                }
            }
            for (int i = 1; i <= face.mesh->NbTriangles(); ++i)
                mesh->SetTriangle(i, face.mesh->Triangle(i));
            mesh->Deflection(.03);
            TopoDS_Face skin;
            builder.MakeFace(skin, mesh);
            skin.Orientation(face.orientation);
            const auto region = face.owner.value_or(face.region);
            const bool selected =
                request.step && region >= layout.walls.size() &&
                std::find(zones.begin(), zones.end(),
                          layout.bends[region - layout.walls.size()].bendIndex) != zones.end();
            builder.Add(regions[selected ? 1 : 0], skin);
            if (face.side != SheetSide::Neutral)
                frame.orientation.sides[face.side == SheetSide::A ? 0 : 1].push_back(skin);
            if (region >= layout.walls.size())
                builder.Add(frame.bendZones[layout.bends[region - layout.walls.size()].bendIndex],
                            skin);
        }
        for (const auto &group : strokes) {
            if (group.size() == 1)
                continue;
            auto strokeShape = compound();
            for (const auto position : group)
                builder.Add(strokeShape, frame.bendZones[request.operations[position].bendIndex]);
            for (const auto position : group)
                frame.bendZones[request.operations[position].bendIndex] = strokeShape;
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
        buildCollisionHighlights(frame, evidence, bend.thicknessMm, stop);
        cancelled(stop);
        visual->frames.push_back(std::move(frame));
    }
    require(!envelope.IsVoid(), "Empty draft display");
    const auto lo = envelope.CornerMin(), hi = envelope.CornerMax();
    visual->envelope = {{lo.X(), lo.Y(), lo.Z()}, {hi.X(), hi.Y(), hi.Z()}};
    prepareBackgauge(*visual, analysis.lengthToleranceMm, stop);
    if (withTools && visual->gauging)
        result->checkedToolPositions += visual->gauging->addedRetractionPositions;
    result->visual = visual;
    cancelled(stop);
    return result;
}
} // namespace bending::cad
