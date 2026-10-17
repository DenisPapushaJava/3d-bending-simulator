#include "bending/cad/StepImporter.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <StepBasic_HArray1OfNamedUnit.hxx>
#include <StepBasic_SiUnitAndLengthUnit.hxx>
#include <StepData_Protocol.hxx>
#include <StepData_StepModel.hxx>
#include <StepData_StepWriter.hxx>
#include <StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx.hxx>
#include <TopoDS_Compound.hxx>
#include <XSControl_Controller.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Pln.hxx>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace bending::cad;

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance, const std::string &name) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
            name + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

struct FixtureDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("bending-step-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    FixtureDirectory() {
        require(std::filesystem::create_directory(path), "Cannot create fixture directory");
    }
    ~FixtureDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

// Writer parameters are global in OCCT 7.6. Fixtures are generated before concurrent import tests.
struct WriteParameter {
    const char *name;
    std::string previous;
    WriteParameter(const char *parameter, const char *value)
        : name(parameter), previous(Interface_Static::CVal(parameter)) {
        require(Interface_Static::SetCVal(name, value), "Cannot set fixture parameter");
    }
    ~WriteParameter() {
        Interface_Static::SetCVal(name, previous.c_str());
    }
};

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    require(!output.fail(), "Cannot write text fixture");
}

enum class UnitFixture { Normal, MissingLength, InvalidLength };

void writeStep(const std::filesystem::path &path, const TopoDS_Shape &shape, const char *unit,
               UnitFixture unitFixture = UnitFixture::Normal) {
    WriteParameter parameter("write.step.unit", unit);
    STEPControl_Writer writer;
    require(writer.Transfer(shape, STEPControl_AsIs) == IFSelect_RetDone,
            "Fixture transfer failed");
    for (int i = 1; i <= writer.Model()->NbEntities(); ++i) {
        const auto &entity = writer.Model()->Value(i);
        if (unitFixture == UnitFixture::MissingLength) {
            if (auto context =
                    Handle(StepGeom_GeomRepContextAndGlobUnitAssCtxAndGlobUncertaintyAssCtx)::
                        DownCast(entity)) {
                Handle(StepBasic_HArray1OfNamedUnit) units =
                    new StepBasic_HArray1OfNamedUnit(1, context->NbUnits() - 1);
                int next = 1;
                for (int j = 1; j <= context->NbUnits(); ++j) {
                    if (!context->UnitsValue(j)->IsKind(
                            STANDARD_TYPE(StepBasic_SiUnitAndLengthUnit))) {
                        units->SetValue(next++, context->UnitsValue(j));
                    }
                }
                require(next == context->NbUnits(), "Fixture must remove one length unit");
                context->SetUnits(units);
            }
        } else if (unitFixture == UnitFixture::InvalidLength) {
            if (auto length = Handle(StepBasic_SiUnitAndLengthUnit)::DownCast(entity)) {
                length->Init(false, StepBasic_spMilli, StepBasic_sunSecond);
            }
        }
    }
    // Use a native filesystem stream for the same Unicode-path coverage as the importer.
    StepData_StepWriter streamWriter(writer.Model());
    streamWriter.SendModel(
        Handle(StepData_Protocol)::DownCast(writer.WS()->NormAdaptor()->Protocol()));
    std::ofstream output(path, std::ios::binary);
    require(streamWriter.Print(output), "Fixture serialization failed");
    output.close();
    require(!output.fail(), "Cannot write STEP fixture");
}

std::unique_ptr<const CadDocument> load(const std::filesystem::path &path) {
    auto result = StepImporter{}.read(path);
    if (const auto *error = std::get_if<StepImportError>(&result)) {
        std::string message = "Import failed: " + error->detail;
        for (const auto &diagnostic : error->diagnostics) {
            message += "\n" + diagnostic.message;
        }
        throw std::runtime_error(message);
    }
    auto document = std::move(std::get<std::unique_ptr<const CadDocument>>(result));
    require(document != nullptr, "Successful import returned a null document");
    return document;
}

