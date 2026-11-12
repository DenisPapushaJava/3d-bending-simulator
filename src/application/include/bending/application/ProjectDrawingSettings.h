#pragma once

#include "bending/application/BendingDrawing.h"

namespace bending::application {
// These values are stored in project files.
enum class DrawingViewKind {
    Projection = 0,
    Section = 1,
    Profile = 2,
    Detail = 3,
    Auxiliary = 4,
    Flat = 5,
    Isometric = 6
};
enum class DrawingFlatSide { Automatic = 0, First = 1, Opposite = 2 };
enum class DrawingPaperFormat { A4 = 0, A3 = 1 };

struct DrawingViewIdentity {
    DrawingViewKind kind;
    std::size_t index = 0;
    // Canonical source bend groups in the embedded model. The variant separates
    // a repeated unit from its full section and an auxiliary reverse view.
    std::vector<std::vector<std::size_t>> groups = {};
    bool variant = false;
    bool operator==(const DrawingViewIdentity &) const = default;
};

struct ProjectDrawingView {
    DrawingViewIdentity identity;
    bool visible;
    std::optional<double> scale;
    bool operator==(const ProjectDrawingView &) const = default;
};

struct ProjectDrawingSettings {
    std::optional<std::string> name;
    std::string designation, material, execution;
    int quantity = 0;
    std::optional<std::size_t> baseFace;
    int quarterTurns = -1;
    bool reverseView = false, allowBreaks = true;
    DrawingFlatSide flatSide = DrawingFlatSide::Automatic;
    std::vector<DrawingSectionEdit> sectionEdits;
    std::vector<ProjectDrawingView> views;
    DrawingPaperFormat paperFormat = DrawingPaperFormat::A4;
    bool operator==(const ProjectDrawingSettings &) const = default;
};

[[nodiscard]] bool validProjectDrawingSettings(const ProjectDrawingSettings &);
} // namespace bending::application
