#include "bending/application/ReleaseVersion.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace bending::application;
int main() {
    try {
        const std::vector<std::string> ordered{
            "2026.1.0-alpha.1", "2026.1.0-alpha.2", "2026.1.0-alpha.10",
            "2026.1.0-beta.1",  "2026.1.0-rc.1",    "2026.1.0",
            "2026.1.1-alpha.1", "2026.2.0",         "2027.1.0"};
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            const auto version = parseReleaseVersion(ordered[i]);
            if (!version || releaseVersionText(*version) != ordered[i] ||
                parseReleaseTag("v" + ordered[i]) != version ||
                (i && !(*parseReleaseVersion(ordered[i - 1]) < *version)))
                throw std::runtime_error("Incorrect numeric release ordering");
        }
        for (const auto *invalid :
             {"2026.1", "2026.1.0-alpha", "2026.1.0-alpha.0", "2026.1.0-alpha.01", "2026.01.0",
              "2026.0.0", "v2026.1.0", "2026.1.0 ", "2026.1.0-dev.1", "2026.1.0+local",
              "2026.1.4294967296", "../2026.1.0"})
            if (parseReleaseVersion(invalid))
                throw std::runtime_error("Invalid release version accepted");
        if (parseReleaseTag("sdk-2026.1.0") || parseReleaseTag("2026.1.0"))
            throw std::runtime_error("SDK/non-release tag accepted");
        if (windowsPackageName(*parseReleaseVersion("2026.1.0-alpha.2")) !=
            "3d-bending-simulator-2026.1.0-alpha.2-windows-x86_64.zip")
            throw std::runtime_error("Wrong package identity");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