StepImportError expectError(const std::filesystem::path &path, StepImportErrorCode code) {
    auto result = StepImporter{}.read(path);
    const auto *error = std::get_if<StepImportError>(&result);
    require(error != nullptr, "Invalid input was accepted: " + path.filename().string());
    std::string detail = "Unexpected error code for " + path.filename().string() + ": " +
                         std::to_string(static_cast<int>(error->code)) + ", " + error->detail;
    for (const auto &diagnostic : error->diagnostics) {
        detail += "\n" + diagnostic.message;
    }
    require(error->code == code, detail);
    require(!error->detail.empty(), "Import error has no diagnostic detail");
    return *error;
}

void checkBox(const CadDocument &document) {
    const auto &metadata = document.metadata();
    constexpr double lengthToleranceMm = 1.0e-6;
    constexpr double volumeToleranceMm3 = 1.0e-4;
    requireNear(metadata.bounds.minimum.x, 10.0, lengthToleranceMm, "minimum X (mm)");
    requireNear(metadata.bounds.minimum.y, -20.0, lengthToleranceMm, "minimum Y (mm)");
    requireNear(metadata.bounds.minimum.z, 5.0, lengthToleranceMm, "minimum Z (mm)");
    requireNear(metadata.bounds.maximum.x, 110.0, lengthToleranceMm, "maximum X (mm)");
    requireNear(metadata.bounds.maximum.y, 40.0, lengthToleranceMm, "maximum Y (mm)");
    requireNear(metadata.bounds.maximum.z, 7.0, lengthToleranceMm, "maximum Z (mm)");
    requireNear(metadata.volumeMm3, 12000.0, volumeToleranceMm3, "volume (mm^3)");
    require(metadata.faceCount == 6, "Box must have six faces");
    require(std::isfinite(metadata.maximumToleranceMm) && metadata.maximumToleranceMm > 0.0,
            "Invalid BRep tolerance metadata");
    require(!metadata.kernelVersion.empty(), "Missing kernel provenance");
    require(metadata.sourcePath.is_absolute(), "Source provenance must be absolute");
    for (const auto &diagnostic : metadata.diagnostics) {
        require(diagnostic.severity != DiagnosticSeverity::Error,
                "Successful import contains an error diagnostic");
    }
}

