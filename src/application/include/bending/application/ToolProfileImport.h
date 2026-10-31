#pragma once
#include "bending/core/ToolCatalog.h"
#include <filesystem>
#include <stop_token>
#include <variant>

namespace bending::application {
struct ToolProfileContour {
    std::string layer;
    core::ToolDrawing drawing; // Source XY coordinates; units are not yet confirmed.
};
struct ToolProfileFile {
    std::optional<double> millimetersPerUnit;
    std::vector<ToolProfileContour> contours;
    std::vector<std::string> notices;
};
struct CheckedToolProfile {};
struct ToolProfileError {
    std::string message;
};
using ToolProfileInput = std::variant<std::filesystem::path, core::ToolDrawing>;
using ToolProfileResult = std::variant<ToolProfileFile, CheckedToolProfile, ToolProfileError>;
// File decoding and final solid validation run on the shared CAD worker.
// Validation proves a constant section, not supported contact or load capacity.
class ToolProfileImporter {
  public:
    virtual ~ToolProfileImporter() = default;
    [[nodiscard]] virtual ToolProfileResult run(const ToolProfileInput &,
                                                std::stop_token) const = 0;
};
} // namespace bending::application
