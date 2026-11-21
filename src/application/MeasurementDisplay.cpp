#include "bending/application/MeasurementDisplay.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

namespace bending::application {
namespace {
std::string decimalText(double value, int decimals, char separator) {
    if (!std::isfinite(value))
        return "—";
    decimals = std::clamp(decimals, 0, maximumPitchDecimalPlaces);
    char buffer[48];
    // Using the shortest round-trip decimal keeps e.g. 1.15 at the decimal
    // midpoint without snapping nearby, representably different values to it.
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::abs(value),
                                      std::chars_format::scientific);
    if (result.ec != std::errc{})
        return "—";
    const std::string_view scientific(buffer, result.ptr);
    const auto e = scientific.find('e');
    if (e == std::string_view::npos)
        return "—";
    auto exponentText = scientific.substr(e + 1);
    if (exponentText.starts_with('+'))
        exponentText.remove_prefix(1);
    int exponent = 0;
    const auto exponentResult =
        std::from_chars(exponentText.data(), exponentText.data() + exponentText.size(), exponent);
    if (exponentResult.ec != std::errc{})
        return "—";

    std::string digits(scientific.substr(0, e));
    std::erase(digits, '.');
    const int keep = exponent + 1 + decimals;
    if (keep < 0) {
        digits.clear();
    } else {
        const auto count = static_cast<std::size_t>(keep);
        bool carry = count < digits.size() && digits[count] >= '5';
        digits.resize(count, '0');
        for (auto it = digits.rbegin(); carry && it != digits.rend(); ++it) {
            if (*it == '9')
                *it = '0';
            else {
                ++*it;
                carry = false;
            }
        }
        if (carry)
            digits.insert(digits.begin(), '1');
    }
    const auto places = static_cast<std::size_t>(decimals);
    if (digits.size() <= places)
        digits.insert(0, places + 1 - digits.size(), '0');
    const bool nonzero = digits.find_first_not_of('0') != std::string::npos;
    if (places)
        digits.insert(digits.size() - places, 1, separator);
    if (value < 0 && nonzero)
        digits.insert(digits.begin(), '-');
    return digits;
}
} // namespace

std::string formatMeasurement(double value, int decimals) {
    auto text = decimalText(value, decimals, ',');
    if (const auto comma = text.find(','); comma != std::string::npos) {
        const auto last = text.find_last_not_of('0');
        text.resize(last == comma ? comma : last + 1);
    }
    return text;
}

double roundedMeasurement(double value, int decimals) {
    const auto text = decimalText(value, decimals, '.');
    double rounded = value;
    if (const auto result = std::from_chars(text.data(), text.data() + text.size(), rounded);
        result.ec != std::errc{})
        return value;
    return rounded;
}
} // namespace bending::application
