#include "BendFixtures.h"
#include "BendOperation.h"
#include "BendWorkpiece.h"
#include "ToolFixtures.h"
#include "bending/cad/CadDocument.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
using namespace bending;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
application::BendPreviewPtr preview(application::BendPreviewResult r) {
    if (const auto *e = std::get_if<application::BendPreviewError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<application::BendPreviewPtr>(r);
}
cad::PreparedBendOperationPtr prepared(cad::PreparedBendOperationResult r) {
    if (const auto *e = std::get_if<application::BendPreviewError>(&r))
        throw std::runtime_error(e->detail);
    return std::get<cad::PreparedBendOperationPtr>(r);
}
double volume(const TopoDS_Shape &s) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return std::abs(p.Mass());
}
int main() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bend-workpiece-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(folder);
        const auto file = folder / "strip.step";
        STEPControl_Writer writer;
        require(writer.Transfer(fixtures::bentStrip(2, 2, std::numbers::pi / 2, 80, true, 20),
                                STEPControl_AsIs) == IFSelect_RetDone &&
                    writer.Write(file.string().c_str()) == IFSelect_RetDone,
                "Own fixture");
        const auto load = [&]() {
            return application::resultDocument(cad::StepImporter{}.load(file, {}, {}));
        };
        const auto doc = load();
        require(bool(doc), "Import");
        const auto source = std::string(doc->sourceStep());
        const auto &b = doc->metadata().bendAnalysis.bends.at(0);
        const core::BendTarget target{b.angleRadians, b.innerRadiusMm};
        cad::BendWorkpieceReuse reuse;
        const auto make = [&](bool reversed, std::optional<core::ToolAssignment> tools = {},
                              std::stop_token stop = {}) {
            return cad::prepareBendOperation(doc, 0, target, reversed, stop,
                                             tools ? core::toolParameters(*tools) : std::nullopt,
                                             bool(tools), tools, false, 0, {}, {}, &reuse);
        };
        auto first = prepared(make(false));
        const auto cold = preview(first->at(0, {}));
        require(reuse.built == 1 && reuse.checked == 21 && reuse.reusedChecks == 0,
                "Cold intrinsic counters");
        auto again = prepared(make(false));
        const auto warm = preview(again->at(0, {}));
        require(reuse.built == 1 && reuse.reused == 1 && reuse.reusedChecks == 21,
                "Intrinsic reuse missing");
        const auto reference =
            preview(prepared(cad::prepareBendOperation(doc, 0, target, false, {}, {}))->at(0, {}));
        for (auto i : {0U, 10U, 20U}) {
            const auto &a = cad::OcctPreviewAccess::frame(*warm, i).shape;
            const auto &other = cad::OcctPreviewAccess::frame(*reference, i).shape;
            require(!a.IsSame(cad::OcctPreviewAccess::frame(*cold, i).shape),
                    "Cached frames were republished");
            for (bool reverse : {false, true}) {
                BRepAlgoAPI_Cut cut(reverse ? other : a, reverse ? a : other);
                require(cut.IsDone() && !cut.HasErrors() && volume(cut.Shape()) < 1e-5,
                        "Cached/reference intrinsic geometry differs");
            }
        }
        auto tool = tool_fixture::pair(16, .8);
        const auto withTool = preview(prepared(make(false, tool))->at(0, {}));
        require(withTool->frameCount() == 61 && withTool->punchRetraction() && reuse.built == 1,
                "Tool setup rebuilt the part or lost full cycle");
        const auto searchCycle = preview(prepared(make(false, tool))->at(0, {}, false));
        for (std::size_t f = 0; f < searchCycle->frameCount(); ++f) {
            const auto &actual = cad::OcctPreviewAccess::frame(*searchCycle, f);
            const auto &explicitFrame = cad::OcctPreviewAccess::frame(*withTool, f);
            require(actual.displayShape.IsNull() == explicitFrame.displayShape.IsNull() &&
                        actual.orientation.sides[0].size() ==
                            explicitFrame.orientation.sides[0].size() &&
                        actual.orientation.sides[1].size() ==
                            explicitFrame.orientation.sides[1].size(),
                    "Deferred display changed the explicit frame representation or side labels");
        }
        const auto actualBounds = searchCycle->bounds(), explicitBounds = withTool->bounds();
        for (const auto &pair : {std::pair{actualBounds.minimum.x, explicitBounds.minimum.x},
                                 {actualBounds.minimum.y, explicitBounds.minimum.y},
                                 {actualBounds.minimum.z, explicitBounds.minimum.z},
                                 {actualBounds.maximum.x, explicitBounds.maximum.x},
                                 {actualBounds.maximum.y, explicitBounds.maximum.y},
                                 {actualBounds.maximum.z, explicitBounds.maximum.z}})
            require(std::abs(pair.first - pair.second) < 1e-7,
                    "Deferred meshing changed the working bounds");
        const auto hits = reuse.reusedChecks;
        auto blocked = tool_fixture::pair(16, .8, 200);
        const auto collision = prepared(make(false, blocked))->at(0, {});
        const auto *issue = std::get_if<application::BendPreviewError>(&collision);
        require(issue &&
                    (issue->code == application::BendPreviewErrorCode::ToolIntersection ||
                     issue->code == application::BendPreviewErrorCode::RetractionNotProven) &&
                    reuse.reusedChecks > hits,
                "New tool collision was hidden by intrinsic reuse");
        cad::BendWorkpieceReuse compact;
        auto rejected = prepared(cad::prepareBendOperation(doc, 0, target, false, {},
                                                           core::toolParameters(blocked), true,
                                                           blocked, false, 0, {}, {}, &compact));
        require(std::holds_alternative<application::BendPreviewError>(rejected->at(0, {}, false)) &&
                    !compact.entries.front()->display->skin,
                "A rejected search-only cycle prepared an unused display skin");
        const auto diagnostic = rejected->at(0, {}, true);
        require(std::holds_alternative<application::BendPreviewError>(diagnostic) &&
                    compact.entries.front()->display->skin,
                "Explicit diagnostic did not prepare its original display path");
        (void)prepared(make(true));
        require(reuse.built == 2, "Opposite workpiece side reused another preparation");
        const auto native = std::dynamic_pointer_cast<const cad::CadDocument>(doc);
        const auto axis = cad::normalBendAxis(*native, 0);
        for (const auto &direction : {axis, axis.Reversed()})
            (void)prepared(cad::prepareBendOperation(doc, 0, target, false, {}, {}, false, {},
                                                     false, 0, direction, {}, &reuse));
        require(reuse.built == 4, "Different orientation reference bypassed the workpiece key");
        const auto before = reuse.reused;
        const auto invalid = cad::prepareBendOperation(doc, 0, {b.angleRadians, 3}, false, {}, {},
                                                       false, {}, false, 0, {}, {}, &reuse);
        require(std::holds_alternative<application::BendPreviewError>(invalid) &&
                    reuse.reused == before,
                "Invalid target bypassed validation on hit");
        std::stop_source stopping;
        stopping.request_stop();
        require(
            std::get<application::BendPreviewError>(make(false, {}, stopping.get_token())).code ==
                application::BendPreviewErrorCode::Cancelled,
            "Cancelled preparation hit succeeded");
        require(std::get<application::BendPreviewError>(again->at(0, stopping.get_token())).code ==
                    application::BendPreviewErrorCode::Cancelled,
                "Cancelled intrinsic hit succeeded");
        for (int i = 0; i < 5; ++i) {
            const auto different = load();
            (void)prepared(cad::prepareBendOperation(different, 0, target, false, {}, {}, false, {},
                                                     false, 0, {}, {}, &reuse));
        }
        require(reuse.entries.size() == 4 && reuse.built == 9,
                "Preparation memory bound or source identity violated");
        require(doc->sourceStep() == source, "Source STEP changed");
        std::cout << "Workpiece preparation, exact intrinsic checks, independent tool collisions "
                     "and guards passed\n";
        std::filesystem::remove_all(folder);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(folder);
        return 1;
    }
}
