#include "bending/core/BendSequence.h"
#include "bending/core/BendTarget.h"

#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

int main() {
    auto require = [](bool value, const char *message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    };
    try {
        using bending::core::validBendTarget;
        const auto radians = [](double degrees) { return degrees * std::numbers::pi / 180.0; };
        for (double angle : {1.0, 30.0, 90.0, 120.0, 179.0}) {
            require(validBendTarget({radians(angle), 4.0}), "Supported target rejected");
        }
        for (double angle : {0.0, -90.0, 180.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
            require(!validBendTarget({radians(angle), 4.0}), "Invalid target angle accepted");
        }
        for (double radius : {0.0, -1.0, 1.0e6 + 1.0, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
            require(!validBendTarget({radians(90.0), radius}), "Invalid target radius accepted");
        }
        bending::core::BendSequence sequence({"B01", "B03", "B02", "B04"});
        require(sequence.move(2, 1) == 1, "Reordering must invalidate from the first changed step");
        require(sequence.bendIds() == std::vector<std::string>({"B01", "B02", "B03", "B04"}),
                "Stable bend IDs must survive moving earlier");
        require(!sequence.move(1, 1), "An unchanged order must preserve validation results");
        require(sequence.move(1, 3) == 1, "Moving later must return the affected suffix");
        require(sequence.bendIds() == std::vector<std::string>({"B01", "B03", "B04", "B02"}),
                "Moving later must preserve the order of the other bends");
        const auto before = sequence.bendIds();
        bool rejected = false;
        try {
            (void)sequence.move(0, 99);
        } catch (const std::out_of_range &) {
            rejected = true;
        }
        require(rejected && sequence.bendIds() == before, "Invalid move must not mutate the route");
        for (const auto &ids :
             {std::vector<std::string>{"B01", "B01"}, std::vector<std::string>{"B01", ""}}) {
            rejected = false;
            try {
                bending::core::BendSequence invalid(ids);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "Empty and duplicate IDs must be rejected");
        }
        bending::core::BendSequence empty({});
        require(empty.bendIds().empty(), "New project must permit an empty sequence");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
