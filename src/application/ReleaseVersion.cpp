#include "bending/application/ReleaseVersion.h"
#include <charconv>

namespace bending::application {
std::optional<ReleaseVersion> parseReleaseVersion(std::string_view value) {
    const auto number = [](std::string_view text, std::uint32_t &out) {
        if (text.empty() || (text.size() > 1 && text.front() == '0'))
            return false;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
        return r.ec == std::errc{} && r.ptr == text.data() + text.size();
    };
    ReleaseVersion result;
    const auto dash = value.find('-');
    auto main = value.substr(0, dash);
    const auto first = main.find('.'),
               second = main.find('.', first == main.npos ? first : first + 1);
    if (first != 4 || second == main.npos || !number(main.substr(0, first), result.year) ||
        result.year < 2000 || !number(main.substr(first + 1, second - first - 1), result.release) ||
        !result.release || !number(main.substr(second + 1), result.patch))
        return std::nullopt;
    if (dash != value.npos) {
        const auto suffix = value.substr(dash + 1);
        const auto dot = suffix.find('.');
        if (dot == suffix.npos || !number(suffix.substr(dot + 1), result.preview) ||
            !result.preview)
            return std::nullopt;
        const auto stage = suffix.substr(0, dot);
        if (stage == "alpha")
            result.stage = ReleaseStage::Alpha;
        else if (stage == "beta")
            result.stage = ReleaseStage::Beta;
        else if (stage == "rc")
            result.stage = ReleaseStage::Rc;
        else
            return std::nullopt;
    }
    return result;
}
std::optional<ReleaseVersion> parseReleaseTag(std::string_view value) {
    return value.starts_with('v') ? parseReleaseVersion(value.substr(1)) : std::nullopt;
}
std::string releaseVersionText(const ReleaseVersion &v) {
    auto text =
        std::to_string(v.year) + "." + std::to_string(v.release) + "." + std::to_string(v.patch);
    if (v.stage != ReleaseStage::Stable) {
        text += v.stage == ReleaseStage::Alpha  ? "-alpha."
                : v.stage == ReleaseStage::Beta ? "-beta."
                                                : "-rc.";
        text += std::to_string(v.preview);
    }
    return text;
}
std::string windowsPackageName(const ReleaseVersion &v) {
    return "3d-bending-simulator-" + releaseVersionText(v) + "-windows-x86_64.zip";
}
} // namespace bending::application
