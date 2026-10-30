#include "BendFixtures.h"
#include "ToolFixtures.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/cad/StepImporter.h"
#include "bending/cad/occt/DrawingTools.h"
#include "bending/cad/occt/MachineAssembly.h"
#include "bending/cad/occt/ToolGeometryCacheAccess.h"
#include "bending/project/GeometryCacheFiles.h"
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <STEPControl_Writer.hxx>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace bending;
core::ToolAssignment segmentedTools() {
    auto a = tool_fixture::tangPair();
    std::vector<core::ToolSegment> stock;
    for (double length : {10., 20., 40., 50., 100., 200.})
        stock.push_back({length, 1, core::SegmentShape::Straight});
    a.punch.segments = a.die.segments = stock;
    a.punchAssembly = a.dieAssembly = core::ToolAssembly{{5, 4, 3, 2, 1, 0}, 1};
    a.punch.lengthMm = a.die.lengthMm = 425;
    return a;
}
std::array<TopoDS_Shape, 2> prepareAssets(core::ToolAssignment a, double tip = 3) {
    const auto g = *core::drawingToolGeometry(a);
    const auto parameters = *core::toolParameters(a);
    auto bodies = cad::makeDrawingTools(a, g, tip);
    std::array<std::vector<cad::ToolDisplaySegment>, 2> display;
    bodies[0] = cad::segmentToolBody(bodies[0], a.die, a.dieAssembly, 0, {}, &display[0]);
    bodies[1] = cad::segmentToolBody(bodies[1], a.punch, a.punchAssembly, 0, {}, &display[1]);
    if (display[0].size() != 6 || display[1].size() != 6)
        throw std::runtime_error("Cached section metadata lost its ordered children");
    bodies[1] = cad::orientPunch(bodies[1], parameters);
    cad::orientPunchSegments(display[1], parameters);
    cad::MachineAssembly machine(parameters, bodies, {}, a);
    if (machine.at(0).empty())
        throw std::runtime_error("Missing machine");
    return bodies;
}
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path nativePath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
double volume(const TopoDS_Shape &shape) {
    GProp_GProps p;
    BRepGProp::VolumeProperties(shape, p);
    return std::abs(p.Mass());
}
void same(const TopoDS_Shape &a, const TopoDS_Shape &b) {
    require(BRepCheck_Analyzer(a).IsValid() && BRepCheck_Analyzer(b).IsValid(), "Invalid body");
    require(std::abs(volume(a) - volume(b)) < 1.e-5, "Cache changed volume");
    for (auto pair : {std::pair{a, b}, std::pair{b, a}}) {
        BRepAlgoAPI_Cut cut;
        TopTools_ListOfShape args, tools;
        args.Append(pair.first);
        tools.Append(pair.second);
        cut.SetArguments(args);
        cut.SetTools(tools);
        cut.SetNonDestructive(true);
        cut.Build();
        require(cut.IsDone() && !cut.HasErrors() && volume(cut.Shape()) < 1.e-5,
                "Cache changed occupied geometry");
    }
}
double run(const std::shared_ptr<cad::ToolGeometryCache> &cache, int count) {
    cad::GeometryCacheScope scope(cache, {});
    const auto start = std::chrono::steady_clock::now();
    auto a = segmentedTools();
    for (int i = 0; i < count; ++i) {
        a.punchReversed = (i % 2) != 0;
        const auto shapes = prepareAssets(a);
        require(volume(shapes[0]) > 0 && volume(shapes[1]) > 0, "Empty tool");
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}
void print(const char *label, double ms, const std::shared_ptr<cad::ToolGeometryCache> &cache) {
    const auto stats = cache ? cache->statistics() : cad::ToolGeometryCache::Statistics{};
    std::cout << label << " ms=" << ms << " builds=" << stats.builds
              << " ram_hits=" << stats.memoryHits << " disk_hits=" << stats.diskHits
              << " writes=" << stats.writes << " build_ms=" << stats.buildMs
              << " load_ms=" << stats.loadMs << " save_ms=" << stats.saveMs << '\n';
}
void fullCycle(const std::shared_ptr<cad::ToolGeometryCache> &cache, const QString &folder) {
    const auto load = [&](const char *name, double thickness) {
        const auto path = nativePath(folder) / name;
        STEPControl_Writer writer;
        require(
            writer.Transfer(fixtures::bentStrip(2, thickness, std::numbers::pi / 2, 40, true, 20),
                            STEPControl_AsIs) == IFSelect_RetDone &&
                writer.Write(path.string().c_str()) == IFSelect_RetDone,
            "Cannot write part");
        const auto doc = application::resultDocument(cad::StepImporter{}.load(path, {}, {}));
        require(doc && doc->metadata().bendAnalysis.bends.size() == 1, "Cannot import part");
        return doc;
    };
    const auto doc = load("part.step", 2);
    const auto assignment = tool_fixture::pair(16);
    const auto build = [&](const std::shared_ptr<cad::ToolGeometryCache> &c, const char *label) {
        const auto start = std::chrono::steady_clock::now();
        auto result = cad::SingleBendPreviewBuilder(c).buildAssignedToolMotion(
            doc, 0, {std::numbers::pi / 2, 2}, assignment, {});
        print(label,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count(),
              c);
        if (const auto *error = std::get_if<application::BendPreviewError>(&result))
            throw std::runtime_error(error->detail);
        return std::get<application::BendPreviewPtr>(result);
    };
    const auto reference = build({}, "cycle_disabled");
    const auto cold = build(cache, "cycle_cold");
    const auto before = cache->statistics().builds;
    const auto warm = build(cache, "cycle_warm");
    require(cache->statistics().builds == before, "Repeated cycle rebuilt tool/sweep");
    auto restarted = std::make_shared<cad::ToolGeometryCache>(
        std::make_shared<project::GeometryCacheFiles>(nativePath(folder) / "cycle-cache"));
    // The caller uses this same directory for the first process's cache.
    const auto disk = build(restarted, "cycle_disk_reload");
    require(restarted->statistics().builds == 0 && restarted->statistics().diskHits > 0,
            "Cycle could not load persisted tools/sweep");
    for (const auto &candidate : {cold, warm, disk}) {
        require(candidate->frameCount() == reference->frameCount() &&
                    candidate->frameCount() == 61 && candidate->punchRetraction(),
                "Cycle scope changed");
        for (std::size_t i = 0; i < reference->frameCount(); ++i) {
            require(candidate->collisionAtFrame(i) == reference->collisionAtFrame(i) &&
                        candidate->machineCollisionAtFrame(i).has_value() ==
                            reference->machineCollisionAtFrame(i).has_value(),
                    "Collision result changed");
            const auto &a = cad::OcctPreviewAccess::frame(*reference, i);
            const auto &b = cad::OcctPreviewAccess::frame(*candidate, i);
            require(std::abs(volume(a.shape) - volume(b.shape)) < 1.e-5, "Frame volume changed");
            if (i == 0 || i == 30 || i == 60)
                for (std::size_t j = 0; j < 2; ++j)
                    same(a.tools[j], b.tools[j]);
        }
    }
    const auto other = load("other-thickness.step", 3);
    const auto result = cad::SingleBendPreviewBuilder(cache).buildAssignedToolSetup(
        other, 0, {std::numbers::pi / 2, 2}, assignment, {});
    require(std::holds_alternative<application::BendPreviewPtr>(result) &&
                cache->statistics().builds == before,
            "New part/thickness rebuilt local tools");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        if (args.size() >= 3 && args[1] == "--child") {
            auto store = std::make_shared<project::GeometryCacheFiles>(nativePath(args[2]));
            auto cache = std::make_shared<cad::ToolGeometryCache>(store);
            print("new_process", run(cache, 4), cache);
            if (args.contains("--warm"))
                require(cache->statistics().builds == 0 && cache->statistics().diskHits > 0,
                        "New process rebuilt persisted geometry");
            if (args.contains("--miss"))
                require(cache->statistics().builds > 0, "Corrupt cache was not rebuilt");
            return 0;
        }
        QTemporaryDir folder;
        require(folder.isValid(), "Temporary directory failed");
        const auto root = folder.path() + QString::fromUtf8("/данные с пробелом");
        const auto directory = nativePath(root);
        auto store = std::make_shared<project::GeometryCacheFiles>(directory);
        auto cache = std::make_shared<cad::ToolGeometryCache>(store);
        print("disabled_4", run({}, 4), {});
        print("cold_disk_4", run(cache, 4), cache);
        const auto built = cache->statistics().builds;
        const auto originalFiles = QDir(root).entryList({"*.brepc"}, QDir::Files);
        require(built > 0 && cache->statistics().writes == built, "Cold artifacts not persisted");
        print("warm_ram_4", run(cache, 4), cache);
        require(cache->statistics().builds == built, "Warm cache rebuilt an unchanged tool");
        const auto child = [&](const QString &path, bool warm, bool miss = false) {
            QProcess p;
            QStringList arguments{"--child", path};
            if (warm)
                arguments << "--warm";
            if (miss)
                arguments << "--miss";
            p.start(QCoreApplication::applicationFilePath(), arguments);
            require(p.waitForFinished(60000), "Child timed out");
            std::cout << p.readAllStandardOutput().constData();
            if (p.exitCode() != 0)
                std::cerr << p.readAllStandardError().constData();
            require(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0, "Child failed");
        };
        child(root, true);
        if (args.contains("--benchmark")) {
            auto cycleCache = std::make_shared<cad::ToolGeometryCache>(
                std::make_shared<project::GeometryCacheFiles>(nativePath(folder.path()) /
                                                              "cycle-cache"));
            fullCycle(cycleCache, folder.path());
            return 0;
        }
        cache->clearMemory();
        print("disk_reload_4", run(cache, 4), cache);
        require(cache->statistics().builds == built, "Reload rebuilt an unchanged tool");
        auto tiny = std::make_shared<cad::ToolGeometryCache>(store, 1);
        run(tiny, 2);
        require(tiny->statistics().builds == 0 && tiny->statistics().diskHits > 0 &&
                    tiny->statistics().memoryHits == 0,
                "LRU eviction changed persistent keys");
        auto a = segmentedTools();
        std::array<TopoDS_Shape, 2> reference, restored;
        {
            cad::GeometryCacheScope scope({}, {});
            reference = prepareAssets(a);
        }
        {
            cad::GeometryCacheScope scope(cache, {});
            restored = prepareAssets(a);
        }
        for (std::size_t i = 0; i < 2; ++i)
            same(reference[i], restored[i]);
        a.punchReversed = true;
        {
            cad::GeometryCacheScope scope({}, {});
            reference = prepareAssets(a);
        }
        {
            cad::GeometryCacheScope scope(cache, {});
            restored = prepareAssets(a);
        }
        for (std::size_t i = 0; i < 2; ++i)
            same(reference[i], restored[i]);
        auto renamed = a;
        renamed.punch.id = renamed.punch.code = "renamed";
        renamed.punch.drawing->sha256 = std::string(64, 'f');
        renamed.punch.drawing->sourceFile = "moved.dxf";
        for (auto &piece : *renamed.punch.segments)
            piece.quantity = 5;
        {
            cad::GeometryCacheScope scope(cache, {});
            prepareAssets(renamed);
        }
        require(cache->statistics().builds == built,
                "Names, original digest or stock quantities invalidated unchanged geometry");
        // Same article and original DXF digest, different captured contour.
        const auto oldDigest = a.punch.drawing->sha256;
        a.punch.drawing->edges[2].end.x += 1;
        a.punch.drawing->edges[3].start.x += 1;
        {
            cad::GeometryCacheScope scope(cache, {});
            restored = prepareAssets(a);
        }
        require(a.punch.drawing->sha256 == oldDigest && cache->statistics().builds > built,
                "Captured contour inherited the original DXF cache key");
        const auto bytes = originalFiles;
        require(!bytes.empty(), "No persistent files");
        // Invalid envelope and truncated body must be a miss, never a valid tool.
        QFile broken(QDir(root).filePath(bytes.front()));
        require(broken.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    broken.write("partial") == 7,
                "Cannot corrupt fixture");
        broken.close();
        child(root, false, true);
        child(root, true);
        require(broken.open(QIODevice::ReadWrite) && broken.seek(broken.size() - 1),
                "Cannot inspect payload fixture");
        auto last = broken.read(1);
        require(last.size() == 1 && broken.seek(broken.size() - 1), "Cannot seek payload");
        last[0] = static_cast<char>(last[0] ^ 1);
        require(broken.write(last) == 1, "Cannot corrupt payload");
        broken.close();
        child(root, false, true);
        child(root, true);
        const auto concurrent = folder.path() + "/concurrent";
        QProcess first, second;
        first.start(QCoreApplication::applicationFilePath(), {"--child", concurrent});
        second.start(QCoreApplication::applicationFilePath(), {"--child", concurrent});
        require(first.waitForFinished(60000) && second.waitForFinished(60000) &&
                    first.exitCode() == 0 && second.exitCode() == 0,
                "Concurrent cache writers failed");
        child(concurrent, true);
        QFile blocker(folder.path() + "/file");
        require(blocker.open(QIODevice::WriteOnly), "Cannot create read-only fixture");
        blocker.close();
        auto unavailable = std::make_shared<cad::ToolGeometryCache>(
            std::make_shared<project::GeometryCacheFiles>(nativePath(blocker.fileName())));
        run(unavailable, 2);
        require(unavailable->statistics().writeFailures > 0,
                "Unavailable disk pretended to persist");
        std::stop_source stopped;
        stopped.request_stop();
        bool cancelled = false;
        try {
            cad::GeometryCacheScope scope(cache, stopped.get_token());
            prepareAssets(segmentedTools());
        } catch (...) {
            cancelled = true;
        }
        require(cancelled, "Cancelled preparation read or wrote cache artifacts");
        auto cycleCache =
            std::make_shared<cad::ToolGeometryCache>(std::make_shared<project::GeometryCacheFiles>(
                nativePath(folder.path()) / "cycle-cache"));
        fullCycle(cycleCache, folder.path());
        std::cout << "Persistent tools: geometry, fingerprints, restart, corruption, concurrency "
                     "and fallback passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