void testImport(const std::filesystem::path &directory) {
    const auto box = BRepPrimAPI_MakeBox(gp_Pnt(10.0, -20.0, 5.0), 100.0, 60.0, 2.0).Shape();
    const auto unicodeDirectory = directory / std::filesystem::path(u8"Детали с пробелами");
    std::filesystem::create_directory(unicodeDirectory);
    const auto millimetres = unicodeDirectory / std::filesystem::path(u8"Пластина 100 мм.STeP");
    const auto inches = directory / "box-inch.stp";
    const auto metres = directory / "box-metre.step";
    writeStep(millimetres, box, "MM");
    writeStep(inches, box, "INCH");
    writeStep(metres, box, "M");

    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, box);
    builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(200.0, 0.0, 0.0), 5.0, 5.0, 5.0).Shape());
    const auto multiple = directory / "two-solids.step";
    writeStep(multiple, compound, "MM");
    const auto surface = directory / "surface.step";
    writeStep(surface, BRepBuilderAPI_MakeFace(gp_Pln(), 0.0, 10.0, 0.0, 20.0).Shape(), "MM");
    const auto missingUnits = directory / "missing-units.step";
    const auto invalidUnits = directory / "invalid-units.step";
    writeStep(missingUnits, box, "MM", UnitFixture::MissingLength);
    writeStep(invalidUnits, box, "MM", UnitFixture::InvalidLength);
    TopoDS_Compound mixedShape;
    builder.MakeCompound(mixedShape);
    builder.Add(mixedShape, box);
    builder.Add(mixedShape, BRepBuilderAPI_MakeFace(gp_Pln(), 0.0, 10.0, 0.0, 20.0).Shape());
    const auto mixed = directory / "solid-and-surface.step";
    writeStep(mixed, mixedShape, "MM");
    const auto assembly = directory / "single-component-assembly.step";
    {
        WriteParameter assemblyMode("write.step.assembly", "On");
        TopoDS_Compound singleComponent;
        builder.MakeCompound(singleComponent);
        builder.Add(singleComponent, box);
        writeStep(assembly, singleComponent, "MM");
    }

    std::ifstream validInput(millimetres, std::ios::binary);
    std::string brokenEntity{std::istreambuf_iterator<char>(validInput),
                             std::istreambuf_iterator<char>()};
    const auto point = brokenEntity.find("CARTESIAN_POINT(");
    require(point != std::string::npos, "Fixture must contain a Cartesian point");
    const auto name = point + std::string("CARTESIAN_POINT(").size();
    brokenEntity.replace(name, brokenEntity.find(',', name) - name, "42");
    const auto malformed = directory / "malformed-entity.step";
    writeText(malformed, brokenEntity);

    auto first = load(millimetres);
    auto second = load(inches);
    auto third = load(metres);
    checkBox(*first);
    checkBox(*second);
    checkBox(*third);
    require(first->metadata().sourcePath == std::filesystem::absolute(millimetres),
            "Unicode source path was lost");
    require(first->metadata().sourceLengthUnits == std::vector<std::string>{"millimetre"},
            "Missing original millimetre units");
    require(second->metadata().sourceLengthUnits == std::vector<std::string>{"INCH"},
            "Original inch units must not be replaced by target millimetres");
    require(third->metadata().sourceLengthUnits == std::vector<std::string>{"metre"},
            "Missing original metre units");

    const auto empty = directory / "empty.step";
    const auto damaged = directory / "damaged.step";
    const auto noGeometry = directory / "no-geometry.step";
    writeText(empty, "");
    writeText(damaged, "not a STEP file\n");
    writeText(noGeometry,
              "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('fixture'),'2;1');\n"
              "FILE_NAME('empty.step','2026-10-17T00:00:00',(''),(''),'','','');\n"
              "FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));\nENDSEC;\nDATA;\nENDSEC;\nEND-ISO-10303-21;\n");
    expectError(directory / "missing.step", StepImportErrorCode::FileNotFound);
    expectError(directory / "part.sldprt", StepImportErrorCode::UnsupportedFormat);
    expectError(directory / "part.m3d", StepImportErrorCode::UnsupportedFormat);
    expectError(empty, StepImportErrorCode::EmptyFile);
    expectError(damaged, StepImportErrorCode::InvalidStep);
    expectError(noGeometry, StepImportErrorCode::NoGeometry);
    expectError(multiple, StepImportErrorCode::UnsupportedComposition);
    expectError(surface, StepImportErrorCode::UnsupportedComposition);
    expectError(missingUnits, StepImportErrorCode::UnsupportedUnits);
    expectError(invalidUnits, StepImportErrorCode::UnsupportedUnits);
    expectError(mixed, StepImportErrorCode::UnsupportedComposition);
    expectError(assembly, StepImportErrorCode::UnsupportedComposition);
    const auto malformedError = expectError(malformed, StepImportErrorCode::InvalidStep);
    require(!malformedError.diagnostics.empty(), "Lost STEP model diagnostics");
    require(malformedError.diagnostics.front().stage == ImportStage::Read,
            "Incorrect diagnostic stage");
    const auto notFile = directory / "directory.step";
    std::filesystem::create_directory(notFile);
    expectError(notFile, StepImportErrorCode::ReadFailed);

    // Existing documents retain geometry/metadata after other imports fail or complete.
    checkBox(*first);
    second.reset();
    checkBox(*first);
    checkBox(*load(millimetres));

    auto concurrentMm = std::async(std::launch::async, [&] { return load(millimetres); });
    auto concurrentInch = std::async(std::launch::async, [&] { return load(inches); });
    checkBox(*concurrentMm.get());
    checkBox(*concurrentInch.get());
}
} // namespace

int main() {
    try {
        STEPControl_Controller::Init();
        WriteParameter assemblyMode("write.step.assembly", "Off");
        FixtureDirectory fixtures;
        testImport(fixtures.path);
        std::cout << "STEP import: units, metadata, Unicode paths, failures and ownership passed\n";
    } catch (const Standard_Failure &error) {
        std::cerr << "OCCT fixture failure: " << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
