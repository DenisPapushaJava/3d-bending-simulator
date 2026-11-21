#pragma once

#include <cmath>

#include "bending/application/MeasurementDisplay.h"
#include "bending/ui/Preferences.h"

namespace bending::ui {
[[nodiscard]] inline QString measurementText(double value, int decimals) {
    return QString::fromStdString(application::formatMeasurement(value, decimals));
}

[[nodiscard]] inline QString measurementText(double value) {
    return measurementText(value, dimensionDecimalPlaces());
}
[[nodiscard]] inline QString nonzeroMeasurementText(double value) {
    const int decimals = dimensionDecimalPlaces();
    if (value > 0 && application::roundedMeasurement(value, decimals) == 0)
        return QStringLiteral("< %1").arg(measurementText(std::pow(10., -decimals), decimals));
    return measurementText(value, decimals);
}
} // namespace bending::ui
