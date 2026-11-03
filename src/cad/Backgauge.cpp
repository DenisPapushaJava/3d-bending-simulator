#include "Backgauge.h"
#include "CollisionHighlight.h"
#include "CollisionReuse.h"
#include "GeometryCacheInternal.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/MachineAssembly.h"
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <algorithm>
#include <cmath>
#include <gp_Ax3.hxx>
#include <stdexcept>

namespace bending::cad {
namespace {
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Backgauge cancelled");
}
using Triangle = std::array<gp_Pnt, 3>;
struct Skin {
    std::vector<Triangle> triangles;
    std::vector<core::GaugeContactPatch> patches;
};
Skin contactSkin(const PreviewFrame &frame, bool filled, double thickness, std::stop_token stop) {
    Skin result;
    const bool source = std::any_of(frame.displayRegions.begin(), frame.displayRegions.end(),
                                    [](const auto &s) { return !s.IsNull(); });
    if (filled && !source)
        throw std::runtime_error("Physical source skin missing for backgauge contacts");
    const auto &regions = source ? frame.displayRegions : frame.regions;
    for (const auto &region : regions)
        for (TopExp_Explorer it(region, TopAbs_FACE); it.More(); it.Next()) {
            cancelled(stop);
            const auto face = TopoDS::Face(it.Current());
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(face, location);
            if (mesh.IsNull())
                throw std::runtime_error("Backgauge source triangulation missing");
            std::vector<Triangle> triangles;
            double xmin = 1.e100, xmax = -1.e100, ymin = 1.e100, ymax = -1.e100;
            for (int n = 1; n <= mesh->NbTriangles(); ++n) {
                int a, b, c;
                mesh->Triangle(n).Get(a, b, c);
                Triangle t{mesh->Node(a).Transformed(location.Transformation()),
                           mesh->Node(b).Transformed(location.Transformation()),
                           mesh->Node(c).Transformed(location.Transformation())};
                if (face.Orientation() == TopAbs_REVERSED)
                    std::swap(t[1], t[2]);
                for (const auto &p : t) {
                    xmin = std::min(xmin, p.X());
                    xmax = std::max(xmax, p.X());
                    ymin = std::min(ymin, p.Y());
                    ymax = std::max(ymax, p.Y());
                }
                triangles.push_back(t);
                result.triangles.push_back(t);
                if (result.triangles.size() > 250000)
                    throw std::runtime_error("Backgauge source triangle limit reached");
            }
            // Check the whole source face: a tangent facet of a curved bend
            // must not become a straight reference.
            if (xmax - xmin > 1.e-6 || triangles.empty())
                continue;
            const auto kind = ymax - ymin <= thickness + 1.e-5
                                  ? core::GaugeReferenceKind::StraightEdge
                                  : core::GaugeReferenceKind::RightAngleWall;
            for (const auto &t : triangles) {
                const auto n = gp_Vec(t[0], t[1]).Crossed(gp_Vec(t[0], t[2]));
                if (n.X() <= 1.e-9)
                    continue;
                result.patches.push_back(
                    {(xmin + xmax) / 2,
                     {{{t[0].Y(), t[0].Z()}, {t[1].Y(), t[1].Z()}, {t[2].Y(), t[2].Z()}}},
                     kind});
            }
        }
    return result;
}
// Reject a contact inside a closed cutout: its rearward ray reaches material.
bool exposed(const Skin &skin, const core::BackgaugePlacement &p, double tolerance) {
    for (const auto z : p.contactZmm)
        for (const auto &t : skin.triangles) {
            const double ay = t[1].Y() - t[0].Y(), az = t[1].Z() - t[0].Z();
            const double by = t[2].Y() - t[0].Y(), bz = t[2].Z() - t[0].Z();
            const double det = ay * bz - az * by;
            if (std::abs(det) < 1.e-10)
                continue;
            const double y = p.contactYmm - t[0].Y(), v = z - t[0].Z();
            const double u = (y * bz - v * by) / det, w = (ay * v - az * y) / det;
            if (u < -1.e-8 || w < -1.e-8 || u + w > 1 + 1.e-8)
                continue;
            const double x = t[0].X() + u * (t[1].X() - t[0].X()) + w * (t[2].X() - t[0].X());
            if (x > p.contactXmm + tolerance)
                return false;
        }
    return true;
}
TopoDS_Shape fingerBody(double width) {
    auto key = geometryKey("backgauge-body-v1");
    key << width;
    return cachedToolShape(
        key.str(),
        [&] {
            BRepBuilderAPI_MakePolygon polygon;
            const std::array<std::array<double, 2>, 8> profile{{{0, 0},
                                                                {0, 10.5},
                                                                {28.8, 10.5},
                                                                {28.8, 21},
                                                                {65.9, 21},
                                                                {65.9, 25.5},
                                                                {150, 25.5},
                                                                {150, 0}}};
            for (const auto q : profile)
                polygon.Add(gp_Pnt(q[0], q[1], 0));
            polygon.Close();
            const auto body = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(polygon.Wire()).Face(),
                                                    gp_Vec(0, 0, width))
                                  .Shape();
            const auto holder = BRepPrimAPI_MakeBox(gp_Pnt(110, -50, 0), 40, 50, width).Shape();
            BRep_Builder builder;
            TopoDS_Compound compound;
            builder.MakeCompound(compound);
            builder.Add(compound, body);
            builder.Add(compound, holder);
            if (!BRepCheck_Analyzer(compound).IsValid())
                throw std::runtime_error("Invalid simplified backgauge body");
            return compound;
        },
        .1);
}
std::vector<MachineDisplayComponent> fingers(const core::BackgaugePlacement &p,
                                             const core::BackgaugeSetup &setup,
                                             const TopoDS_Shape &prototype) {
    std::vector<MachineDisplayComponent> result;
    for (std::size_t i = 0; i < 2; ++i) {
        gp_Trsf position;
        position.SetTranslation(gp_Vec(p.xMm, p.rMm, p.zMm[i] - setup.fingerWidthMm / 2));
        result.push_back({"backgauge-" + std::to_string(i),
                          i ? "Правый палец упора" : "Левый палец упора",
                          prototype.Moved(TopLoc_Location(position)), false, false, false, true});
    }
    return result;
}
TopoDS_Shape material(const PreviewFrame &frame) {
    BRep_Builder builder;
    TopoDS_Compound shape;
    builder.MakeCompound(shape);
    bool any = false;
    for (const auto &r : frame.regions)
        if (!r.IsNull()) {
            builder.Add(shape, r);
            any = true;
        }
    return any ? TopoDS_Shape(shape) : frame.shape;
}
PreviewFrame checkingFrame(const PreviewFrame &source) {
    PreviewFrame frame;
    frame.shape = material(source);
    frame.tools = source.tools;
    frame.machine = source.machine;
    frame.floor = source.floor;
    return frame;
}
} // namespace
std::optional<double> backgaugeInvariantSpan(const CadDocument &doc, std::stop_token stop) {
    const auto &bends = OcctDocumentAccess::simulationBends(doc).bends;
    if (bends.empty())
        return {};
    const auto axis = [](const auto &b) {
        return gp_Vec(gp_Pnt(b.axisStart.x, b.axisStart.y, b.axisStart.z),
                      gp_Pnt(b.axisEnd.x, b.axisEnd.y, b.axisEnd.z));
    };
    auto direction = axis(bends.front());
    if (direction.SquareMagnitude() < 1.e-12)
        return {};
    direction.Normalize();
    for (const auto &b : bends) {
        cancelled(stop);
        auto next = axis(b);
        if (next.SquareMagnitude() < 1.e-12 ||
            direction.Crossed(next.Normalized()).Magnitude() > 1.e-12)
            return {}; // Other bends can change projection along this axis.
    }
    gp_Trsf basis;
    basis.SetTransformation(gp_Ax3(gp_Pnt(), gp_Dir(direction)));
    const auto projected =
        BRepBuilderAPI_Transform(OcctDocumentAccess::shape(doc), basis, false).Shape();
    Bnd_Box bounds;
    BRepBndLib::AddOptimal(projected, bounds, false, true);
    cancelled(stop);
    if (bounds.IsVoid() || bounds.IsOpen())
        return {};
    return bounds.CornerMax().Z() - bounds.CornerMin().Z();
}
std::optional<std::string> backgaugeContactIssue(const PreviewFrame &frame,
                                                 const core::BackgaugeSetup &setup,
                                                 double thickness, double centre, double withdrawal,
                                                 bool filled, double tolerance,
                                                 std::stop_token stop) {
    const auto skin = contactSkin(frame, filled, thickness, stop);
    const auto candidates = core::backgaugeCandidates(skin.patches, setup, thickness, centre, stop);
    cancelled(stop);
    for (const auto &p : candidates)
        if (p.xMm + withdrawal <= setup.xMaxMm && exposed(skin, p, tolerance))
            return {};
    return "В начальном положении не найдены два физических контакта упора "
           "в заданных пределах X/R/Z с учётом отвода. Проверьте настройки упора.";
}
void prepareBackgauge(CadPreview &preview, double tolerance, std::stop_token stop) {
    if (!preview.setup || !preview.setup->tools.backgauge || preview.frames.empty())
        return;
    const auto setup = *preview.setup->tools.backgauge;
    preview.gauging = application::BackgaugeReport{setup};
    const auto skin = contactSkin(preview.frames.front(), preview.filledCutouts,
                                  preview.geometry.thicknessMm, stop);
    const auto candidates = core::backgaugeCandidates(
        skin.patches, setup, preview.geometry.thicknessMm, preview.setup->tools.lengthMm / 2, stop);
    cancelled(stop);
    const auto prototype = candidates.empty() ? TopoDS_Shape{} : fingerBody(setup.fingerWidthMm);
    std::size_t hold = 0;
    bool movement = preview.frames.size() > 1 && setup.retractMm > 0;
    if (movement && preview.motion) {
        const auto it =
            std::find_if(preview.states.begin(), preview.states.end(), [](const auto &s) {
                return s.phase == simulation::PressPhase::Bending &&
                       std::abs(s.section.angleRadians) < 1.e-8;
            });
        // A diagnostic prefix can end before the punch touches. Do not withdraw
        // an unheld sheet merely to complete that prefix.
        movement = it != preview.states.end();
        if (movement)
            hold = static_cast<std::size_t>(it - preview.states.begin());
    }
    struct Pose {
        std::size_t source;
        double fraction;
    };
    std::vector<Pose> poses;
    for (std::size_t i = 0; i < preview.frames.size(); ++i) {
        poses.push_back({i, movement && i > hold ? 1. : 0.});
        if (movement && i == hold)
            for (unsigned j = 1; j <= 4; ++j)
                poses.push_back({i, j / 4.});
    }
    const auto at = [&](core::BackgaugePlacement p, double fraction) {
        p.xMm += setup.retractMm * fraction;
        return fingers(p, setup, prototype);
    };
    std::optional<core::BackgaugePlacement> placement;
    std::size_t bestPrefix = 0;
    std::vector<std::vector<MachineCollisionEvidence>> chosenHits;
    std::array<std::vector<MachineDisplayComponent>, 5> chosenBodies;
    bool clear = false;
    for (const auto &p : candidates) {
        cancelled(stop);
        if ((movement && p.xMm + setup.retractMm > setup.xMaxMm) || !exposed(skin, p, tolerance))
            continue;
        RigidClearanceCache cache;
        std::array<std::vector<MachineDisplayComponent>, 5> samples;
        for (std::size_t j = 0; j < samples.size(); ++j)
            samples[j] = at(p, static_cast<double>(j) / 4);
        std::size_t prefix = 0;
        std::vector<std::vector<MachineCollisionEvidence>> checked;
        for (const auto pose : poses) {
            cancelled(stop);
            auto frame = checkingFrame(preview.frames[pose.source]);
            const auto &bodies = samples[static_cast<std::size_t>(pose.fraction * 4)];
            frame.machine.insert(frame.machine.end(), bodies.begin(), bodies.end());
            checked.push_back(MachineAssembly::backgaugeCollisions(frame, tolerance, stop, &cache));
            if (!checked.back().empty())
                break;
            ++prefix;
        }
        if (!placement || prefix > bestPrefix) {
            placement = p;
            bestPrefix = prefix;
            chosenHits = std::move(checked);
            chosenBodies = std::move(samples);
        }
        if (prefix == poses.size()) {
            clear = true;
            break;
        }
    }
    auto &report = *preview.gauging;
    report.placement = placement;
    if (!placement) {
        report.detail =
            "Не найдены два доступных контакта заднего упора в заданных пределах "
            "с учётом отвода. Поиск ограничен прямыми наружными гранями и стенками 90°.";
        return;
    }
    report.addedRetractionPositions = movement ? 4 : 0;
    report.status =
        clear ? application::BackgaugeStatus::Checked : application::BackgaugeStatus::Collision;
    report.detail =
        "Упрощённые пальцы: отвод X после геометрического касания пуансона, затем гибка. "
        "Проверены подготовленные положения, включая 4 положения отвода. "
        "Непрерывное движение, физическое удержание и перенос детали не проверены. "
        "Координаты кинематические, не для ЧПУ.";
    if (!movement)
        report.detail =
            "Неподвижные упрощённые пальцы; проверены подготовленные положения. "
            "Отвод, физическое удержание и перенос не проверены. Координаты не для ЧПУ.";
    auto oldFrames = std::move(preview.frames);
    auto oldStates = std::move(preview.states);
    auto oldConflicts = std::move(preview.machineConflicts);
    auto oldToolConflicts = std::move(preview.frameConflicts);
    const auto heldFrame = oldFrames[hold];
    preview.frames.reserve(poses.size());
    preview.machineConflicts.resize(poses.size());
    if (!oldToolConflicts.empty())
        preview.frameConflicts.resize(poses.size());
    RigidClearanceCache cache;
    for (std::size_t i = 0; i < poses.size(); ++i) {
        cancelled(stop);
        const auto pose = poses[i];
        auto frame = pose.source == hold && pose.fraction > 0 ? heldFrame
                                                              : std::move(oldFrames[pose.source]);
        if (!oldStates.empty())
            preview.states.push_back(oldStates[pose.source]);
        if (pose.source < oldConflicts.size())
            preview.machineConflicts[i] = oldConflicts[pose.source];
        if (pose.source < oldToolConflicts.size())
            preview.frameConflicts[i] = oldToolConflicts[pose.source];
        if (i == 0)
            for (const auto z : placement->contactZmm)
                frame.backgaugeContacts.emplace_back(placement->contactXmm, placement->contactYmm,
                                                     z);
        const auto &bodies = chosenBodies[static_cast<std::size_t>(pose.fraction * 4)];
        frame.machine.insert(frame.machine.end(), bodies.begin(), bodies.end());
        const auto hits = i < chosenHits.size()
                              ? std::move(chosenHits[i])
                              : MachineAssembly::backgaugeCollisions(checkingFrame(frame),
                                                                     tolerance, stop, &cache);
        ++report.checkedPositions;
        if (!hits.empty()) {
            if (!preview.machineConflicts[i])
                preview.machineConflicts[i] = hits.front().pair;
            std::vector<CollisionEvidence> evidence;
            for (const auto &h : hits)
                evidence.push_back({h.pair.firstId, h.pair.secondId, h.overlap});
            auto previous = std::move(frame.collisionHighlights);
            buildCollisionHighlights(frame, evidence, preview.geometry.thicknessMm, stop);
            frame.collisionHighlights.insert(frame.collisionHighlights.end(), previous.begin(),
                                             previous.end());
        }
        preview.frames.push_back(std::move(frame));
    }
    // Prepared working bounds include the gauges, without fitting the whole press.
    preview.envelope.minimum.x = std::min(preview.envelope.minimum.x, placement->xMm);
    preview.envelope.maximum.x = std::max(preview.envelope.maximum.x,
                                          placement->xMm + (movement ? setup.retractMm : 0) + 150);
    preview.envelope.minimum.y = std::min(preview.envelope.minimum.y, placement->rMm - 50);
    preview.envelope.maximum.y = std::max(preview.envelope.maximum.y, placement->rMm + 25.5);
    preview.envelope.minimum.z =
        std::min(preview.envelope.minimum.z,
                 std::min(placement->zMm[0], placement->zMm[1]) - setup.fingerWidthMm / 2);
    preview.envelope.maximum.z =
        std::max(preview.envelope.maximum.z,
                 std::max(placement->zMm[0], placement->zMm[1]) + setup.fingerWidthMm / 2);
}
void truncateCollisionPreview(CadPreview &preview, std::size_t failed) {
    preview.frames.resize(failed + 1);
    if (!preview.states.empty())
        preview.states.resize(failed + 1);
    preview.machineConflicts.resize(failed + 1);
    if (!preview.frameConflicts.empty()) {
        preview.frameConflicts.resize(failed + 1);
        preview.conflict = preview.frameConflicts.back();
    }
    preview.retraction.reset();
    preview.sweep.Nullify();
}
} // namespace bending::cad
