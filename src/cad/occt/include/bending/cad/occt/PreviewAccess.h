#pragma once

#include "bending/application/SingleBendPreview.h"
#include <TopoDS_Shape.hxx>

#include <array>

namespace bending::cad {
struct PreviewFrame {
    TopoDS_Shape shape;
    std::array<TopoDS_Shape, 3> regions; // Fixed flange, bend, moving flange.
};
// Borrowed immutable shapes, independently built/meshed in the worker.
class OcctPreviewAccess final {
  public:
    [[nodiscard]] static const PreviewFrame &frame(const application::SingleBendPreview &preview,
                                                   std::size_t index);
};
} // namespace bending::cad
