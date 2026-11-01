#pragma once
#include <QColor>
#include <cstdint>
#include <map>
#include <optional>

namespace bending::viewer {
// Display preferences keyed by length in 0.001 mm; independent of CAD/project parameters.
using ToolSegmentColors = std::map<std::int64_t, QColor>;
[[nodiscard]] std::optional<std::int64_t> toolSegmentColorKey(double lengthMm) noexcept;
[[nodiscard]] QColor toolSegmentColor(double lengthMm, const ToolSegmentColors &overrides = {});
} // namespace bending::viewer
