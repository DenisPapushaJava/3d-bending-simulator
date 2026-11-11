#include "BendFixtures.h"
#include "BendOperation.h"
#include "CompositeBendFixtures.h"
#include "EndSupportFixtures.h"
#include "JointBends.h"
#include "RouteGeometry.h"
#include "ToolFixtures.h"
#include "TrayFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
void checkSkin(const cad::PreviewFrame &frame) {
    std::vector<std::unique_ptr<BRepClass3d_SolidClassifier>> solid;
    for (const auto &region : frame.regions)
        for (TopExp_Explorer e(region, TopAbs_SOLID); e.More(); e.Next())
            solid.push_back(std::make_unique<BRepClass3d_SolidClassifier>(e.Current()));
    unsigned count = 0;
    // Independent classification in each MOVED frame checks cached source membership,
    // including bend-rail nodes and faces with visible holes on the filled calculation body.
    for (const auto &region : frame.displayRegions)
        for (TopExp_Explorer f(region, TopAbs_FACE); f.More(); f.Next()) {
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), location);
            require(!mesh.IsNull(), "Visible surface lost its mesh");
            for (int n = 1; n <= mesh->NbNodes(); ++n) {
                const auto point = mesh->Node(n).Transformed(location.Transformation());
                bool contained = false;
                for (auto &classifier : solid) {
                    classifier->Perform(point, .02);
                    contained |=
                        classifier->State() == TopAbs_ON || classifier->State() == TopAbs_IN;
                }
                require(contained, "Visible skin left the checked material during motion");
                ++count;
            }
        }
    require(count > 0, "Empty visible skin");
}
application::DocumentPtr load(const std::filesystem::path &path) {
    const auto d = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
    require(bool(d), "Import failed");
    return d;
}
void checkBoundaryMembership() {
    const std::array<TopoDS_Shape, 3> bodies{
        BRepPrimAPI_MakeBox(gp_Pnt(-10, 0, 0), 10, 2, 4).Shape(),
        BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 2, 2, 4).Shape(),
        BRepPrimAPI_MakeBox(gp_Pnt(2, 0, 0), 10, 2, 4).Shape()};
    cad::SkinBoundaryLookup lookup(bodies, .001);
    require(lookup.membership({0, 1, 2}, {}) == 3 && lookup.membership({2, 1, 2}, {}) == 6,
            "Boundary fallback dropped a shared rail membership");
    require(lookup.membership({0, -.0005, 2}, {}) == 3 &&
                lookup.membership({0, -.002, 2}, {}) == 0 &&
                lookup.membership({100, 100, 100}, {}) == 0,
            "Boundary fallback exceeded its prepared tolerance");
    std::stop_source stop;
    stop.request_stop();
    require(lookup.membership({0, 1, 2}, stop.get_token()) == 0,
            "Cancelled boundary fallback returned partial memberships");
}
// An open cut-out in one flange contains a rigid return attached to the other
// side, around the axial end. Its rectangle overlaps both components, while the
// actual tangency faces do not. Sharp corners of the rigid return are deliberate:
// this tests a CAD transition, not recognition of additional forming operations.
TopoDS_Shape flangeWithRigidReturn(bool bridge = false) {
    auto shape = BRepAlgoAPI_Cut(fixtures::bentStrip(4, 2, std::numbers::pi / 2, 120),
                                 BRepPrimAPI_MakeBox(gp_Pnt(-31, 3, 40), 23, 4, 81).Shape())
                     .Shape();
    for (const auto &part : {BRepPrimAPI_MakeBox(gp_Pnt(-28, 4, 50), 2, 2, 78).Shape(),
                             BRepPrimAPI_MakeBox(gp_Pnt(-28, 4, 126), 34, 2, 2).Shape(),
                             BRepPrimAPI_MakeBox(gp_Pnt(4, -5, 120), 2, 4, 8).Shape(),
                             BRepPrimAPI_MakeBox(gp_Pnt(4, -5, 126), 2, 11, 2).Shape()})
        shape = BRepAlgoAPI_Fuse(shape, part).Shape();
    if (bridge)
        shape = BRepAlgoAPI_Fuse(shape, BRepPrimAPI_MakeBox(gp_Pnt(-28, 4, 70), 22, 2, 2).Shape())
                    .Shape();
    ShapeUpgrade_UnifySameDomain unify(shape, true, true, true);
    unify.Build();
    return unify.Shape();
}
void checkFlangeOwnership(const std::filesystem::path &folder) {
    for (const bool bridged : {false, true})
        for (const bool rotated : {false, true}) {
            auto shape = flangeWithRigidReturn(bridged);
            if (rotated) {
                gp_Trsf placement;
                placement.SetRotation(gp_Ax1(gp_Pnt(), gp_Dir(1, 2, 3)), .73);
                placement.SetTranslationPart(gp_Vec(217, -92, 13));
                shape = BRepBuilderAPI_Transform(shape, placement, true).Shape();
            }
            require(BRepCheck_Analyzer(shape).IsValid(), "Invalid ownership fixture");
            const auto file = folder / "flange-return.step";
            STEPControl_Writer writer;
            require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Ownership fixture write failed");
            const auto source = std::dynamic_pointer_cast<const cad::CadDocument>(load(file));
            // The final remaining bend of a route still needs the whole-part split.
            const auto state = cad::OcctDocumentAccess::routeState(
                *source, cad::OcctDocumentAccess::simulationShape(*source),
                cad::OcctDocumentAccess::shape(*source), {});
            const auto &bends = cad::OcctDocumentAccess::simulationBends(*state).bends;
            require(bends.size() == 1, "Ownership fixture lost its active cylinder pair");
            const auto &bend = bends.front();
            const core::BendTarget target{bend.angleRadians, bend.innerRadiusMm};
            std::ostringstream before;
            BRepTools::Write(cad::OcctDocumentAccess::shape(*state), before);
            for (const bool reversed : {false, true}) {
                const auto result = cad::buildBendOperation(state, 0, target, reversed, {}, {});
                if (bridged) {
                    const auto *error = std::get_if<application::BendPreviewError>(&result);
                    require(error && error->code ==
                                         application::BendPreviewErrorCode::UnsupportedGeometry,
                            "A material bridge was assigned to two moving sides");
                    continue;
                }
                if (const auto *error = std::get_if<application::BendPreviewError>(&result))
                    throw std::runtime_error("Real flange ownership: " + error->detail);
                const auto preview = std::get<application::BendPreviewPtr>(result);
                require(preview->frameCount() == 21, "Ownership bypassed complete motion");
                for (std::size_t f = 0; f < preview->frameCount(); ++f) {
                    const auto &frame = cad::OcctPreviewAccess::frame(*preview, f);
                    require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                                std::abs(volume(frame.shape) - source->metadata().volumeMm3) < .001,
                            "Real flange ownership lost material");
                    if (f == 0 || f == 10 || f == 20)
                        checkSkin(frame);
                }
            }
            std::stop_source stop;
            stop.request_stop();
            const auto cancelled =
                cad::buildBendOperation(state, 0, target, false, stop.get_token(), {});
            const auto *error = std::get_if<application::BendPreviewError>(&cancelled);
            require(error && error->code == application::BendPreviewErrorCode::Cancelled,
                    "Cancelled ownership check returned a preview");
            std::ostringstream after;
            BRepTools::Write(cad::OcctDocumentAccess::shape(*state), after);
            require(before.str() == after.str(), "Ownership changed the source BRep");
        }
}
void checkJointOperation(const std::filesystem::path &path) {
    const auto source = fixtures::jointBend();
    const auto import = [&](const TopoDS_Shape &shape, const char *name) {
        const auto file = path.parent_path() / name;
        STEPControl_Writer writer;
        require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.string().c_str()) == IFSelect_RetDone,
                "Joint fixture write failed");
        return load(file);
    };
    const auto doc = import(source, "joint.step");
    const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
    std::ostringstream before;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), before);
    const auto sourceBytes = std::string(doc->sourceStep());
    const auto &analysis = doc->metadata().bendAnalysis;
    require(analysis.bends.size() == 2, "Separate joint zones were merged at import");
    const std::array<std::size_t, 2> members{0, 1};
    const auto envelope = cad::jointBendEnvelope(analysis, members);
    require(envelope && std::abs(envelope->lengthMm - 120) < .001,
            "Joint envelope lost the gap between zones");
    auto changed = analysis;
    changed.bends[1].innerRadiusMm += 1;
    require(!cad::jointBendEnvelope(changed, members), "Different radii grouped");
    changed = analysis;
    changed.bends[1].angleRadians -= .1;
    require(!cad::jointBendEnvelope(changed, members), "Different angles grouped");
    changed = analysis;
    changed.bends[1].axisStart.x += 1;
    changed.bends[1].axisEnd.x += 1;
    require(!cad::jointBendEnvelope(changed, members), "Offset axes grouped");
    changed = analysis;
    changed.lengthToleranceMm = .01;
    changed.bends[1].axisStart.x += .005;
    changed.bends[1].axisEnd.x -= .005;
    require(!cad::jointBendEnvelope(changed, members),
            "Nearby endpoints hid angularly incompatible axes");
    const auto &bend = analysis.bends.front();
    const core::BendTarget target{bend.angleRadians, bend.innerRadiusMm};
    const core::PressTools parameters{16, std::numbers::pi / 3, std::numbers::pi / 6, .8, 120, 10};
    const auto assignment = tool_fixture::explicitPair(parameters);
    for (const bool reversed : {false, true}) {
        const auto result =
            cad::buildBendOperation(doc, 0, target, reversed, {}, parameters, true, assignment,
                                    false, 3, {}, true, nullptr, members);
        if (const auto *error = std::get_if<application::BendPreviewError>(&result))
            throw std::runtime_error("Joint operation: " + error->detail);
        const auto preview = std::get<application::BendPreviewPtr>(result);
        require(preview->activeBendCount() == 2 && preview->otherFormedBends() == 0 &&
                    preview->frameCount() == 61 && !preview->collision() &&
                    preview->punchRetraction(),
                "Joint operation bypassed the full press cycle");
        for (std::size_t f = 0; f < preview->frameCount(); ++f) {
            const auto &frame = cad::OcctPreviewAccess::frame(*preview, f);
            double material = 0;
            for (const auto &region : frame.regions)
                material += volume(region);
            require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                        std::abs(material - doc->metadata().volumeMm3) < .001,
                    "Joint motion lost material or filled the gap");
            if (f == 0 || f == 20 || f == 40)
                checkSkin(frame);
        }
    }
    const auto partial = import(end_support_fixture::joint(), "joint-partial.step");
    require(partial->metadata().bendAnalysis.bends.size() == 2,
            "End cuts changed the joint zone identities");
    for (const bool reversed : {false, true}) {
        const auto result =
            cad::buildBendOperation(partial, 0, target, reversed, {}, parameters, true, assignment,
                                    true, 3, {}, true, nullptr, members);
        if (const auto *failure = std::get_if<application::BendPreviewError>(&result))
            throw std::runtime_error("Partial joint operation: " + failure->detail);
        const auto preview = std::get<application::BendPreviewPtr>(result);
        require(preview->activeBendCount() == 2 && preview->hasPartialEndSupport() &&
                    preview->frameCount() == 61 && preview->punchRetraction() &&
                    application::validEndSupportIntervals(*preview, 3, .001),
                "Joint partial support lost its complete checked scope");
        const auto intervals = preview->endSupportIntervals();
        require(intervals.size() == 2 &&
                    intervals[0].fromMm + intervals[0].material.bendLengthMm + 19.99 <
                        intervals[1].fromMm,
                "Joint support filled the relief between real intervals");
        std::vector<double> losses;
        for (const auto &interval : intervals) {
            losses.push_back(interval.material.supportedFromMm);
            losses.push_back(interval.material.bendLengthMm - interval.material.supportedToMm);
        }
        std::sort(losses.begin(), losses.end());
        for (std::size_t i = 0; i < losses.size(); ++i)
            require(std::abs(losses[i] - std::array{.46, 1.25, 2.5, 3.}[i]) < .001,
                    "A member's end loss was replaced by a common envelope loss");
    }
    const auto excessive =
        import(end_support_fixture::joint(.46, 3, 3.2, 2.5), "joint-excessive.step");
    for (const auto &input : {partial, excessive}) {
        const auto result = cad::buildBendOperation(
            input, 0, target, false, {}, parameters, true, assignment, input != partial,
            input == partial ? 0 : 3, {}, true, nullptr, members);
        const auto *failure = std::get_if<application::BendPreviewError>(&result);
        require(failure && failure->code ==
                               (input == partial
                                    ? application::BendPreviewErrorCode::SupportNotProven
                                    : application::BendPreviewErrorCode::EndSupportLimitExceeded),
                "Joint end policy ignored strict zero or an excessive member");
    }
    application::BendRouteRequest partialRequest;
    partialRequest.withTools = partialRequest.allowPartialEndSupport = true;
    partialRequest.maximumEndSupportLossMm = 3;
    for (std::size_t i = 0; i < 2; ++i)
        partialRequest.operations.push_back({"partial-" + std::to_string(i), i, target,
                                             core::toolParameters(assignment), assignment});
    cad::SingleBendPreviewBuilder partialBuilder;
    for (const bool automatic : {true, false}) {
        partialRequest.automatic = automatic;
        const auto result = partialBuilder.buildRoute(partial, partialRequest, {});
        if (const auto *failure = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error("Partial joint route: " + failure->detail);
        const auto route = std::get<application::BendRoutePtr>(result);
        require(route->partialEndSupport && route->steps.size() == 1 &&
                    route->steps.front().preview->endSupportIntervals().size() == 2,
                "Manual/automatic route collapsed joint support reports");
    }
    partialRequest.maximumEndSupportLossMm = 2;
    require(std::holds_alternative<application::BendRouteError>(
                partialBuilder.buildRoute(partial, partialRequest, {})),
            "Joint cache ignored a tightened end limit");
    partialRequest.maximumEndSupportLossMm = 3;
    require(std::holds_alternative<application::BendRoutePtr>(
                partialBuilder.buildRoute(partial, partialRequest, {})),
            "Joint cache retained a refusal after restoring the limit");
    partialRequest.maximumEndSupportLossMm.reset();
    for (const bool automatic : {false, true}) {
        partialRequest.automatic = automatic;
        const auto result = partialBuilder.buildRoute(excessive, partialRequest, {});
        if (const auto *failure = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error("Conditional joint route: " + failure->detail);
        const auto route = std::get<application::BendRoutePtr>(result);
        const auto preview = route->steps.front().preview;
        require(route->steps.size() == 1 && route->partialEndSupport &&
                    preview->endSupportIntervals().size() == 2 && !preview->endSupportLimitMm() &&
                    application::validEndSupportIntervals(*preview, {}, .001),
                "Conditional joint route lost a member or its warning scope");
    }
    partialRequest.maximumEndSupportLossMm = 3;
    require(std::holds_alternative<application::BendRouteError>(
                partialBuilder.buildRoute(excessive, partialRequest, {})),
            "A conditional group cache entry escaped the restored bounded policy");
    // Composite radial end with a slope and straight cap, identical through thickness.
    const auto trimmed =
        import(BRepAlgoAPI_Cut(composite_fixture::part(120),
                               BRepPrimAPI_MakeBox(gp_Pnt(-40, -5, 45), 55, 20, 20).Shape())
                   .Shape(),
               "joint-trimmed.step");
    require(trimmed->metadata().bendAnalysis.bends.size() == 2 &&
                std::any_of(trimmed->metadata().bendAnalysis.bends.begin(),
                            trimmed->metadata().bendAnalysis.bends.end(),
                            [](const auto &zone) { return zone.trimmedEnds; }),
            "Trimmed joint fixture lost its real cylindrical cut");
    const auto trimmedResult =
        cad::buildBendOperation(trimmed, 0, target, false, {}, parameters, true, assignment, true,
                                3, {}, true, nullptr, members);
    if (const auto *failure = std::get_if<application::BendPreviewError>(&trimmedResult))
        throw std::runtime_error("Trimmed joint support: " + failure->detail);
    const auto trimmedPreview = std::get<application::BendPreviewPtr>(trimmedResult);
    require(trimmedPreview->hasPartialEndSupport() &&
                application::validEndSupportIntervals(*trimmedPreview, 3, .001),
            "A true trimmed joint did not retain its bounded support report");
    for (std::size_t f = 0; f < trimmedPreview->frameCount(); ++f) {
        const auto &frame = cad::OcctPreviewAccess::frame(*trimmedPreview, f);
        double material = 0;
        for (const auto &region : frame.regions)
            material += volume(region);
        require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                    std::abs(material - trimmed->metadata().volumeMm3) < .001,
                "Trimmed joint support changed the source end contour or material");
    }
    auto shortTools = parameters;
    shortTools.lengthMm = 110;
    const auto shortAssignment = tool_fixture::explicitPair(shortTools);
    const auto shortResult =
        cad::buildBendOperation(doc, 0, target, false, {}, shortTools, true, shortAssignment, false,
                                3, {}, true, nullptr, members);
    const auto *error = std::get_if<application::BendPreviewError>(&shortResult);
    require(error && error->code == application::BendPreviewErrorCode::ToolTooShort,
            "Joint operation measured summed metal lengths instead of the full span");
    const auto notched = import(
        BRepAlgoAPI_Cut(source, BRepPrimAPI_MakeBox(gp_Pnt(3, -31, 20), 4, 28, 2).Shape()).Shape(),
        "joint-notched.step");
    require(notched->metadata().bendAnalysis.bends.size() == 2 &&
                notched->metadata().closedCutouts.status != application::ClosedCutoutStatus::Filled,
            "Internal support notch hid a zone or was filled");
    const auto notchResult =
        cad::buildBendOperation(notched, 0, target, false, {}, parameters, true, assignment, true,
                                3, {}, true, nullptr, members);
    const auto *notchError = std::get_if<application::BendPreviewError>(&notchResult);
    require(notchError && notchError->code == application::BendPreviewErrorCode::SupportNotProven,
            "A real internal support gap was confused with the relief between zones");
    gp_Trsf rotation, translation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), .73);
    translation.SetTranslation(gp_Vec(123.4, -456.7, 89.1));
    const auto rotated =
        import(BRepBuilderAPI_Transform(source, translation * rotation, true).Shape(),
               "joint-rotated.step");
    const auto &rotatedBend = rotated->metadata().bendAnalysis.bends.front();
    const auto rotatedResult =
        cad::buildBendOperation(rotated, 0, {rotatedBend.angleRadians, rotatedBend.innerRadiusMm},
                                false, {}, {}, false, {}, false, {}, {}, true, nullptr, members);
    require(std::holds_alternative<application::BendPreviewPtr>(rotatedResult) &&
                std::get<application::BendPreviewPtr>(rotatedResult)->activeBendCount() == 2,
            "Joint geometry depends on world axes or origin");
    auto gaugedAssignment = assignment;
    gaugedAssignment.backgauge = core::BackgaugeSetup{};
    const auto gauged =
        cad::buildBendOperation(doc, 0, target, false, {}, core::toolParameters(gaugedAssignment),
                                true, gaugedAssignment, false, 3, {}, true, nullptr, members);
    if (const auto *failure = std::get_if<application::BendPreviewError>(&gauged))
        throw std::runtime_error("Joint gauging: " + failure->detail);
    require(std::get<application::BendPreviewPtr>(gauged)->backgauge() &&
                std::get<application::BendPreviewPtr>(gauged)->backgauge()->status ==
                    application::BackgaugeStatus::Checked,
            "Joint operation bypassed the common X/R backgauge");
    auto blocked = assignment;
    blocked.punch.id += "-blocked";
    auto &edges = blocked.punch.drawing->edges;
    const auto right = edges[2];
    const core::DrawingPoint shoulder{right.start.x + .25 * (right.end.x - right.start.x),
                                      right.start.y + .25 * (right.end.y - right.start.y)};
    const auto line = [](core::DrawingPoint a, core::DrawingPoint b) {
        return core::DrawingEdge{a, b, {}, 0};
    };
    // Own finite contour: preserve the valid circular nose, add a blocking wing.
    edges = {edges[0],
             edges[1],
             line(right.start, shoulder),
             line(shoulder, {50, shoulder.y}),
             line({50, shoulder.y}, {50, right.end.y}),
             line({50, right.end.y}, right.end),
             edges[3]};
    require(core::hasCompleteDrawingToolGeometry(blocked),
            "Blocked joint fixture lost its DXF profile");
    const auto blockedResult =
        cad::buildBendOperation(partial, 0, target, false, {}, core::toolParameters(blocked), true,
                                blocked, true, 3, {}, true, nullptr, members);
    const auto *blockedError = std::get_if<application::BendPreviewError>(&blockedResult);
    require(blockedError &&
                blockedError->code == application::BendPreviewErrorCode::ToolIntersection &&
                blockedError->diagnostic && blockedError->diagnostic->activeBendCount() == 2 &&
                blockedError->diagnostic->hasPartialEndSupport() &&
                blockedError->diagnostic->endSupportIntervals().size() == 2 &&
                blockedError->diagnostic->collision(),
            "Joint operation bypassed a real tool collision or lost the stopping diagnostic");
    application::BendRouteRequest request;
    request.withTools = true;
    request.maximumChecks = 32;
    request.maximumTimeMs = 60000;
    for (std::size_t i = 0; i < analysis.bends.size(); ++i)
        request.operations.push_back(
            {"zone-" + std::to_string(i),
             i,
             {analysis.bends[i].angleRadians, analysis.bends[i].innerRadiusMm},
             core::toolParameters(assignment),
             assignment});
    cad::SingleBendPreviewBuilder builder;
    for (const bool automatic : {true, false}) {
        request.automatic = automatic;
        const auto result = builder.buildRoute(doc, request, {});
        if (const auto *failure = std::get_if<application::BendRouteError>(&result))
            throw std::runtime_error("Joint route: " + failure->detail);
        const auto route = std::get<application::BendRoutePtr>(result);
        require(route->steps.size() == 1 && route->steps[0].additionalBends.size() == 1 &&
                    route->steps[0].preview->activeBendCount() == 2 &&
                    !route->steps[0].preview->collision(),
                "Automatic/manual checked route lost the joint operation");
    }
    request.withTools = false;
    request.automatic = true;
    request.allowJointBends = false;
    const auto separate = builder.buildRoute(doc, request, {});
    require(std::holds_alternative<application::BendRoutePtr>(separate) &&
                std::get<application::BendRoutePtr>(separate)->steps.size() == 2,
            "Joint support broke the separate geometric route");
    request.allowJointBends = true;
    const auto together = builder.buildRoute(doc, request, {});
    require(std::holds_alternative<application::BendRoutePtr>(together) &&
                std::get<application::BendRoutePtr>(together)->steps.size() == 1,
            "Joint and single transition cache keys were confused");
    // Persisting the complete grouped path must not overwrite a single-zone key.
    request.allowJointBends = false;
    const auto singlesAgain = builder.buildRoute(doc, request, {});
    require(std::holds_alternative<application::BendRoutePtr>(singlesAgain),
            "Grouped path retention poisoned a later separate route");
    const auto separateRoute = std::get<application::BendRoutePtr>(singlesAgain);
    require(separateRoute->steps.size() == 2 &&
                std::all_of(separateRoute->steps.begin(), separateRoute->steps.end(),
                            [](const auto &step) {
                                return step.additionalBends.empty() &&
                                       step.preview->activeBendCount() == 1;
                            }),
            "A single-zone cache key returned a grouped preview");
    request.allowJointBends = true;
    auto scoped = request;
    scoped.withTools = scoped.automatic = true;
    auto third = scoped.operations.front();
    third.id = "third";
    third.bendIndex = 2;
    scoped.operations.push_back(third);
    const auto oldThird = scoped.operations.back();
    auto replacement = assignment;
    replacement.clearanceMm = 17;
    scoped.toolVariants = {{replacement, 0, {1}}};
    require(application::applyRouteToolVariant(scoped, 0) &&
                scoped.operations[0].assignment == replacement &&
                scoped.operations[1].assignment == replacement &&
                scoped.operations.back() == oldThird,
            "Joint checked-tool variant changed a different operation");
    const auto beforeInvalid = scoped.operations;
    scoped.toolVariants.front().additionalBendIndices = {0};
    require(!application::applyRouteToolVariant(scoped, 0) && scoped.operations == beforeInvalid,
            "Repeated joint target partially changed the request");
    scoped.toolVariants.front().additionalBendIndices = {999};
    require(!application::applyRouteToolVariant(scoped, 0) && scoped.operations == beforeInvalid,
            "Missing joint target partially changed the request");
    std::ostringstream after;
    BRepTools::Write(cad::OcctDocumentAccess::shape(*native), after);
    require(before.str() == after.str() && sourceBytes == doc->sourceStep(),
            "Joint transitions mutated the imported BRep or STEP");
}
int main(int argc, char **argv) {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("multi-operation-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        checkBoundaryMembership();
        std::filesystem::create_directories(folder);
        if (argc == 1) {
            checkFlangeOwnership(folder);
            checkJointOperation(folder / "joint.step");
        }
        const bool sample = argc >= 2;
        auto file = sample ? std::filesystem::path(argv[1]) : folder / "tray.step";
        if (!sample) {
            STEPControl_Writer writer;
            require(writer.Transfer(fixtures::tray(), STEPControl_AsIs) == IFSelect_RetDone &&
                        writer.Write(file.string().c_str()) == IFSelect_RetDone,
                    "Fixture write failed");
        }
        const auto doc = load(file);
        const auto bytes = std::string(doc->sourceStep());
        const auto &bends = doc->metadata().bendAnalysis.bends;
        unsigned successful = 0;
        for (std::size_t i = 0; i < bends.size(); ++i) {
            if (argc == 4 && std::string_view(argv[2]) == "--bend" && i != std::stoul(argv[3]))
                continue;
            const auto &b = bends[i];
            const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
            const auto result = cad::SingleBendPreviewBuilder{}.build(doc, i, target, false, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&result)) {
                std::cout << "B" << i + 1 << " kinematic error: " << error->detail << '\n';
                if (!sample)
                    throw std::runtime_error(error->detail);
                continue;
            }
            const auto preview = std::get<application::BendPreviewPtr>(result);
            ++successful;
            require(preview->otherFormedBends() == bends.size() - 1 && preview->frameCount() == 21,
                    "Whole-part operation scope lost");
            const double expected = volume(cad::OcctPreviewAccess::frame(*preview, 20).shape);
            for (std::size_t f = 0; f < preview->frameCount(); ++f) {
                const auto &frame = cad::OcctPreviewAccess::frame(*preview, f);
                require(BRepCheck_Analyzer(frame.shape).IsValid() &&
                            std::abs(volume(frame.shape) - expected) <
                                std::max(.01, expected * 1.e-7),
                        "Whole-part frame lost material");
                require(!frame.displayShape.IsNull(), "Original holes not displayed");
                if (!sample && (f == 0 || f == 10 || f == 20))
                    checkSkin(frame);
            }
            if (!sample && i > 0)
                continue; // One press cycle plus every kinematic component split.
            core::PressTools tools{
                8 * b.thicknessMm, std::numbers::pi / 3, std::numbers::pi / 6, .8, b.lengthMm, 40};
            const auto motion =
                cad::SingleBendPreviewBuilder{}.buildToolMotion(doc, i, target, tools, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&motion))
                std::cout << "B" << i + 1 << " conditional tool L=" << b.lengthMm
                          << " error=" << error->detail
                          << " frame=" << error->frameIndex.value_or(999)
                          << " diagnostic=" << bool(error->diagnostic) << '\n';
            else {
                const auto p = std::get<application::BendPreviewPtr>(motion);
                require(p->frameCount() == 61 && p->punchRetraction() &&
                            p->otherFormedBends() == bends.size() - 1,
                        "Incomplete press motion");
                if (!sample)
                    for (auto f : {0u, 20u, 40u, 60u})
                        checkSkin(cad::OcctPreviewAccess::frame(*p, f));
                std::cout << "B" << i + 1 << " conditional tool L=" << b.lengthMm << " motion OK\n";
            }
        }
        require(successful > 0 && doc->sourceStep() == bytes,
                "No supported operation or source changed");
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
