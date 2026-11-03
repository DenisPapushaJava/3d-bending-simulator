#pragma once
#include "bending/core/Backgauge.h"
#include <cstdint>
#include <optional>
#include <string>

namespace bending::application {
// Actual work in this request, not the historical work of a cached preview.
// Collision checks count checker calls with their broad phase/reuse, not Booleans.
struct BackgaugeSearchStatistics {
    std::size_t contactSearches = 0, candidates = 0, exposureChecks = 0;
    std::size_t placementTrials = 0, collisionChecks = 0;
    std::size_t noReferenceTransitions = 0, reusedNoReferenceTransitions = 0;
    std::uint64_t contactTimeUs = 0, collisionTimeUs = 0;
    BackgaugeSearchStatistics &operator+=(const BackgaugeSearchStatistics &s) noexcept {
        contactSearches += s.contactSearches;
        candidates += s.candidates;
        exposureChecks += s.exposureChecks;
        placementTrials += s.placementTrials;
        collisionChecks += s.collisionChecks;
        noReferenceTransitions += s.noReferenceTransitions;
        reusedNoReferenceTransitions += s.reusedNoReferenceTransitions;
        contactTimeUs += s.contactTimeUs;
        collisionTimeUs += s.collisionTimeUs;
        return *this;
    }
};
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
