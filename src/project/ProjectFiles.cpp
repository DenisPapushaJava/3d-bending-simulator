#include "bending/project/ProjectFiles.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <utility>

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
QJsonArray pointJson(PointMm point) {
    return {point.x, point.y, point.z};
}
bool readPoint(const QJsonValue &value, PointMm &point) {
    if (!value.isArray()) {
        return false;
    }
    const auto values = value.toArray();
    if (values.size() != 3 || !values[0].isDouble() || !values[1].isDouble() ||
        !values[2].isDouble()) {
        return false;
    }
    point = {values[0].toDouble(), values[1].toDouble(), values[2].toDouble()};
    return true;
}
QString reviewName(BendReview review) {
    switch (review) {
    case BendReview::Pending:
        return QStringLiteral("pending");
    case BendReview::Confirmed:
        return QStringLiteral("confirmed");
    case BendReview::Excluded:
        return QStringLiteral("excluded");
    }
    return {};
}
QJsonArray bendsJson(const std::vector<BendPreparation> &bends) {
    QJsonArray values;
    for (const auto &bend : bends) {
        const auto &a = bend.anchor;
        const QJsonObject geometry{
            {QStringLiteral("axisStartMm"), pointJson(a.axisStart)},
            {QStringLiteral("axisEndMm"), pointJson(a.axisEnd)},
            {QStringLiteral("angleRadians"), a.angleRadians},
            {QStringLiteral("innerRadiusMm"), a.innerRadiusMm},
            {QStringLiteral("thicknessMm"), a.thicknessMm},
            {QStringLiteral("lengthMm"), a.lengthMm},
            {QStringLiteral("trimmedEnds"), a.trimmedEnds},
            {QStringLiteral("minimumStraightLengthMm"), a.minimumStraightLengthMm}};
        const auto target =
            bend.target ? QJsonValue(QJsonObject{
                              {QStringLiteral("angleRadians"), bend.target->angleRadians},
                              {QStringLiteral("innerRadiusMm"), bend.target->innerRadiusMm}})
                        : QJsonValue(QJsonValue::Null);
        values.append(QJsonObject{{QStringLiteral("id"), QString::fromStdString(bend.id)},
                                  {QStringLiteral("review"), reviewName(bend.review)},
                                  {QStringLiteral("geometry"), geometry},
                                  {QStringLiteral("target"), target}});
    }
    return values;
}
bool readBends(const QJsonValue &value, std::vector<BendPreparation> &bends) {
    if (!value.isArray() || value.toArray().size() > 512) {
        return false;
    }
    for (const auto &item : value.toArray()) {
        if (!item.isObject()) {
            return false;
        }
        const auto object = item.toObject();
        if (!object.value(QStringLiteral("id")).isString() ||
            !object.value(QStringLiteral("geometry")).isObject()) {
            return false;
        }
        BendPreparation bend{};
        bend.id = object.value(QStringLiteral("id")).toString().toStdString();
        const auto review = object.value(QStringLiteral("review")).toString();
        if (review == QStringLiteral("pending")) {
            bend.review = BendReview::Pending;
        } else if (review == QStringLiteral("confirmed")) {
            bend.review = BendReview::Confirmed;
        } else if (review == QStringLiteral("excluded")) {
            bend.review = BendReview::Excluded;
        } else {
            return false;
        }
        const auto geometry = object.value(QStringLiteral("geometry")).toObject();
        auto &a = bend.anchor;
        if (!readPoint(geometry.value(QStringLiteral("axisStartMm")), a.axisStart) ||
            !readPoint(geometry.value(QStringLiteral("axisEndMm")), a.axisEnd) ||
            !geometry.value(QStringLiteral("trimmedEnds")).isBool()) {
            return false;
        }
        a.trimmedEnds = geometry.value(QStringLiteral("trimmedEnds")).toBool();
        for (const auto &[name, destination] :
             {std::pair{"angleRadians", &a.angleRadians},
              {"innerRadiusMm", &a.innerRadiusMm},
              {"thicknessMm", &a.thicknessMm},
              {"lengthMm", &a.lengthMm},
              {"minimumStraightLengthMm", &a.minimumStraightLengthMm}}) {
            const auto number = geometry.value(QLatin1String(name));
            if (!number.isDouble()) {
                return false;
            }
            *destination = number.toDouble();
        }
        const auto target = object.value(QStringLiteral("target"));
        if (target.isObject()) {
            const auto parameters = target.toObject();
            const auto angle = parameters.value(QStringLiteral("angleRadians"));
            const auto radius = parameters.value(QStringLiteral("innerRadiusMm"));
            if (!angle.isDouble() || !radius.isDouble()) {
                return false;
            }
            bend.target = core::BendTarget{angle.toDouble(), radius.toDouble()};
        } else if (!target.isNull()) {
            return false;
        }
        bends.push_back(std::move(bend));
    }
    return validBendPreparations(bends);
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
    if (!version.isDouble() || (version.toDouble() != 1.0 && version.toDouble() != 2.0)) {
        return ImportError{ImportErrorCode::UnsupportedProjectVersion,
                           "Supported project format versions are 1 and 2",
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
    std::vector<BendPreparation> bends;
    if (version.toDouble() == 2.0) {
        if (!readBends(root.value(QStringLiteral("bends")), bends)) {
            return invalid("Invalid saved bend preparations");
        }
    } else if (root.contains(QStringLiteral("bends"))) {
        return invalid("Version 1 cannot contain bend preparations");
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
    return OpenedProject{std::move(document), manual, absolute, std::move(bends)};
}

std::optional<std::string> ProjectFiles::save(const std::filesystem::path &path,
                                              const Document &document,
                                              std::optional<double> manualThicknessMm,
                                              const std::vector<BendPreparation> &bends,
                                              std::stop_token stop) const {
    if (qtPath(path.extension()).compare(QStringLiteral(".bend"), Qt::CaseInsensitive) != 0) {
        return "Project filename must end with .bend";
    }
    if (manualThicknessMm && rejected(*manualThicknessMm, document.metadata().thickness)) {
        return "Thickness conflicts with geometry";
    }
    if (!validBendPreparations(bends)) {
        return "Invalid bend preparations";
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
        {QStringLiteral("version"), 2},
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
                                                               : QJsonValue(QJsonValue::Null)}}},
        {QStringLiteral("bends"), bendsJson(bends)}};
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
