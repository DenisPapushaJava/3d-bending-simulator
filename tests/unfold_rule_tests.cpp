#include "bending/core/UnfoldRule.h"
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
using namespace bending::core;
void check(bool ok) {
    if (!ok)
        throw std::runtime_error("Unfold rule regression");
}
int main() {
    UnfoldSettings s{SheetMaterial::MildSteel, {}, 1};
    check(!unfoldK({}, 2, 2));
    check(unfoldK(s, 2, 2) == .38 && unfoldK(s, 4, 2) == .43 && unfoldK(s, 6, 2) == .5);
    check(unfoldK(s, 2 + 1e-8, 2) == .38 && unfoldK(s, 6 - 1e-8, 2) == .5);
    s.material = SheetMaterial::StainlessSteel;
    check(unfoldK(s, 2, 2) == .40 && unfoldK(s, 4, 2) == .45);
    s.material = SheetMaterial::Aluminium;
    check(unfoldK(s, 2, 2) == .33 && unfoldK(s, 4, 2) == .40);
    s.material = SheetMaterial::Unknown;
    s.manualK = .4;
    check(unfoldK(s, 2, 2) == .4);
    const auto ba = bendAllowance(2, 2, std::numbers::pi / 2, .4);
    check(ba && std::abs(*ba - 4.39822971502571) < 1e-12);
    check(std::abs(*bendAllowance(2, 2, std::numbers::pi / 4, .4) - *ba / 2) < 1e-12);
    check(!bendAllowance(2, 2, std::numbers::pi, .4));
    check(!bendAllowance(2, 0, 1, .4));
    s.manualK = std::numeric_limits<double>::quiet_NaN();
    check(!validUnfoldSettings(s));
    s.manualK = .6;
    check(!validUnfoldSettings(s));
    s.manualK = .4;
    s.referenceVersion = 2;
    check(!validUnfoldSettings(s));
}
