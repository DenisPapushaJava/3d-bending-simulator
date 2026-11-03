#pragma once
#include "bending/core/Backgauge.h"
#include <optional>
#include <string>

namespace bending::application {
enum class BackgaugeStatus { NoReference, Checked, Collision };
struct BackgaugeReport {
    core::BackgaugeSetup setup;
    BackgaugeStatus status = BackgaugeStatus::NoReference;
    std::optional<core::BackgaugePlacement> placement = std::nullopt;
    std::size_t checkedPositions = 0;
    std::size_t addedRetractionPositions = 0;
    std::string detail = {};
};
} // namespace bending::application
