#include "bending/cad/StepImporter.h"

#include "BendRecognizer.h"
#include "CadDocumentData.h"
#include "ClosedCutouts.h"
#include "ThicknessRecognizer.h"
#include "bending/application/BendPreparation.h"
#include "bending/cad/Kernel.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Interface_Check.hxx>
#include <Interface_CheckIterator.hxx>
#include <STEPConstruct_UnitContext.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <StepBasic_ConversionBasedUnitAndLengthUnit.hxx>
#include <StepBasic_MeasureWithUnit.hxx>
#include <StepBasic_SiUnit.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepData_StepModel.hxx>
#include <StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx.hxx>
#include <StepGeom_GeometricRepresentationContext.hxx>
#include <StepGeom_GeometricRepresentationContextAndGlobalUnitAssignedContext.hxx>
#include <StepRepr_AssemblyComponentUsage.hxx>
#include <StepRepr_GlobalUnitAssignedContext.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>

namespace bending::cad {
namespace {
std::mutex importMutex;

void appendDiagnostics(Interface_CheckIterator checks, ImportStage stage,
                       std::vector<ImportDiagnostic> &diagnostics) {
    for (checks.Start(); checks.More(); checks.Next()) {
        const auto &check = checks.Value();
        for (int i = 1; i <= check->NbFails(); ++i) {
            diagnostics.push_back(
                {stage, DiagnosticSeverity::Error, checks.Number(), check->CFail(i)});
        }
        for (int i = 1; i <= check->NbWarnings(); ++i) {
            diagnostics.push_back(
                {stage, DiagnosticSeverity::Warning, checks.Number(), check->CWarning(i)});
        }
    }
}

bool hasErrors(const std::vector<ImportDiagnostic> &diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto &diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

bool validLengthDefinition(const Handle(StepBasic_NamedUnit) & unit) {
    if (auto si = Handle(StepBasic_SiUnitAndLengthUnit)::DownCast(unit)) {
        return si->Name() == StepBasic_sunMetre;
    }
    if (auto converted = Handle(StepBasic_ConversionBasedUnitAndLengthUnit)::DownCast(unit)) {
        const auto &factor = converted->ConversionFactor();
        if (factor.IsNull()) {
            return false;
        }
        auto base = Handle(StepBasic_SiUnit)::DownCast(factor->UnitComponent().NamedUnit());
        return !base.IsNull() && base->Name() == StepBasic_sunMetre;
    }
    return true; // Other unit dimensions are checked by OCCT below.
}

bool hasValidLengthUnits(const Handle(StepData_StepModel) & model) {
    bool found = false;
    for (int i = 1; i <= model->NbEntities(); ++i) {
        const auto &entity = model->Value(i);
        Handle(StepRepr_GlobalUnitAssignedContext) context;
        if (auto combined =
                Handle(StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx)::DownCast(
                    entity)) {
            context = combined->GlobalUnitAssignedContext();
        } else if (auto geometric =
                       Handle(StepGeom_GeometricRepresentationContextAndGlobalUnitAssignedContext)::
                           DownCast(entity)) {
            context = geometric->GlobalUnitAssignedContext();
        } else if (!Handle(StepGeom_GeometricRepresentationContext)::DownCast(entity).IsNull()) {
            return false; // Geometric context without assigned units.
        }
        if (context.IsNull()) {
            continue;
        }
        STEPConstruct_UnitContext units;
        // OCCT 7.6's context overload overwrites earlier unit errors with the last status.
        for (int j = 1; j <= context->NbUnits(); ++j) {
            const auto &unit = context->UnitsValue(j);
            if (!validLengthDefinition(unit) || units.ComputeFactors(unit) != 0) {
                return false;
            }
        }
        if (!units.LengthDone() || !std::isfinite(units.LengthFactor()) ||
            units.LengthFactor() <= 0.0) {
            return false;
        }
        found = true;
    }
    return found;
}

// Do not silently discard free surfaces/edges or repeated solid occurrences in a compound.
bool countSolidOccurrences(const TopoDS_Shape &shape, std::size_t &count) {
    if (shape.ShapeType() == TopAbs_SOLID) {
        return ++count == 1;
    }
    if (shape.ShapeType() != TopAbs_COMPOUND && shape.ShapeType() != TopAbs_COMPSOLID) {
        return false;
    }
    bool hasChildren = false;
    for (TopoDS_Iterator child(shape); child.More(); child.Next()) {
        hasChildren = true;
        if (!countSolidOccurrences(child.Value(), count)) {
            return false;
        }
    }
    return hasChildren;
}

double maximumTolerance(const TopoDS_Shape &shape) {
    TopTools_IndexedMapOfShape shapes;
    TopExp::MapShapes(shape, shapes);
    double maximum = 0.0;
    for (int i = 1; i <= shapes.Extent(); ++i) {
        const auto &part = shapes(i);
        double tolerance = 0.0;
        switch (part.ShapeType()) {
        case TopAbs_FACE:
            tolerance = BRep_Tool::Tolerance(TopoDS::Face(part));
            break;
        case TopAbs_EDGE:
            tolerance = BRep_Tool::Tolerance(TopoDS::Edge(part));
            break;
        case TopAbs_VERTEX:
            tolerance = BRep_Tool::Tolerance(TopoDS::Vertex(part));
            break;
        default:
            break;
        }
        if (!std::isfinite(tolerance) || tolerance < 0.0) {
            return -1.0;
        }
        maximum = std::max(maximum, tolerance);
    }
    return maximum;
}

bool validBounds(const BoundsMm &bounds) {
    const auto &min = bounds.minimum;
    const auto &max = bounds.maximum;
    return std::isfinite(min.x) && std::isfinite(min.y) && std::isfinite(min.z) &&
           std::isfinite(max.x) && std::isfinite(max.y) && std::isfinite(max.z) && max.x > min.x &&
           max.y > min.y && max.z > min.z;
}

application::SurfaceKind surfaceKind(GeomAbs_SurfaceType surface) {
    using Kind = application::SurfaceKind;
    switch (surface) {
    case GeomAbs_Plane:
        return Kind::Plane;
    case GeomAbs_Cylinder:
        return Kind::Cylinder;
    case GeomAbs_Cone:
        return Kind::Cone;
    case GeomAbs_Sphere:
        return Kind::Sphere;
    case GeomAbs_Torus:
        return Kind::Torus;
    case GeomAbs_BezierSurface:
        return Kind::Bezier;
    case GeomAbs_BSplineSurface:
        return Kind::BSpline;
    default:
        return Kind::Other;
    }
}
} // namespace

StepImportResult StepImporter::read(const std::filesystem::path &path, std::stop_token stop,
                                    const application::ImportProgress &progress) const {
    std::vector<ImportDiagnostic> diagnostics;
    const auto fail = [&](StepImportErrorCode code, std::string detail) -> StepImportResult {
        return StepImportError{code, std::move(detail), std::move(diagnostics)};
    };
    try {
        if (stop.stop_requested()) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled");
        }
        auto extension = path.extension().u8string();
        for (auto &character : extension) {
            if (character >= u8'A' && character <= u8'Z') {
                character = static_cast<char8_t>(character + (u8'a' - u8'A'));
            }
        }
        if (extension != u8".step" && extension != u8".stp") {
            return fail(StepImportErrorCode::UnsupportedFormat, "Expected a .step or .stp file");
        }
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error) {
            return fail(StepImportErrorCode::FileNotFound, "Source file does not exist");
        }
        if (error || !std::filesystem::is_regular_file(path, error)) {
            return fail(StepImportErrorCode::ReadFailed, "Source is not a readable regular file");
        }
        // Native filesystem paths handle Unicode on Windows without passing a narrow filename to
        // OCCT.
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return fail(StepImportErrorCode::ReadFailed, "Cannot open source file");
        }
        if (input.peek() == std::ifstream::traits_type::eof()) {
            return fail(input.bad() ? StepImportErrorCode::ReadFailed
                                    : StepImportErrorCode::EmptyFile,
                        "Source file contains no readable data");
        }

