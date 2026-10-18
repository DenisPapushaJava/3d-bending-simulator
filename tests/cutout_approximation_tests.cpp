#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DocumentAccess.h"
#include "bending/cad/occt/PreviewAccess.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return std::abs(props.Mass());
}
int displayTriangles(const bending::cad::PreviewFrame &frame, const TopoDS_Shape &source,
                     bool endpoint) {
    TopTools_IndexedMapOfShape displayed, original;
    for (const auto &region : frame.displayRegions) {
        TopExp::MapShapes(region, TopAbs_FACE, displayed);
    }
    TopExp::MapShapes(source, TopAbs_FACE, original);
    require(!frame.displayShape.IsNull() && displayed.Extent() == original.Extent(),
            "Visible surface lost cutout walls or added caps");
    int triangles = 0;
    for (int i = 1; i <= displayed.Extent(); ++i) {
        require(!original.Contains(displayed(i)), "Display shares mutable source face");
        TopLoc_Location location;
        const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(displayed(i)), location);
        require(!mesh.IsNull() && mesh->NbTriangles() > 0, "Unmeshed visible cutout");
        triangles += mesh->NbTriangles();
        if (endpoint) {
            for (int n : {1, mesh->NbNodes() / 2 + 1, mesh->NbNodes()}) {
                const auto node = mesh->Node(n).Transformed(location.Transformation());
                BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(node).Vertex(),
                                                    source);
                require(distance.IsDone() && distance.Value() < 1e-5,
                        "Final visible cutout vertex differs from original STEP");
            }
        }
    }
    return triangles;
}
} // namespace
int main() {
    using namespace bending;
    try {
        for (const auto *name : {"bend-zone-hole-6mm.step", "bend-zone-pocket-6mm.step",
                                 "profiled-bend-6mm.step", "channel-zone-hole-2mm.step"}) {
            const auto loaded =
                cad::StepImporter{}.load(std::filesystem::path(BENDING_EXAMPLE_DIR) / name, {}, {});
            const auto doc = application::resultDocument(loaded);
            require(doc != nullptr, "Cannot import cutout fixture");
            const auto &metadata = doc->metadata();
            const auto &cutouts = metadata.closedCutouts;
            std::cout << name << " status=" << static_cast<int>(cutouts.status)
                      << " walls=" << cutouts.boundaryFaces.size()
                      << " B=" << metadata.bendAnalysis.bends.size()
                      << " D=" << metadata.bendAnalysis.interiorContourDiagnostics.size()
                      << " added=" << cutouts.addedVolumeMm3 << std::endl;
            require(cutouts.status == application::ClosedCutoutStatus::Filled &&
                        !cutouts.boundaryFaces.empty(),
                    "Closed cutouts were not filled");
            const auto &native = dynamic_cast<const cad::CadDocument &>(*doc);
            const auto &source = cad::OcctDocumentAccess::shape(native);
            const auto &filled = cad::OcctDocumentAccess::simulationShape(native);
            const double modelVolume = volume(filled);
            require(std::abs(modelVolume - metadata.volumeMm3 - cutouts.addedVolumeMm3) < 1e-4,
                    "Incorrect added volume");
            BRepAlgoAPI_Cut lost(source, filled);
            require(lost.IsDone() && volume(lost.Shape()) < 1e-4,
                    "Simplification lost source material");
            const auto &bend = metadata.bendAnalysis.bends.front();
            const core::BendTarget target{bend.angleRadians, bend.innerRadiusMm};
            const cad::SingleBendPreviewBuilder builder;
            const auto kin = builder.build(doc, 0, target, false, {});
            if (metadata.bendAnalysis.bends.size() > 1) {
                require(std::holds_alternative<application::BendPreviewError>(kin),
                        "Multiple bends were treated as one");
                continue;
            }
            if (const auto *error = std::get_if<application::BendPreviewError>(&kin))
                throw std::runtime_error(error->detail);
            const auto preview = std::get<application::BendPreviewPtr>(kin);
            require(preview->ignoresClosedCutouts(), "Preview did not report approximation");
            int topology = 0;
            for (std::size_t i = 0; i < preview->frameCount(); ++i) {
                require(std::abs(volume(cad::OcctPreviewAccess::frame(*preview, i).shape) -
                                 modelVolume) < 1e-4,
                        "Approximate volume changed during motion");
                const int triangles =
                    displayTriangles(cad::OcctPreviewAccess::frame(*preview, i), source, i == 20);
                if (i == 0) {
                    topology = triangles;
                }
                require(triangles == topology, "Animation changed cutout topology");
            }
            const core::PressTools tools{
                48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
            const auto motion = builder.buildToolMotion(doc, 0, target, tools, {});
            if (const auto *error = std::get_if<application::BendPreviewError>(&motion))
                throw std::runtime_error(error->detail);
            const auto moved = std::get<application::BendPreviewPtr>(motion);
            require(moved->ignoresClosedCutouts() && moved->frameCount() == 61 &&
                        moved->punchRetraction(),
                    "Missing approximate tool cycle/check");
            for (std::size_t i = 0; i < moved->frameCount(); ++i) {
                require(displayTriangles(cad::OcctPreviewAccess::frame(*moved, i), source, false) ==
                            topology,
                        "Tool cycle lost visible holes");
            }
            require(std::abs(volume(source) - metadata.volumeMm3) < 1e-4, "Source CAD changed");
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
