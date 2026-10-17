#include "bending/project/ProjectFiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>

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
    require(!files.save(nativePath(destination), *document, 2.0, {}), "Cannot save project");
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
    require(files.save(nativePath(moved), *document, 3.0, {}).has_value() &&
                read(moved) == originalProject,
            "Rejected save overwrote existing project");
    std::stop_source stop;
    stop.request_stop();
    require(files.save(nativePath(moved), *document, {}, stop.get_token()).has_value() &&
                read(moved) == originalProject,
            "Cancelled save overwrote project");
    require(files
                .save(nativePath(temporary.path() + QStringLiteral("/missing/no.bend")), *document,
                      {}, {})
                .has_value(),
            "Write failure was ignored");
    const auto folded = resultDocument(files.load(
        nativePath(QString::fromUtf8(BENDING_EXAMPLE_DIR) + QStringLiteral("/angle-mm.step")), {},
        {}));
    require(folded && validateThickness(2.0, folded->metadata().thickness) ==
                          ThicknessValidation::LocalMatch,
            "Folded part must provide local evidence without a global thickness proof");
    const auto foldedPath = temporary.path() + QStringLiteral("/folded.bend");
    require(!files.save(nativePath(foldedPath), *folded, 2.0, {}),
            "Cannot save local manual thickness");
    const auto foldedProject = files.load(nativePath(foldedPath), {}, {});
    require(std::get<OpenedProject>(foldedProject).manualThicknessMm == 2.0 &&
                resultDocument(foldedProject)->metadata().thickness.status !=
                    ThicknessStatus::Detected,
            "Reload converted a local manual value into automatic recognition");
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
    changed.insert(QStringLiteral("version"), 2);
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
    require(!files.save(nativePath(moved), *document, {}, {}),
            "Automatic parameters could not be saved");
    const auto automatic = files.load(nativePath(moved), {}, {});
    require(!std::get<OpenedProject>(automatic).manualThicknessMm &&
                resultDocument(automatic)->metadata().thickness.thicknessMm,
            "Automatic provenance lost");
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        testProject();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Portable project: embedded snapshot, units, Unicode, relocation, schema, "
                 "integrity and atomic failure passed\n";
}
