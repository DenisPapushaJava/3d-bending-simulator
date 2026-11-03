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

        // Completing a DXF-only entry with actual straight stock needs no repeated
        // nominal input. It must not mutate a loaded record or a captured assignment.
        auto inventoryOnly = pair.punch;
        inventoryOnly.lengthMm = 0;
        inventoryOnly.parametersConfirmed = false;
        inventoryOnly.segments = std::vector<core::ToolSegment>{
            {835, 3}, {100, 1}, {2000, 0}, {900, 1, core::SegmentShape::LeftHorn}};
        application::ToolLibrarySnapshot pending{{{inventoryOnly}, {}}, "pending"};
        bool rejectWrite = false;
        application::ToolLibraryStorage completionStorage{
            [&](const auto &) -> application::ToolLibraryResult { return pending; },
            [&](const auto &, const auto &next, const auto &) -> application::ToolLibraryResult {
                if (rejectWrite)
                    return std::string("no write");
                pending = {next, "completed"};
                return pending;
            }};
        application::ToolLibrary completion({inventoryOnly}, "memory", completionStorage);
        require(completion.data().tools.front() == inventoryOnly,
                "opening automatically completed the record");
        rejectWrite = true;
        require(!completion.upsert(inventoryOnly).empty() &&
                    completion.data().tools.front() == inventoryOnly,
                "failed completion published data");
        rejectWrite = false;
        require(completion.upsert(inventoryOnly).empty(), "stock completion failed");
        const auto ready = completion.data().tools.front();
        require(ready.parametersConfirmed && ready.lengthMm == 835 &&
                    ready.segments == inventoryOnly.segments &&
                    ready.drawing == inventoryOnly.drawing,
                "available stock did not complete the supported profile");
        require(!inventoryOnly.parametersConfirmed && inventoryOnly.lengthMm == 0,
                "completion changed the captured source");
        auto incomplete = inventoryOnly;
        incomplete.segments = std::vector<core::ToolSegment>{{835, 0}};
        require(completion.upsert(incomplete).empty() &&
                    !completion.data().tools.front().parametersConfirmed &&
                    completion.data().tools.front().lengthMm == 0,
                "zero template stock invented a nominal length");
        incomplete.segments =
            std::vector<core::ToolSegment>{{835, 1, core::SegmentShape::LeftHorn}};
        require(completion.upsert(incomplete).empty() &&
                    !completion.data().tools.front().parametersConfirmed,
                "unsupported horn-only stock completed the tool");
        incomplete.segments.reset();
        incomplete.lengthMm = 415;
        require(completion.upsert(incomplete).empty() &&
                    completion.data().tools.front().parametersConfirmed &&
                    completion.data().tools.front().lengthMm == 415,
                "explicit nominal length did not refresh readiness");
        incomplete.noseRadiusMm = 3;
        require(completion.upsert(incomplete).empty() &&
                    !completion.data().tools.front().parametersConfirmed,
                "wrong working parameters promoted without matching the DXF");
        incomplete = inventoryOnly;
        incomplete.drawing = core::rotateToolDrawing(*incomplete.drawing, std::numbers::pi / 2);
        require(completion.upsert(incomplete).empty() &&
                    !completion.data().tools.front().parametersConfirmed,
                "unsupported working orientation promoted");
        auto matrix = tool_fixture::fourWayDie();
        matrix.parametersConfirmed = false;
        matrix.lengthMm = 0;
        matrix.segments = std::vector<core::ToolSegment>{{415, 2}};
        require(completion.upsert(matrix).empty(), "matrix completion failed");
        const auto &completeMatrix = completion.data().tools.back();
        require(completeMatrix.parametersConfirmed && completeMatrix.lengthMm == 415 &&
                    completeMatrix.openingsMm == matrix.openingsMm &&
                    completeMatrix.grooveSettings == matrix.grooveSettings,
                "multi-groove completion changed working sides");

        auto reference = pair.punch;
        reference.segments = std::vector<core::ToolSegment>{{835, 9}, {415, 4}};
        application::ToolLibrarySnapshot firstRun{{}, "missing", false};
        int seedWrites = 0;
        bool seedFailure = false;
        application::ToolLibraryStorage seedStorage{
            [&](const auto &) -> application::ToolLibraryResult { return firstRun; },
            [&](const auto &, const auto &next,
                const auto &revision) -> application::ToolLibraryResult {
                ++seedWrites;
                if (seedFailure || revision != firstRun.revision)
                    return std::string("seed conflict or write failure");
                firstRun = {next, "seeded" + std::to_string(seedWrites)};
                return firstRun;
            }};
        application::ToolLibrary seeded({}, "new", seedStorage, {reference});
        require(seeded.error().empty() && seedWrites == 1 && firstRun.exists &&
                    seeded.data().tools.size() == 1 &&
                    seeded.data().tools[0].segments == reference.segments &&
                    seeded.data().tools[0].drawing == reference.drawing,
                "First launch did not atomically seed the profile and starting sections");
        auto renamedReference = seeded.data().tools[0];
        renamedReference.code = "My name";
        renamedReference.id = "different-id";
        require(seeded.usesStarterSections(renamedReference),
                "Renaming hid the starting section set");
        renamedReference.segments.reset();
        require(!seeded.usesStarterSections(renamedReference),
                "Unknown stock was marked as a starting set");
        const auto rows = seeded.sectionTemplate(renamedReference);
        require(rows.size() == 2 && rows[0].quantity == 0 && rows[1].quantity == 0,
                "Reference sections are stock or depend on a mutable name/ID");
        auto changedProfile = renamedReference;
        changedProfile.drawing = core::rotateToolDrawing(*changedProfile.drawing, std::numbers::pi);
        require(seeded.sectionTemplate(changedProfile).empty(),
                "A changed profile inherited reference lengths by hash alone");
        changedProfile.segments = reference.segments;
        require(!seeded.usesStarterSections(changedProfile),
                "A changed profile inherited the starting-set hint by hash alone");
        renamedReference.segments = std::vector<core::ToolSegment>{{50, 2}};
        require(seeded.sectionTemplate(renamedReference).empty() &&
                    !seeded.usesStarterSections(renamedReference),
                "Known stock was replaced with a starter template");
        application::ToolLibrary seededRestart(firstRun.data.tools, "new", seedStorage,
                                               {reference, pair.die});
        require(seedWrites == 1 && seededRestart.data().tools == firstRun.data.tools,
                "An update silently merged starter entries into the workshop");
        application::ToolLibrary lostSettings({}, "new", seedStorage, {reference, pair.die});
        require(seedWrites == 1 && lostSettings.data().tools == firstRun.data.tools,
                "Missing preferences hid an existing workshop");
        require(seededRestart.archive(reference.id).empty(), "seed archive failed");
        application::ToolLibrary afterRemoval({}, "new", seedStorage, {reference});
        require(afterRemoval.data().tools.empty() && afterRemoval.data().archived.size() == 1 &&
                    seedWrites == 2,
                "Startup resurrected a removed profile");
        require(afterRemoval.purge(reference.id).empty(), "seed purge failed");
        application::ToolLibrary intentionallyEmpty({}, "new", seedStorage, {reference});
        require(intentionallyEmpty.data().tools.empty() && seedWrites == 3,
                "An intentionally empty existing library was reseeded");
        firstRun = {{}, "missing", false};
        seedFailure = true;
        application::ToolLibrary denied({}, "new", seedStorage, {reference});
        require(!denied.error().empty() && denied.data().tools.empty() && !firstRun.exists,
                "Failed seed write published an in-memory library");
        seedFailure = false;
        application::ToolLibrary external({pair.die}, "new", seedStorage, {reference});
        require(!firstRun.exists && external.data().tools == core::ToolCatalog{pair.die},
                "Starter replaced an explicit external catalogue");
        auto unsupportedReference = reference;
        unsupportedReference.drawing.reset();
        const int beforeInvalid = seedWrites;
        application::ToolLibrary invalidStarter({}, "new", seedStorage, {unsupportedReference});
        require(!invalidStarter.error().empty() && seedWrites == beforeInvalid,
                "A parameter-only starter reached storage");
        application::ToolLibrary unreadableSeed(
            {}, "bad",
            {[](const auto &) -> application::ToolLibraryResult { return std::string("bad"); },
             seedStorage.write},
            {reference});
        require(!unreadableSeed.error().empty() && seedWrites == beforeInvalid,
                "An unreadable workshop was overwritten by the starter");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
