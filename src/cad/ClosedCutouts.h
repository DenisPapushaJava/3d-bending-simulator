#pragma once
#include "bending/application/Document.h"
#include <TopoDS_Shape.hxx>
#include <stop_token>

namespace bending::cad {
struct FilledCutouts {
    TopoDS_Shape shape;
    application::ClosedCutouts report;
};
// Separate approximation; never edits source topology. Only enclosed surface wires.
FilledCutouts fillClosedCutouts(const TopoDS_Shape &source, double tolerance, std::stop_token stop);
} // namespace bending::cad
