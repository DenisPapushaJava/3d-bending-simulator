#pragma once

#include "bending/application/Document.h"
#include <cstddef>
#include <optional>
#include <vector>

namespace bending::application {
// Immutable, source-scoped measurements. These are display data, not a process certificate.
struct MeasuredSpan {
    PointMm first{}, second{};
    double valueMm = 0;
    std::vector<std::size_t> faces;
};
struct MeasuredEdge {
    std::size_t id = 0;
    PointMm first{}, second{}, center{};
    double lengthMm = 0;
    std::optional<double> radiusMm;
    bool straight = false;
    bool bendBoundary = false;
    std::vector<PointMm> outline;
    std::vector<std::size_t> faces;
};
struct MeasuredFace {
    std::size_t id = 0;
    PointMm anchor{}, normal{};
    double areaMm2 = 0;
    bool planar = false;
    std::vector<MeasuredSpan> sizes; // Only a verified rectangular outer boundary.
    std::vector<std::size_t> outerEdges;
    std::vector<PointMm> outline;
};
struct MeasuredOpening {
    std::size_t id = 0, hostFace = 0;
    std::vector<std::size_t> hostFaces;
    PointMm center{}, normal{}, major{}, minor{};
    double lengthMm = 0, widthMm = 0;
    bool circular = false;
    std::vector<std::size_t> faces, edges;
    std::vector<MeasuredSpan> locations; // Actual perpendiculars to finite straight base edges.
    std::vector<PointMm> outline;
};
struct ModelMeasurements {
    std::vector<MeasuredSpan> flanges; // Outside dimensions at the recorded local sections.
    std::vector<MeasuredFace> faces;
    std::vector<MeasuredEdge> edges;
    std::vector<MeasuredOpening> openings;
    std::vector<std::size_t> datumEdges; // Flat-stock outer edges; IDs local to this snapshot.
};
} // namespace bending::application
