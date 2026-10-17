#include "bending/cad/StepImporter.h"

#include "CadDocumentData.h"
#include "bending/cad/Kernel.h"

#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
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
#include <StepRepr_GlobalUnitAssignedContext.hxx>
#include <TColStd_SequenceOfAsciiString.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <XSControl_TransferReader.hxx>
#include <XSControl_WorkSession.hxx>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
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
} // namespace

StepImportResult StepImporter::read(const std::filesystem::path &path) const {
    std::vector<ImportDiagnostic> diagnostics;
    const auto fail = [&](StepImportErrorCode code, std::string detail) -> StepImportResult {
        return StepImportError{code, std::move(detail), std::move(diagnostics)};
    };
    try {
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

        std::lock_guard lock(importMutex);
        STEPControl_Reader reader;
        if (reader.ReadStream("import.step", input) != IFSelect_RetDone || input.bad()) {
            return fail(StepImportErrorCode::InvalidStep, "STEP parser could not read the file");
        }
        if (reader.StepModel().IsNull() || reader.StepModel()->NbEntities() == 0) {
            return fail(StepImportErrorCode::NoGeometry, "STEP contains no model entities");
        }
        appendDiagnostics(reader.WS()->ModelCheckList(), ImportStage::Read, diagnostics);
        if (hasErrors(diagnostics)) {
            return fail(StepImportErrorCode::InvalidStep, "STEP model contains read errors");
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
        const int transferred = reader.TransferRoots();
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
        return std::unique_ptr<const CadDocument>(new CadDocument(std::move(data)));
    } catch (const Standard_Failure &error) {
        return fail(StepImportErrorCode::KernelFailure,
                    error.GetMessageString() ? error.GetMessageString() : "OCCT import failure");
    } catch (const std::filesystem::filesystem_error &error) {
        return fail(StepImportErrorCode::ReadFailed, error.what());
    }
}
} // namespace bending::cad
