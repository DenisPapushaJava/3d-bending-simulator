#include "cad/Kernel.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>

#include <cmath>
#include <iostream>

int main() {
    try {
        const auto shape = BRepPrimAPI_MakeBox(10.0, 20.0, 2.0).Shape();
        GProp_GProps properties;
        BRepGProp::VolumeProperties(shape, properties);
        if (shape.IsNull() || std::abs(properties.Mass() - 400.0) > 1.0e-8 ||
            bending::cad::kernelVersion().empty()) {
            return 1;
        }
    } catch (const Standard_Failure &error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    }
    return 0;
}
