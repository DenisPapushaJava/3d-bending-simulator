#include "ToolFixtures.h"
#include "bending/application/ToolLibrary.h"
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
} // namespace
int main() {
    try {
        const auto pair = tool_fixture::pair();
        application::ToolLibrarySnapshot disk{{{pair.punch, pair.die}, {}}, "r0"};
        bool fail = false;
        int writes = 0;
        application::ToolLibraryStorage storage{
            [&](const auto &) -> application::ToolLibraryResult { return disk; },
            [&](const auto &, const auto &next,
                const auto &revision) -> application::ToolLibraryResult {
                ++writes;
                if (fail)
                    return std::string("write failed");
                if (revision != disk.revision)
                    return std::string("stale");
                disk = {next, "r" + std::to_string(writes)};
                return disk;
            }};
        application::ToolLibrary library(disk.data.tools, "memory", storage);
        const auto snapshot = pair;
        auto renamed = pair.punch;
        renamed.code = "Renamed";
        require(library.upsert(renamed).empty() && writes == 1, "rename failed");
        require(snapshot.punch.code == pair.punch.code, "assignment mutated");
        require(library.upsert(renamed).empty() && writes == 1, "unchanged data rewritten");
        fail = true;
        require(!library.archive(pair.die.id).empty(), "write failure hidden");
        require(library.data().tools.size() == 2 && library.data().archived.empty(),
                "failed delete published");
        fail = false;
        require(library.archive(pair.die.id).empty(), "archive failed");
        require(library.data().tools.size() == 1 && library.data().archived.front() == pair.die,
                "archive lost body or stock");
        application::ToolLibrary restarted(disk.data.tools, "memory", storage);
        require(restarted.restore(pair.die.id).empty() && restarted.data().tools.back() == pair.die,
                "restore after restart failed");
        require(!library.restore(pair.die.id).empty(), "stale owner overwrote newer state");
        require(!restarted.replace({pair.punch, pair.punch}).empty(), "duplicate IDs accepted");
        require(restarted.archive(pair.die.id).empty() && restarted.purge(pair.die.id).empty(),
                "purge failed");
        require(restarted.data().archived.empty(), "purged item retained");
        application::ToolLibrary bad({pair.punch}, "memory",
                                     {[](const auto &) -> application::ToolLibraryResult {
                                          return std::string("bad input");
                                      },
                                      storage.write});
        require(!bad.upsert(renamed).empty() && bad.data().tools.front() == pair.punch,
                "unreadable library overwritten");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