        std::string source;
        std::array<char, 65536> buffer{};
        while (input) {
            if (stop.stop_requested()) {
                return fail(StepImportErrorCode::Cancelled, "Import cancelled while reading");
            }
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            source.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
            if (source.size() > maximumSourceBytes) {
                return fail(StepImportErrorCode::ReadFailed,
                            "STEP exceeds the 64 MiB source limit");
            }
        }
        if (input.bad()) {
            return fail(StepImportErrorCode::ReadFailed, "Cannot read STEP source");
        }
        return readBytes(std::move(source), path, stop, progress);
    } catch (const std::filesystem::filesystem_error &error) {
        return fail(StepImportErrorCode::ReadFailed, error.what());
    }
}

StepImportResult StepImporter::readBytes(std::string source, const std::filesystem::path &path,
                                         std::stop_token stop,
                                         const application::ImportProgress &progress) const {
    std::vector<ImportDiagnostic> diagnostics;
    const auto fail = [&](StepImportErrorCode code, std::string detail) -> StepImportResult {
        return StepImportError{code, std::move(detail), std::move(diagnostics)};
    };
    const auto report = [&](application::ImportPhase phase) {
        if (progress) {
            progress(phase);
        }
        return stop.stop_requested();
    };
    if (source.empty() || source.size() > maximumSourceBytes) {
        return fail(StepImportErrorCode::ReadFailed, "Empty or oversized STEP source");
    }
    try {
        std::lock_guard lock(importMutex);
        if (report(application::ImportPhase::Reading)) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled");
        }
        std::istringstream input(source);
        STEPControl_Reader reader;
        if (reader.ReadStream("import.step", input) != IFSelect_RetDone || input.bad()) {
            return fail(StepImportErrorCode::InvalidStep, "STEP parser could not read the file");
        }
        if (stop.stop_requested()) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled after reading");
        }
        if (reader.StepModel().IsNull() || reader.StepModel()->NbEntities() == 0) {
            return fail(StepImportErrorCode::NoGeometry, "STEP contains no model entities");
        }
        appendDiagnostics(reader.WS()->ModelCheckList(), ImportStage::Read, diagnostics);
        if (hasErrors(diagnostics)) {
            return fail(StepImportErrorCode::InvalidStep, "STEP model contains read errors");
        }
        for (int i = 1; i <= reader.StepModel()->NbEntities(); ++i) {
            if (reader.StepModel()->Value(i)->IsKind(
                    STANDARD_TYPE(StepRepr_AssemblyComponentUsage))) {
                return fail(
                    StepImportErrorCode::UnsupportedComposition,
                    "STEP assemblies require explicit component selection; import a single part");
            }
        }
        const int roots = reader.NbRootsForTransfer();
        if (roots == 0) {
            return fail(StepImportErrorCode::NoGeometry, "STEP contains no transferable roots");
        }
        TColStd_SequenceOfAsciiString lengths, angles, solidAngles;
        reader.FileUnits(lengths, angles, solidAngles);
        if (lengths.IsEmpty() || !hasValidLengthUnits(reader.StepModel())) {
            return fail(StepImportErrorCode::UnsupportedUnits,
                        "Missing or invalid length units; no implicit millimetre assumption");
        }
        // OCCT expresses this factor in millimetres. This changes this reader's model, not
        // Interface_Static.
        reader.SetSystemLengthUnit(1.0);
        if (report(application::ImportPhase::Transferring)) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled before transfer");
        }
        const int transferred = reader.TransferRoots();
        if (report(application::ImportPhase::Validating)) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled after transfer");
        }
        appendDiagnostics(reader.WS()->TransferReader()->LastCheckList(), ImportStage::Transfer,
                          diagnostics);
        if (transferred != roots || hasErrors(diagnostics)) {
            return fail(StepImportErrorCode::TransferFailed,
                        "STEP roots were not transferred completely");
        }
        auto shape = reader.OneShape();
        if (shape.IsNull()) {
            return fail(StepImportErrorCode::NoGeometry, "Transfer produced no geometry");
        }
        std::size_t solids = 0;
        if (!countSolidOccurrences(shape, solids) || solids != 1) {
            return fail(StepImportErrorCode::UnsupportedComposition,
                        "Expected exactly one solid without extra surfaces or wires");
        }
        if (!BRepCheck_Analyzer(shape).IsValid()) {
            return fail(StepImportErrorCode::InvalidGeometry, "Transferred BRep is not valid");
        }
        Bnd_Box box;
        BRepBndLib::AddOptimal(shape, box, false, false);
        if (box.IsVoid() || box.IsWhole() || box.IsOpen()) {
            return fail(StepImportErrorCode::InvalidGeometry, "Geometry has no finite bounds");
        }
        BoundsMm bounds{};
        box.Get(bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, bounds.maximum.x,
                bounds.maximum.y, bounds.maximum.z);
        GProp_GProps properties;
        BRepGProp::VolumeProperties(shape, properties, true);
        const double volume = properties.Mass();
        const double tolerance = maximumTolerance(shape);
        if (!validBounds(bounds) || !std::isfinite(volume) || volume <= 0.0 || tolerance < 0.0) {
            return fail(StepImportErrorCode::InvalidGeometry,
                        "Geometry has invalid bounds, volume or tolerances");
        }
        TopTools_IndexedMapOfShape faces;
        TopExp::MapShapes(shape, TopAbs_FACE, faces);
        DocumentMetadata metadata{std::filesystem::absolute(path).lexically_normal(),
                                  {},
                                  std::string(kernelVersion()),
                                  bounds,
                                  volume,
                                  tolerance,
                                  static_cast<std::size_t>(faces.Extent()),
                                  std::move(diagnostics)};
        for (int i = 1; i <= lengths.Length(); ++i) {
            metadata.sourceLengthUnits.emplace_back(lengths(i).ToCString());
        }
        auto data = std::make_unique<CadDocument::Impl>();
        data->shape = std::move(shape);
        data->metadata = std::move(metadata);
        data->sourceStep = std::move(source);
        if (report(application::ImportPhase::Analyzing)) {
            return fail(StepImportErrorCode::Cancelled,
                        "Import cancelled before thickness analysis");
        }
        data->metadata.thickness = recognizeThickness(data->shape, tolerance, stop);
        data->metadata.bendAnalysis = recognizeBends(data->shape, tolerance, stop);
        auto filled =
            fillClosedCutouts(data->shape, data->metadata.bendAnalysis.lengthToleranceMm, stop);
        data->metadata.closedCutouts = filled.report;
        if (filled.report.status == application::ClosedCutoutStatus::Filled) {
            auto analysis = recognizeBends(filled.shape, tolerance, stop);
            auto originals = data->metadata.bendAnalysis.bends;
            for (const auto &diagnostic : data->metadata.bendAnalysis.interiorContourDiagnostics) {
                originals.push_back(diagnostic.candidate);
            }
            std::vector<application::BendPreparation> anchors;
            for (const auto &bend : originals) {
                anchors.push_back({"source-" + std::to_string(anchors.size()),
                                   {bend.axisStart, bend.axisEnd, bend.angleRadians,
                                    bend.innerRadiusMm, bend.thicknessMm, bend.lengthMm,
                                    bend.trimmedEnds, bend.minimumStraightLengthMm},
                                   application::BendReview::Pending,
                                   std::nullopt,
                                   std::nullopt});
            }
            const auto bindings = application::restoreBendPreparations(analysis, anchors);
            bool matched = analysis.status == application::BendRecognitionStatus::Completed &&
                           analysis.interiorContourDiagnostics.empty() &&
                           analysis.bends.size() == originals.size() &&
                           anchors.size() == originals.size() && bindings.unresolved.empty();
            std::vector<application::RecognizedBend> ordered(originals.size());
            for (std::size_t i = 0; matched && i < analysis.bends.size(); ++i) {
                if (!bindings.byRecognizedBend[i] ||
                    *bindings.byRecognizedBend[i] >= ordered.size()) {
                    matched = false;
                } else {
                    ordered[*bindings.byRecognizedBend[i]] = analysis.bends[i];
                }
            }
            if (matched) {
                data->metadata.bendAnalysis.bends = std::move(originals);
                analysis.bends = std::move(ordered);
                data->filledBends = std::move(analysis);
                data->filledShape = std::move(filled.shape);
            } else {
                data->metadata.closedCutouts.status = application::ClosedCutoutStatus::Unsupported;
                data->metadata.closedCutouts.addedVolumeMm3 = 0;
            }
        }
        if (report(application::ImportPhase::Preparing)) {
            return fail(StepImportErrorCode::Cancelled,
                        "Import cancelled before presentation preparation");
        }
        for (int i = 1; i <= faces.Extent(); ++i) {
            if (stop.stop_requested()) {
                return fail(StepImportErrorCode::Cancelled, "Import cancelled during preparation");
            }
            const auto &face = TopoDS::Face(faces(i));
            GProp_GProps area;
            BRepGProp::SurfaceProperties(face, area);
            if (!std::isfinite(area.Mass()) || area.Mass() < 0.0) {
                return fail(StepImportErrorCode::InvalidGeometry, "Invalid face area");
            }
            data->faces.push_back({static_cast<std::size_t>(i), area.Mass(),
                                   surfaceKind(BRepAdaptor_Surface(face).GetType())});
        }
        const auto &minimum = bounds.minimum;
        const auto &maximum = bounds.maximum;
        const double diagonal =
            std::hypot(maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z);
        data->metadata.meshDeflectionMm = std::clamp(diagonal * 0.001, 0.02, 1.0);
        data->metadata.meshAngleRadians = 0.5;
        BRepMesh_IncrementalMesh mesh(data->shape, data->metadata.meshDeflectionMm, false,
                                      data->metadata.meshAngleRadians, false);
        if (stop.stop_requested()) {
            return fail(StepImportErrorCode::Cancelled, "Import cancelled after meshing");
        }
        if (!mesh.IsDone()) {
            return fail(StepImportErrorCode::PreparationFailed, "CAD triangulation failed");
        }
        for (int i = 1; i <= faces.Extent(); ++i) {
            TopLoc_Location location;
            const auto triangulation = BRep_Tool::Triangulation(TopoDS::Face(faces(i)), location);
            if (triangulation.IsNull() || triangulation->NbTriangles() == 0) {
                return fail(StepImportErrorCode::PreparationFailed,
                            "A face has no display triangulation");
            }
        }
        return std::unique_ptr<const CadDocument>(new CadDocument(std::move(data)));
    } catch (const Standard_Failure &error) {
        return fail(StepImportErrorCode::KernelFailure,
                    error.GetMessageString() ? error.GetMessageString() : "OCCT import failure");
    } catch (const std::filesystem::filesystem_error &error) {
        return fail(StepImportErrorCode::ReadFailed, error.what());
    }
}

application::ImportResult StepImporter::load(const std::filesystem::path &path,
                                             std::stop_token stop,
                                             const application::ImportProgress &progress) const {
    auto result = read(path, stop, progress);
    if (auto *error = std::get_if<StepImportError>(&result)) {
        return std::move(*error);
    }
    return application::DocumentPtr(
        std::move(std::get<std::unique_ptr<const CadDocument>>(result)));
}
} // namespace bending::cad
