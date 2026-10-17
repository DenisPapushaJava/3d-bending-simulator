#include "BendFixtures.h"

#include <APIHeaderSection_MakeHeader.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Interface_Static.hxx>
#include <STEPControl_Controller.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <StepData_Protocol.hxx>
#include <StepData_StepModel.hxx>
#include <StepData_StepWriter.hxx>
#include <TCollection_HAsciiString.hxx>
#include <XSControl_Controller.hxx>
#include <XSControl_WorkSession.hxx>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void write(const std::filesystem::path &path, const TopoDS_Shape &shape, const char *unit) {
    if (!Interface_Static::SetCVal("write.step.unit", unit)) {
        throw std::runtime_error("Cannot set output unit");
    }
    STEPControl_Writer writer;
    if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) {
        throw std::runtime_error("STEP fixture transfer failed");
    }
    APIHeaderSection_MakeHeader header(writer.Model());
    header.SetName(new TCollection_HAsciiString(path.filename().string().c_str()));
    header.SetTimeStamp(new TCollection_HAsciiString("2026-10-18T00:00:00"));
    header.Apply(writer.Model());
    StepData_StepWriter serializer(writer.Model());
    serializer.SendModel(
        Handle(StepData_Protocol)::DownCast(writer.WS()->NormAdaptor()->Protocol()));
    std::ofstream stream(path, std::ios::binary);
    if (!serializer.Print(stream)) {
        throw std::runtime_error("STEP fixture serialization failed");
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write STEP fixture");
    }
}
} // namespace
int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: step_examples OUTPUT_DIRECTORY\n";
        return 1;
    }
    try {
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        STEPControl_Controller::Init();
        Interface_Static::SetCVal("write.step.assembly", "Off");
        const auto plate = BRepPrimAPI_MakeBox(100.0, 60.0, 2.0).Shape();
        write(output / "plate-mm.step", plate, "MM");
        write(output / "plate-inch.stp", plate, "INCH");
        const auto flange = BRepPrimAPI_MakeBox(gp_Pnt(0, 58, 0), 100.0, 2.0, 40.0).Shape();
        BRepAlgoAPI_Fuse angle(plate, flange);
        if (!angle.IsDone()) {
            throw std::runtime_error("Cannot construct example angle");
        }
        write(output / "angle-mm.step", angle.Shape(), "MM");
        const auto thickPlate = BRepPrimAPI_MakeBox(100.0, 60.0, 6.0).Shape();
        const auto thickFlange = BRepPrimAPI_MakeBox(gp_Pnt(0, 54, 0), 100.0, 6.0, 40.0).Shape();
        BRepAlgoAPI_Fuse thickAngle(thickPlate, thickFlange);
        if (!thickAngle.IsDone()) {
            throw std::runtime_error("Cannot construct 6 mm angle");
        }
        write(output / "angle-6mm.step", thickAngle.Shape(), "MM");
        write(output / "bend-90-6mm.step", bending::fixtures::bentStrip(8.0, 6.0), "MM");
        write(output / "channel-2mm.step", bending::fixtures::channel(), "MM");
        write(output / "trimmed-bend-2mm.step", bending::fixtures::trimmedBend(), "MM");
    } catch (const Standard_Failure &error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
