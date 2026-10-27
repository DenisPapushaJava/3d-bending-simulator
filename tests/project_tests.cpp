#include "bending/application/DocumentSession.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {
using namespace bending::application;
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
std::filesystem::path nativePath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot open fixture");
    return file.readAll();
}
void write(const QString &path, const QByteArray &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(),
            "Cannot write fixture");
}
void testProject() {
    bending::project::ProjectFiles files;
    QTemporaryDir temporary;
    require(temporary.isValid(), "No temporary directory");
    const auto source = temporary.path() + QStringLiteral("/Деталь с пробелами.stp");
    require(QFile::copy(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/plate-inch.stp"),
                        source),
            "Cannot copy STEP");
    const auto loaded = files.load(nativePath(source), {}, {});
    const auto document = resultDocument(loaded);
    require(document && document->metadata().thickness.thicknessMm.has_value(),
            "No recognized plate");
    const auto originalBytes = read(source);
    write(source, "source changed after import");
    const auto destination = temporary.path() + QStringLiteral("/Проект.bend");
    require(!files.save(nativePath(destination), *document, 2.0, {}, {}), "Cannot save project");
    require(QFile::remove(source), "Cannot remove original source");
    const auto relocated = temporary.path() + QStringLiteral("/Перенос");
    require(QDir().mkpath(relocated), "Cannot create relocated folder");
    const auto moved = relocated + QStringLiteral("/Новый путь.bend");
    require(QFile::rename(destination, moved), "Cannot move project");
    const auto reopened = files.load(nativePath(moved), {}, {});
    const auto *project = std::get_if<OpenedProject>(&reopened);
    require(project && project->manualThicknessMm == 2.0 && project->path == nativePath(moved),
            "Portable project parameters/path lost");
    require(std::abs(project->document->metadata().volumeMm3 - 12000.0) < 1.0e-5,
            "Embedded model changed");
    require(project->document->sourceStep() == originalBytes.toStdString() &&
                project->document->metadata().sourceLengthUnits == std::vector<std::string>{"INCH"},
            "Save re-read modified source or lost original units");
    const auto originalProject = read(moved);
    require(files.save(nativePath(moved), *document, 3.0, {}, {}).has_value() &&
                read(moved) == originalProject,
            "Rejected save overwrote existing project");
    std::stop_source stop;
    stop.request_stop();
    require(files.save(nativePath(moved), *document, {}, {}, stop.get_token()).has_value() &&
                read(moved) == originalProject,
            "Cancelled save overwrote project");
    require(files
                .save(nativePath(temporary.path() + QStringLiteral("/missing/no.bend")), *document,
                      {}, {}, {})
                .has_value(),
            "Write failure was ignored");
    const auto folded = resultDocument(files.load(
        nativePath(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/angle-mm.step")), {},
        {}));
    require(folded && validateThickness(2.0, folded->metadata().thickness) ==
                          ThicknessValidation::LocalMatch,
            "Folded part must provide local evidence without a global thickness proof");
    const auto foldedPath = temporary.path() + QStringLiteral("/folded.bend");
    require(!files.save(nativePath(foldedPath), *folded, 2.0, {}, {}),
            "Cannot save local manual thickness");
    const auto foldedProject = files.load(nativePath(foldedPath), {}, {});
    require(std::get<OpenedProject>(foldedProject).manualThicknessMm == 2.0 &&
                resultDocument(foldedProject)->metadata().thickness.status !=
                    ThicknessStatus::Detected,
            "Reload converted a local manual value into automatic recognition");
    for (const auto &name : {"channel-2mm.step", "trimmed-bend-2mm.step"}) {
        const auto bent =
            resultDocument(files.load(nativePath(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                                                 QLatin1Char('/') + QString::fromLatin1(name)),
                                      {}, {}));
        const auto expected = std::string_view(name) == "channel-2mm.step" ? 2U : 1U;
        require(bent && bent->metadata().bendAnalysis.bends.size() == expected,
                "Missing bend fixtures");
        const auto bendPath = nativePath(temporary.path() + QStringLiteral("/bends.bend"));
        require(!files.save(bendPath, *bent, {}, {}, {}), "Cannot save bent part");
        const auto bendReopened = resultDocument(files.load(bendPath, {}, {}));
        require(bendReopened && bendReopened->metadata().bendAnalysis.bends.size() == expected,
                "Bends not reconstructed from embedded STEP");
        for (std::size_t i = 0; i < expected; ++i) {
            const auto &a = bent->metadata().bendAnalysis.bends[i];
            const auto &b = bendReopened->metadata().bendAnalysis.bends[i];
            require(std::abs(a.angleRadians - b.angleRadians) < 1.0e-6 &&
                        std::abs(a.lengthMm - b.lengthMm) < 1.0e-5 &&
                        std::abs(a.innerRadiusMm - b.innerRadiusMm) < 1.0e-5 &&
                        std::abs(a.thicknessMm - b.thicknessMm) < 1.0e-5 &&
                        a.trimmedEnds == b.trimmedEnds &&
                        std::abs(a.minimumStraightLengthMm - b.minimumStraightLengthMm) < 1.0e-4,
                    "Reopened bend measurements changed");
        }
    }
    const auto invalid = temporary.path() + QStringLiteral("/invalid.bend");
    const auto reject = [&](QJsonObject root, ImportErrorCode expected) {
        write(invalid, QJsonDocument(root).toJson());
        const auto result = files.load(nativePath(invalid), {}, {});
        const auto *error = std::get_if<ImportError>(&result);
        require(error && error->code == expected,
                "Malformed project was accepted or has wrong diagnostic");
    };
    auto root = QJsonDocument::fromJson(originalProject).object();
    auto changed = root;
    changed.insert(QStringLiteral("version"), 99);
    reject(changed, ImportErrorCode::UnsupportedProjectVersion);
    changed = root;
    auto payload = changed.value(QStringLiteral("source")).toObject();
    payload.insert(QStringLiteral("sha256"), QStringLiteral("bad hash"));
    changed.insert(QStringLiteral("source"), payload);
    reject(changed, ImportErrorCode::InvalidProject);
    changed = root;
    changed.insert(QStringLiteral("parameters"),
                   QJsonObject{{QStringLiteral("manualThicknessMm"), 3.0}});
    reject(changed, ImportErrorCode::InvalidProject);
    changed.insert(QStringLiteral("parameters"),
                   QJsonObject{{QStringLiteral("manualThicknessMm"), -1.0}});
    reject(changed, ImportErrorCode::InvalidProject);
    payload = root.value(QStringLiteral("source")).toObject();
    payload.insert(QStringLiteral("data"), QStringLiteral("invalid base64!"));
    changed = root;
    changed.insert(QStringLiteral("source"), payload);
    reject(changed, ImportErrorCode::InvalidProject);
    write(invalid, "{broken");
    require(std::holds_alternative<ImportError>(files.load(nativePath(invalid), {}, {})),
            "Malformed JSON accepted");
    require(!files.save(nativePath(moved), *document, {}, {}, {}),
            "Automatic parameters could not be saved");
    const auto automatic = files.load(nativePath(moved), {}, {});
    require(!std::get<OpenedProject>(automatic).manualThicknessMm &&
                resultDocument(automatic)->metadata().thickness.thicknessMm,
            "Automatic provenance lost");
}
void testBendProject() {
    bending::project::ProjectFiles files;
    QTemporaryDir temporary;
    const auto destination = temporary.path() + QStringLiteral("/Подготовка.bend");
    const auto path = nativePath(destination);
    for (const auto *fixture : {"channel-2mm.step", "trimmed-bend-2mm.step"}) {
        DocumentSession session;
        session.complete(session.beginImport("part.step"),
                         files.load(nativePath(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                                               QLatin1Char('/') + QString::fromUtf8(fixture)),
                                    {}, {}));
        require(session.document() && session.bend(0), "Missing bent project fixture");
        const auto id = session.bend(0)->id;
        const bending::core::BendTarget target{2.0, 5.25};
        session.setBendReview(0, BendReview::Confirmed);
        session.setBendTarget(0, target);
        const bending::core::PressTools tools{
            48, std::numbers::pi / 2, 85 * std::numbers::pi / 180, 2, 100, 10};
        require(session.setBendTools(0, tools), "Cannot assign tools");
        if (session.bend(1)) {
            session.setBendReview(1, BendReview::Excluded);
        }
        auto entries = session.bends();
        std::reverse(entries.begin(), entries.end());
        require(!files.save(path, *session.document(), 2.0, entries, {}),
                "Cannot save bend decisions");
        const auto bytes = read(destination);
        auto root = QJsonDocument::fromJson(bytes).object();
        require(root.value(QStringLiteral("version")).toInt() == 5, "Wrong output version");
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bend(0)->id == id && session.bend(0)->target == target &&
                    session.bend(0)->tools == tools &&
                    session.bend(0)->review == BendReview::Confirmed && !session.modified() &&
                    session.bendBindings().unresolved.empty(),
                "Reopen lost bend target or stable ID");
        require(!session.bend(1) || session.bend(1)->review == BendReview::Excluded,
                "Reopen lost exclusion");
        const auto reject = [&](const QJsonObject &invalid) {
            write(destination, QJsonDocument(invalid).toJson());
            const auto loaded = files.load(path, {}, {});
            require(std::holds_alternative<ImportError>(loaded) &&
                        std::get<ImportError>(loaded).code == ImportErrorCode::InvalidProject,
                    "Malformed bend data accepted");
        };
        auto invalid = root;
        invalid.remove(QStringLiteral("bends"));
        reject(invalid);
        auto bends = root.value(QStringLiteral("bends")).toArray();
        for (const auto &targetJson :
             {QJsonValue(QJsonArray{90, 5}),
              QJsonValue(QJsonObject{{QStringLiteral("angleRadians"), 0},
                                     {QStringLiteral("innerRadiusMm"), 4}}),
              QJsonValue(QJsonObject{{QStringLiteral("angleRadians"), 1},
                                     {QStringLiteral("innerRadiusMm"), -1}})}) {
            auto bad = bends;
            auto entry = bad[0].toObject();
            entry.insert(QStringLiteral("target"), targetJson);
            bad[0] = entry;
            invalid = root;
            invalid.insert(QStringLiteral("bends"), bad);
            reject(invalid);
        }
        for (const auto &toolValue :
             {QJsonValue(QJsonValue::Undefined), QJsonValue(QStringLiteral("bad")),
              QJsonValue(QJsonObject{{QStringLiteral("openingMm"), 48}})}) {
            auto badTools = bends;
            auto entry = badTools[0].toObject();
            entry.insert(QStringLiteral("tools"), toolValue);
            badTools[0] = entry;
            invalid = root;
            invalid.insert(QStringLiteral("bends"), badTools);
            reject(invalid);
        }
        // Legacy v2 retains targets but has no tools. A v3-only record must not be silently read as
        // v2.
        invalid = root;
        invalid.insert(QStringLiteral("version"), 2);
        reject(invalid);
        auto v3Bends = bends;
        for (qsizetype i = 0; i < v3Bends.size(); ++i) {
            auto entry = v3Bends[i].toObject();
            entry.remove(QStringLiteral("catalogTools"));
            auto oldTools = entry.value(QStringLiteral("tools")).toObject();
            if (!oldTools.isEmpty()) {
                oldTools.remove(QStringLiteral("punchLengthMm"));
                entry.insert(QStringLiteral("tools"), oldTools);
            }
            v3Bends[i] = entry;
        }
        auto v3 = root;
        v3.insert(QStringLiteral("version"), 3);
        v3.insert(QStringLiteral("bends"), v3Bends);
        write(destination, QJsonDocument(v3).toJson());
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bend(0)->tools == tools && session.bend(0)->target == target &&
                    !session.modified(),
                "Legacy v3 migration failed");
        auto legacyBends = bends;
        for (qsizetype i = 0; i < legacyBends.size(); ++i) {
            auto entry = legacyBends[i].toObject();
            entry.remove(QStringLiteral("tools"));
            entry.remove(QStringLiteral("catalogTools"));
            legacyBends[i] = entry;
        }
        auto legacy = root;
        legacy.insert(QStringLiteral("version"), 2);
        legacy.insert(QStringLiteral("bends"), legacyBends);
        write(destination, QJsonDocument(legacy).toJson());
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bend(0)->target == target && !session.bend(0)->tools && !session.modified(),
                "Legacy v2 migration failed");
        auto bad = bends;
        bad.append(bad[0]);
        invalid = root;
        invalid.insert(QStringLiteral("bends"), bad);
        reject(invalid);
        for (const auto &field :
             {QStringLiteral("review"), QStringLiteral("id"), QStringLiteral("geometry")}) {
            auto entry = bends[0].toObject();
            entry.insert(field, QStringLiteral("bad value"));
            bad = bends;
            bad[0] = entry;
            invalid = root;
            invalid.insert(QStringLiteral("bends"), bad);
            reject(invalid);
        }
        auto rejectedEntries = entries;
        rejectedEntries[0].target = bending::core::BendTarget{1, -1};
        write(destination, bytes);
        require(files.save(path, *session.document(), {}, rejectedEntries, {}).has_value() &&
                    read(destination) == bytes,
                "Rejected bend save overwrote project");

        // Previously released v1 has no saved decisions; measurements become pending records.
        root.insert(QStringLiteral("version"), 1);
        root.remove(QStringLiteral("bends"));
        write(destination, QJsonDocument(root).toJson());
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bend(0) && session.bend(0)->review == BendReview::Automatic &&
                    !session.bend(0)->target && !session.modified(),
                "Legacy v1 did not open cleanly");
        require(!files.save(path, *session.document(), session.manualThicknessMm(), session.bends(),
                            {}),
                "Legacy project could not be saved as v4");

        // A valid but unmatched geometry record is retained and never transferred to another zone.
        auto unresolved = session.bends();
        unresolved[0].review = BendReview::Confirmed;
        unresolved[0].target = target;
        unresolved[0].tools = tools;
        unresolved[0].anchor.axisStart.x += 20;
        unresolved[0].anchor.axisEnd.x += 20;
        const auto unresolvedId = unresolved[0].id;
        require(!files.save(path, *session.document(), {}, unresolved, {}),
                "Cannot save unmatched record");
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bendBindings().unresolved.size() == 1 && session.bend(0) &&
                    !session.bend(0)->target && session.bend(0)->id != unresolvedId,
                "Unmatched target applied to a different zone");
        require(!files.save(path, *session.document(), {}, session.bends(), {}),
                "Cannot retain unresolved decisions");
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bendBindings().unresolved.size() == 1 &&
                    session.bends()[session.bendBindings().unresolved[0]].target == target &&
                    session.bends()[session.bendBindings().unresolved[0]].tools == tools,
                "Repeated save discarded unresolved intent");
    }
}
void testPrivatePreparation() {
    const auto source = qEnvironmentVariable("BENDING_RECOGNITION_STEP");
    if (source.isEmpty()) {
        return;
    }
    bending::project::ProjectFiles files;
    DocumentSession session;
    session.complete(session.beginImport(nativePath(source)),
                     files.load(nativePath(source), {}, {}));
    require(session.document() && !session.bends().empty(), "Private fixture has no bend records");
    const auto expected = qEnvironmentVariableIntValue("BENDING_RECOGNITION_COUNT");
    require(expected > 0 && session.bends().size() == static_cast<std::size_t>(expected),
            "Private fixture has unexpected bend count");
    const bending::core::BendTarget target{2, 5.25};
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < session.bends().size(); ++i) {
        ids.push_back(session.bend(i)->id);
        require(session.setBendReview(i, BendReview::Confirmed) && session.setBendTarget(i, target),
                "Cannot prepare private bend");
    }
    session.setBendReview(ids.size() - 1, BendReview::Excluded);
    QTemporaryDir temporary;
    const auto path = nativePath(temporary.path() + QStringLiteral("/private.bend"));
    require(!files.save(path, *session.document(), {}, session.bends(), {}), "Private save failed");
    session.complete(session.beginImport(path), files.load(path, {}, {}));
    require(session.bendBindings().unresolved.empty() && !session.modified(),
            "Private bend records did not resolve cleanly");
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(session.bend(i) && session.bend(i)->id == ids[i] &&
                    session.bend(i)->target == target &&
                    session.bend(i)->review ==
                        (i + 1 == ids.size() ? BendReview::Excluded : BendReview::Confirmed),
                "Private project lost target/decision/ID");
    }
    std::cout << "Private project: " << ids.size() << " bend records restored\n";
}
void testCatalogProject() {
    using namespace bending;
    const auto bundled = project::builtInToolCatalog();
    require(std::holds_alternative<core::ToolCatalog>(bundled), "Bundled catalogue unreadable");
    const auto &catalog = std::get<core::ToolCatalog>(bundled);
    require(catalog.size() == 17 && std::count_if(catalog.begin(), catalog.end(),
                                                  [](const auto &t) { return !t.teaching; }) == 15,
            "Wrong catalogue contents");
    const auto find = [&](const std::string &id) -> const core::ToolPreset & {
        const auto it =
            std::find_if(catalog.begin(), catalog.end(), [&](const auto &t) { return t.id == id; });
        require(it != catalog.end(), "Missing preset");
        return *it;
    };
    core::ToolAssignment assignment{find("s-tool-st1801m"), find("s-tool-st2401s"), 3, 10,
                                    "Test machine"};
    assignment.punch.drawing =
        core::ToolDrawing{"own.dxf",
                          std::string(64, 'a'),
                          true,
                          {{{0, 0}, {2, 0}, {}, 0},
                           {{2, 0}, {0, 2}, core::DrawingPoint{0, 0}, std::numbers::pi / 2},
                           {{0, 2}, {0, 0}, {}, 0}}};
    require(assignment.punch.lengthMm == 525 &&
                assignment.die.openingsMm == std::vector<double>{10, 16, 22, 35},
            "Catalogue facts changed");
    project::ProjectFiles files;
    DocumentSession session;
    session.complete(session.beginImport("fixture.step"),
                     files.load(nativePath(QString::fromUtf8(BENDING_EXAMPLE_DIR) +
                                           QStringLiteral("/bend-90-6mm.step")),
                                {}, {}));
    require(session.bend(0) && session.bend(0)->review == BendReview::Automatic &&
                session.bendTarget(0),
            "No automatic target");
    const auto revision = session.editRevision();
    require(
        session.setBendToolAssignment(0, assignment) && session.editRevision() == revision + 1 &&
            session.setBendToolAssignment(0, assignment) && session.editRevision() == revision + 1,
        "Assignment is not atomic/idempotent");
    QTemporaryDir temp;
    const auto path = temp.path() + QStringLiteral("/catalog.bend");
    require(!files.save(nativePath(path), *session.document(), {}, session.bends(), {}),
            "Snapshot save failed");
    const auto bytes = read(path);
    const auto root = QJsonDocument::fromJson(bytes).object();
    {
        auto legacyRoot = root;
        legacyRoot.insert(QStringLiteral("version"), 4);
        auto legacyBends = legacyRoot.value(QStringLiteral("bends")).toArray();
        auto legacyBend = legacyBends[0].toObject();
        auto pair = legacyBend.value(QStringLiteral("catalogTools")).toObject();
        for (const auto &key : {QStringLiteral("punch"), QStringLiteral("die")}) {
            auto tool = pair.value(key).toObject();
            tool.remove(QStringLiteral("drawing"));
            tool.remove(QStringLiteral("parametersConfirmed"));
            pair.insert(key, tool);
        }
        legacyBend.insert(QStringLiteral("catalogTools"), pair);
        legacyBends[0] = legacyBend;
        legacyRoot.insert(QStringLiteral("bends"), legacyBends);
        write(path, QJsonDocument(legacyRoot).toJson());
        DocumentSession legacy;
        legacy.complete(legacy.beginImport(nativePath(path)), files.load(nativePath(path), {}, {}));
        require(legacy.document() && legacy.bend(0)->catalogTools &&
                    !legacy.bend(0)->catalogTools->punch.drawing &&
                    legacy.bend(0)->catalogTools->punch.parametersConfirmed,
                "Legacy v4 catalogue without drawing no longer opens");
        write(path, bytes);
    }
    session.complete(session.beginImport(nativePath(path)), files.load(nativePath(path), {}, {}));
    require(!session.modified() && session.bend(0)->catalogTools == assignment &&
                session.bend(0)->tools == core::toolParameters(assignment) &&
                !session.bend(0)->target && session.bendTarget(0),
            "Portable snapshot or automatic target lost");
    auto excluded = session.bends();
    excluded[0].review = BendReview::Excluded;
    require(session.setBendReview(0, BendReview::Excluded) && !session.bendTarget(0) &&
                session.bend(0)->catalogTools == assignment,
            "Exclusion lost snapshot");
    require(session.setBendReview(0, BendReview::Automatic) && session.bendTarget(0),
            "Automatic mode did not recover");
    require(session.setBendTools(0, session.bend(0)->tools) && !session.bend(0)->catalogTools,
            "Manual parameters retained catalogue attribution");
    const auto reject = [&](QJsonObject rootValue) {
        write(path, QJsonDocument(rootValue).toJson());
        const auto loaded = files.load(nativePath(path), {}, {});
        require(std::holds_alternative<ImportError>(loaded) &&
                    std::get<ImportError>(loaded).code == ImportErrorCode::InvalidProject,
                "Corrupt snapshot accepted");
    };
    for (const auto &field :
         {QStringLiteral("groove"), QStringLiteral("clearanceMm"), QStringLiteral("punch")}) {
        auto altered = root;
        auto bends = altered.value(QStringLiteral("bends")).toArray();
        auto bend = bends[0].toObject();
        auto a = bend.value(QStringLiteral("catalogTools")).toObject();
        a.insert(field, field == QStringLiteral("groove")        ? QJsonValue(99)
                        : field == QStringLiteral("clearanceMm") ? QJsonValue(20)
                                                                 : QJsonValue(QJsonValue::Null));
        bend.insert(QStringLiteral("catalogTools"), a);
        bends[0] = bend;
        altered.insert(QStringLiteral("bends"), bends);
        reject(altered);
    }
    auto altered = root;
    auto bends = altered.value(QStringLiteral("bends")).toArray();
    auto bend = bends[0].toObject();
    bend.remove(QStringLiteral("catalogTools"));
    bends[0] = bend;
    altered.insert(QStringLiteral("bends"), bends);
    reject(altered);
    // External catalogue input is data only, bounded and independently validated.
    const auto catalogPath = temp.path() + QStringLiteral("/tools.json");
    QFile resource(QStringLiteral(":/bending/tool-catalog.json"));
    require(resource.open(QIODevice::ReadOnly), "No bundled JSON");
    const auto catalogBytes = resource.readAll();
    write(catalogPath, catalogBytes);
    require(std::get<core::ToolCatalog>(project::readToolCatalog(nativePath(catalogPath))) ==
                catalog,
            "External catalogue differs");
    auto catalogRoot = QJsonDocument::fromJson(catalogBytes).object();
    {
        auto legacyCatalog = catalogRoot;
        legacyCatalog.insert(QStringLiteral("version"), 1);
        QJsonArray legacyTools;
        for (const auto &item : legacyCatalog.value(QStringLiteral("tools")).toArray()) {
            auto tool = item.toObject();
            if (tool.value(QStringLiteral("parametersConfirmed")).isBool() &&
                !tool.value(QStringLiteral("parametersConfirmed")).toBool())
                continue;
            tool.remove(QStringLiteral("drawing"));
            tool.remove(QStringLiteral("parametersConfirmed"));
            legacyTools.append(tool);
        }
        legacyCatalog.insert(QStringLiteral("tools"), legacyTools);
        write(catalogPath, QJsonDocument(legacyCatalog).toJson());
        require(std::holds_alternative<core::ToolCatalog>(
                    project::readToolCatalog(nativePath(catalogPath))),
                "Legacy catalogue v1 no longer reads");
    }
    auto defs = catalogRoot.value(QStringLiteral("tools")).toArray();
    defs.append(defs[0]);
    catalogRoot.insert(QStringLiteral("tools"), defs);
    write(catalogPath, QJsonDocument(catalogRoot).toJson());
    require(std::holds_alternative<std::string>(project::readToolCatalog(nativePath(catalogPath))),
            "Duplicate catalogue IDs accepted");
    write(catalogPath, QByteArray(1024 * 1024 + 1, ' '));
    require(std::holds_alternative<std::string>(project::readToolCatalog(nativePath(catalogPath))),
            "Unbounded catalogue accepted");
}

} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        testProject();
        testBendProject();
        testCatalogProject();
        testPrivatePreparation();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Portable project: embedded snapshot, units, Unicode, relocation, schema, "
                 "integrity and atomic failure passed\n";
}
