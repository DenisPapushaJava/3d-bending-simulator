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
        "#E3A8CA", "#519C53", "#474687", "#66A7C4", "#9C5185", "#C49766",
        "#8FDCE3", "#B87CC4", "#E3E28F", "#6670C4", "#8FC466", "#876F46"};
    for (std::size_t i = 0; i < lengths.size(); ++i)
        if (*key == lengths[i])
            return QColor(colors[i]);
    // Spread length keys around the hue circle, leaving the red band for collisions.
    constexpr std::int64_t turn = 360000;
    constexpr std::int64_t goldenAngle = 137508;
    const auto phase = (*key * goldenAngle) % turn;
    const double hue = 30. + 300. * static_cast<double>(phase) / turn;
    return QColor::fromHsvF(hue / 360., .32, .70);
}
} // namespace bending::viewer
