#include "core/BendSequence.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace bending {

BendSequence::BendSequence(std::vector<std::string> bendIds) : bendIds_(std::move(bendIds)) {
    std::unordered_set<std::string> seen;
    for (const auto &id : bendIds_) {
        if (id.empty() || !seen.insert(id).second) {
            throw std::invalid_argument("Bend IDs must be non-empty and unique");
        }
    }
}

const std::vector<std::string> &BendSequence::bendIds() const noexcept {
    return bendIds_;
}

std::optional<std::size_t> BendSequence::move(std::size_t from, std::size_t to) {
    if (from >= bendIds_.size() || to >= bendIds_.size()) {
        throw std::out_of_range("Bend index is outside the sequence");
    }
    if (from == to) {
        return std::nullopt;
    }
    // Rotate in-place so no allocation can leave a partially edited sequence.
    auto begin = bendIds_.begin();
    if (from < to) {
        std::rotate(begin + static_cast<std::ptrdiff_t>(from),
                    begin + static_cast<std::ptrdiff_t>(from + 1),
                    begin + static_cast<std::ptrdiff_t>(to + 1));
    } else {
        std::rotate(begin + static_cast<std::ptrdiff_t>(to),
                    begin + static_cast<std::ptrdiff_t>(from),
                    begin + static_cast<std::ptrdiff_t>(from + 1));
    }
    return std::min(from, to);
}

} // namespace bending
