#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace bending::core {

// IDs survive reordering. No Qt, CAD objects or simulated validation results here.
class BendSequence {
  public:
    explicit BendSequence(std::vector<std::string> bendIds);

    [[nodiscard]] const std::vector<std::string> &bendIds() const noexcept;
    // Returns the first affected operation; callers must invalidate results from it onward.
    // An unchanged order returns nullopt. Invalid indices leave the sequence untouched.
    [[nodiscard]] std::optional<std::size_t> move(std::size_t from, std::size_t to);

  private:
    std::vector<std::string> bendIds_;
};

} // namespace bending::core
