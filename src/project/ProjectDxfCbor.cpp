#include "ProjectDxfCbor.h"

#include <QCborArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>

namespace bending::project {
namespace {
using namespace application;
constexpr int cacheVersion = 1;
constexpr qsizetype maximumCutItems = 32768;
constexpr qsizetype maximumBytes = 2 * 1024 * 1024;
constexpr double invalidNumber = std::numeric_limits<double>::quiet_NaN();

class DxfSnapshot final : public FlatPattern {
  public:
    FlatPatternInfo data;
    const FlatPatternInfo &info() const noexcept override {
        return data;
    }
};

QByteArray fingerprint(const QCborMap &inputs, const QByteArray &payload) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QCborValue(inputs).toCbor(QCborValue::SortKeysInMaps));
    hash.addData(payload);
    return hash.result();
}

bool coordinate(double value) {
    return std::isfinite(value) && std::abs(value) <= 1.e8;
}

bool xy(const QCborArray &array, qsizetype index, PointMm &point) {
    point = {array.at(index).toDouble(invalidNumber), array.at(index + 1).toDouble(invalidNumber),
             0};
    return coordinate(point.x) && coordinate(point.y);
}

QCborMap cutData(const FlatPatternInfo &info) {
    QCborArray loops;
    qsizetype items = 0;
    for (const auto &loop : info.cutProfile->loops) {
        items += std::max<qsizetype>(1, static_cast<qsizetype>(loop.vertices.size()));
        if (items > maximumCutItems)
            return {};
        if (loop.radiusMm > 0) {
            loops.append(QCborArray{loop.centre.x, loop.centre.y, loop.radiusMm});
        } else {
            QCborArray vertices;
            for (const auto &vertex : loop.vertices) {
                vertices.append(vertex.point.x);
                vertices.append(vertex.point.y);
                vertices.append(vertex.bulge);
            }
            loops.append(vertices);
        }
    }
    QCborArray bends;
    for (const auto &bend : info.bends) {
        const auto ends = bend.centreLine();
        bends.append(QCborArray{ends[0].x, ends[0].y, ends[1].x, ends[1].y});
    }
    return {{QStringLiteral("loops"), loops},
            {QStringLiteral("bends"), bends},
            {QStringLiteral("blind"), qint64(info.cutProfile->blindOpenings)},
            {QStringLiteral("mid"), info.cutProfile->midThickness}};
}

FlatPatternPtr readCutData(const QCborMap &data) {
    const auto loops = data.value(QStringLiteral("loops")).toArray();
    const auto bendLines = data.value(QStringLiteral("bends")).toArray();
    const auto blind = data.value(QStringLiteral("blind")).toInteger(-1);
    const auto mid = data.value(QStringLiteral("mid"));
    if (loops.isEmpty() || loops.size() > maximumCutItems || !mid.isBool() || blind < 0 ||
        blind > maximumCutItems ||
        bendLines.size() > static_cast<qsizetype>(maximumFlatBendSegments) ||
        !data.value(QStringLiteral("bends")).isArray())
        return {};
    auto profile = std::make_shared<FlatCutProfile>();
    profile->blindOpenings = static_cast<std::size_t>(blind);
    profile->midThickness = mid.toBool();
    qsizetype items = 0;
    for (const auto value : loops) {
        const auto points = value.toArray();
        if (points.size() < 3 || points.size() % 3 != 0 ||
            (items += points.size() / 3) > maximumCutItems)
            return {};
        FlatCutLoop loop;
        if (points.size() == 3) {
            loop.radiusMm = points.at(2).toDouble(invalidNumber);
            if (!xy(points, 0, loop.centre) || !coordinate(loop.radiusMm) || loop.radiusMm <= 0)
                return {};
        } else {
            for (qsizetype i = 0; i < points.size(); i += 3) {
                FlatCutVertex vertex;
                vertex.bulge = points.at(i + 2).toDouble(invalidNumber);
                if (!xy(points, i, vertex.point) || !coordinate(vertex.bulge))
                    return {};
                loop.vertices.push_back(vertex);
            }
            for (std::size_t i = 0; i < loop.vertices.size(); ++i) {
                const auto &a = loop.vertices[i].point;
                const auto &b = loop.vertices[(i + 1) % loop.vertices.size()].point;
                if (a.x == b.x && a.y == b.y)
                    return {};
            }
        }
        profile->loops.push_back(std::move(loop));
    }
    auto flat = std::make_shared<DxfSnapshot>();
    flat->data.cutProfile = std::move(profile);
    for (const auto value : bendLines) {
        const auto line = value.toArray();
        FlatBend bend{};
        if (line.size() != 4 || !xy(line, 0, bend.start) || !xy(line, 2, bend.end) ||
            bend.lengthMm() <= 1.e-8)
            return {};
        flat->data.bends.push_back(bend);
    }
    return flat;
}

