#include "bending/viewer/ToolSegmentColors.h"
#include <array>
#include <cmath>

namespace bending::viewer {
std::optional<std::int64_t> toolSegmentColorKey(double lengthMm) noexcept {
    if (!std::isfinite(lengthMm) || lengthMm < .001 || lengthMm > 100000)
        return {};
    return std::llround(lengthMm * 1000);
}
QColor toolSegmentColor(double lengthMm, const ToolSegmentColors &overrides) {
    const auto key = toolSegmentColorKey(lengthMm);
    if (!key)
        return QColor("#9AA6B2");
    if (const auto found = overrides.find(*key);
        found != overrides.end() && found->second.isValid()) {
        auto color = found->second;
        color.setAlpha(255);
        return color;
    }
    static constexpr std::array<std::int64_t, 12> lengths{
        10000, 15000, 20000, 40000, 50000, 100000, 200000, 270000, 370000, 415000, 525000, 835000};
    static constexpr std::array<const char *, 12> colors{
        "#8CA2B6", "#A698B3", "#91A58F", "#BEA58C", "#7F9FA0", "#AD94A4",
        "#93A0C0", "#B2AB85", "#8BAA9A", "#BA9C8F", "#9998B0", "#829BB8"};
    for (std::size_t i = 0; i < lengths.size(); ++i)
        if (*key == lengths[i])
            return QColor(colors[i]);
    const auto hash = static_cast<std::uint64_t>(*key) * 11400714819323198485ULL;
    return QColor::fromHsvF(static_cast<double>(hash % 3600) / 3600.,
                            .18 + static_cast<double>((hash >> 16) % 17) / 100.,
                            .62 + static_cast<double>((hash >> 32) % 16) / 100.);
}
} // namespace bending::viewer
