#pragma once
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bending::application {
enum class ReleaseStage { Alpha, Beta, Rc, Stable };
struct ReleaseVersion {
    std::uint32_t year = 0, release = 0, patch = 0;
    ReleaseStage stage = ReleaseStage::Stable;
    std::uint32_t preview = 0;
    auto operator<=>(const ReleaseVersion &) const = default;
};
std::optional<ReleaseVersion> parseReleaseVersion(std::string_view value);
std::optional<ReleaseVersion> parseReleaseTag(std::string_view value);
std::string releaseVersionText(const ReleaseVersion &version);
std::string windowsPackageName(const ReleaseVersion &version);
} // namespace bending::application
