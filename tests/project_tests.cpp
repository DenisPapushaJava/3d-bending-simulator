#include "bending/application/DocumentSession.h"
#include "bending/project/ProjectFiles.h"

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
        if (session.bend(1)) {
            session.setBendReview(1, BendReview::Excluded);
        }
        auto entries = session.bends();
        std::reverse(entries.begin(), entries.end());
        require(!files.save(path, *session.document(), 2.0, entries, {}),
                "Cannot save bend decisions");
        const auto bytes = read(destination);
        auto root = QJsonDocument::fromJson(bytes).object();
        require(root.value(QStringLiteral("version")).toInt() == 2, "Wrong output version");
        session.complete(session.beginImport(path), files.load(path, {}, {}));
        require(session.bend(0)->id == id && session.bend(0)->target == target &&
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
        require(session.bend(0) && session.bend(0)->review == BendReview::Pending &&
                    !session.bend(0)->target && !session.modified(),
                "Legacy v1 did not open cleanly");
        require(!files.save(path, *session.document(), session.manualThicknessMm(), session.bends(),
                            {}),
                "Legacy project could not be saved as v2");

        // A valid but unmatched geometry record is retained and never transferred to another zone.
        auto unresolved = session.bends();
        unresolved[0].review = BendReview::Confirmed;
        unresolved[0].target = target;
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
                    session.bends()[session.bendBindings().unresolved[0]].target == target,
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
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        testProject();
        testBendProject();
        testPrivatePreparation();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Portable project: embedded snapshot, units, Unicode, relocation, schema, "
                 "integrity and atomic failure passed\n";
}
