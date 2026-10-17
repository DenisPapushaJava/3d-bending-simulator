#include "bending/project/ProjectFiles.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace bending::project {
namespace {
using namespace application;
constexpr qint64 maximumProjectBytes = 92 * 1024 * 1024;
QString qtPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromUtf8(path.native().data(), static_cast<qsizetype>(path.native().size()));
#endif
}
std::filesystem::path nativePath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
ImportError invalid(const char *message) {
    return {ImportErrorCode::InvalidProject, message, {}};
}
bool rejected(double value, const ThicknessAnalysis &analysis) {
    const auto result = validateThickness(value, analysis);
    return result == ThicknessValidation::Invalid ||
           result == ThicknessValidation::ConflictsWithGeometry;
}
} // namespace
ImportResult ProjectFiles::load(const std::filesystem::path &path, std::stop_token stop,
                                const ImportProgress &progress) const {
    if (qtPath(path.extension()).compare(QStringLiteral(".bend"), Qt::CaseInsensitive) != 0) {
        return importer_.load(path, stop, progress);
    }
    if (stop.stop_requested()) {
        return ImportError{ImportErrorCode::Cancelled, "Open cancelled", {}};
    }
    if (progress) {
        progress(ImportPhase::Reading);
    }
    QFile file(qtPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return ImportError{file.exists() ? ImportErrorCode::ReadFailed
                                         : ImportErrorCode::FileNotFound,
                           file.errorString().toStdString(),
                           {}};
    }
    if (file.size() <= 0 || file.size() > maximumProjectBytes) {
        return invalid("Empty or oversized project");
    }
    const auto bytes = file.read(maximumProjectBytes + 1);
    if (bytes.size() > maximumProjectBytes || file.error() != QFileDevice::NoError) {
        return invalid("Cannot read project");
    }
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        return invalid("Invalid project JSON");
    }
    const auto root = json.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("3d-bending-simulator")) {
        return invalid("Unknown project format");
    }
    const auto version = root.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        return ImportError{ImportErrorCode::UnsupportedProjectVersion,
                           "Supported project format version is 1",
                           {}};
    }
    if (root.value(QStringLiteral("units")).toString() != QStringLiteral("mm") ||
        !root.value(QStringLiteral("source")).isObject() ||
        !root.value(QStringLiteral("parameters")).isObject()) {
        return invalid("Missing source, parameters or millimetre units");
    }
    const auto source = root.value(QStringLiteral("source")).toObject();
    const auto name = source.value(QStringLiteral("name")).toString();
    const auto suffix = qtPath(nativePath(name).extension()).toLower();
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name.contains(QChar(0)) ||
        (suffix != QStringLiteral(".step") && suffix != QStringLiteral(".stp")) ||
        !source.value(QStringLiteral("data")).isString() ||
        !source.value(QStringLiteral("sha256")).isString()) {
        return invalid("Invalid embedded STEP description");
    }
    const auto decoded =
        QByteArray::fromBase64Encoding(source.value(QStringLiteral("data")).toString().toLatin1(),
                                       QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty() ||
        static_cast<std::size_t>(decoded.decoded.size()) > cad::StepImporter::maximumSourceBytes ||
        QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex() !=
            source.value(QStringLiteral("sha256")).toString().toLatin1()) {
        return invalid("Embedded STEP data is invalid or its SHA-256 does not match");
    }
    const auto value = root.value(QStringLiteral("parameters"))
                           .toObject()
                           .value(QStringLiteral("manualThicknessMm"));
    if (!value.isNull() && !value.isDouble()) {
        return invalid("Missing or invalid manual thickness");
    }
    std::optional<double> manual;
    if (value.isDouble()) {
        manual = value.toDouble();
    }
    if (manual && rejected(*manual, {})) {
        return invalid("Thickness is outside the supported range");
    }
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto loaded = importer_.readBytes(decoded.decoded.toStdString(),
                                      absolute.parent_path() / nativePath(name), stop, progress);
    if (auto *error = std::get_if<cad::StepImportError>(&loaded)) {
        return std::move(*error);
    }
    DocumentPtr document(std::move(std::get<std::unique_ptr<const cad::CadDocument>>(loaded)));
    if (manual && rejected(*manual, document->metadata().thickness)) {
        return invalid("Saved thickness conflicts with embedded geometry");
    }
    return OpenedProject{std::move(document), manual, absolute};
}

std::optional<std::string> ProjectFiles::save(const std::filesystem::path &path,
                                              const Document &document,
                                              std::optional<double> manualThicknessMm,
                                              std::stop_token stop) const {
    if (qtPath(path.extension()).compare(QStringLiteral(".bend"), Qt::CaseInsensitive) != 0) {
        return "Project filename must end with .bend";
    }
    if (manualThicknessMm && rejected(*manualThicknessMm, document.metadata().thickness)) {
        return "Thickness conflicts with geometry";
    }
    const auto source = document.sourceStep();
    if (source.empty() || source.size() > cad::StepImporter::maximumSourceBytes) {
        return "Document has no supported STEP snapshot";
    }
    if (stop.stop_requested()) {
        return "Save cancelled";
    }
    const QByteArray bytes(source.data(), static_cast<qsizetype>(source.size()));
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("3d-bending-simulator")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("units"), QStringLiteral("mm")},
        {QStringLiteral("source"),
         QJsonObject{{QStringLiteral("name"), qtPath(document.metadata().sourcePath.filename())},
                     {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
                     {QStringLiteral("sha256"),
                      QString::fromLatin1(
                          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}},
        {QStringLiteral("parameters"),
         QJsonObject{{QStringLiteral("manualThicknessMm"), manualThicknessMm
                                                               ? QJsonValue(*manualThicknessMm)
                                                               : QJsonValue(QJsonValue::Null)}}}};
    const auto encoded = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (stop.stop_requested()) {
        return "Save cancelled";
    }
    QSaveFile file(qtPath(path));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size()) {
        return file.errorString().toStdString();
    }
    if (stop.stop_requested()) {
        file.cancelWriting();
        return "Save cancelled";
    }
    if (!file.commit()) {
        return file.errorString().toStdString();
    }
    return {};
}
} // namespace bending::project
