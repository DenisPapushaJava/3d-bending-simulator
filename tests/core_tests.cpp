#include "bending/core/BendSequence.h"

#include <iostream>
#include <stdexcept>

int main() {
    auto require = [](bool value, const char *message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    };
    try {
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