QCborMap writeReference(const DxfExportReference &ref, const std::filesystem::path &project) {
    const QFileInfo file(ref.path);
    const QDir folder = QFileInfo(project).absoluteDir();
    const auto absolute = file.absoluteFilePath();
    const auto relative = folder.relativeFilePath(absolute);
    const bool local =
        !QDir::isAbsolutePath(relative) && !relative.startsWith(QStringLiteral("../"));
    return {{QStringLiteral("path"), local ? relative : absolute},
            {QStringLiteral("relative"), local},
            {QStringLiteral("includeBends"), ref.includeBends},
            {QStringLiteral("layer"), QString::fromStdString(ref.bendLayer)},
            {QStringLiteral("color"), ref.bendColor}};
}

std::optional<DxfExportReference> readReference(const QCborMap &ref,
                                                const std::filesystem::path &project) {
    const auto path = ref.value(QStringLiteral("path")).toString();
    const auto clean = QDir::cleanPath(path);
    const auto layer = ref.value(QStringLiteral("layer")).toString();
    const auto color = ref.value(QStringLiteral("color")).toInteger();
    const auto bends = ref.value(QStringLiteral("includeBends"));
    const auto relative = ref.value(QStringLiteral("relative"));
    if (path.size() > 32768 || path.contains(QChar::Null) ||
        !clean.endsWith(QStringLiteral(".dxf"), Qt::CaseInsensitive) || !relative.isBool() ||
        (relative.toBool() &&
         (QDir::isAbsolutePath(clean) || clean.startsWith(QStringLiteral("../")))) ||
        !bends.isBool() || layer.size() > 63 || color < 1 || color > 255)
        return {};
    const auto resolved =
        relative.toBool() ? QFileInfo(project).absoluteDir().absoluteFilePath(clean) : clean;
    return DxfExportReference{QFileInfo(resolved).filesystemFilePath(), bends.toBool(),
                              layer.toStdString(), static_cast<int>(color)};
}
} // namespace

QCborMap writeProjectDxf(const ProjectDxf &dxf, const QCborMap &inputs,
                         const std::filesystem::path &project) {
    if (!dxf.flat || !dxf.flat->info().cutProfile || dxf.flat->info().cutProfile->loops.empty())
        return {};
    auto data = cutData(dxf.flat->info());
    if (data.isEmpty())
        return {};
    if (dxf.exported && QFileInfo(dxf.exported->path).isAbsolute())
        data.insert(QStringLiteral("export"), writeReference(*dxf.exported, project));
    const auto payload = QCborValue(data).toCbor();
    if (payload.size() > maximumBytes)
        return {};
    return {{QStringLiteral("version"), cacheVersion},
            {QStringLiteral("payload"), payload},
            {QStringLiteral("sha256"), fingerprint(inputs, payload)}};
}

ProjectDxf readProjectDxf(const QCborValue &value, const QCborMap &inputs,
                          const std::filesystem::path &project) {
    const auto record = value.toMap();
    if (record.value(QStringLiteral("version")).toInteger() != cacheVersion)
        return {};
    const auto payload = record.value(QStringLiteral("payload")).toByteArray();
    if (payload.isEmpty() || payload.size() > maximumBytes ||
        fingerprint(inputs, payload) != record.value(QStringLiteral("sha256")).toByteArray())
        return {};
    QCborParserError error;
    const auto data = QCborValue::fromCbor(payload, &error).toMap();
    if (error.error != QCborError::NoError || error.offset != payload.size())
        return {};
    auto flat = readCutData(data);
    if (!flat)
        return {};
    return {std::move(flat), readReference(data.value(QStringLiteral("export")).toMap(), project)};
}
} // namespace bending::project
