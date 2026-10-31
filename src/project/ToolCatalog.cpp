#include "bending/project/ToolCatalog.h"
#include "ToolJson.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <limits>

namespace bending::project {
namespace {
QJsonValue number(const std::optional<double> &value) {
    return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}
bool readNumber(const QJsonValue &value, std::optional<double> &out) {
    if (value.isNull()) {
        out.reset();
        return true;
    }
    if (!value.isDouble()) {
        return false;
    }
    out = value.toDouble();
    return true;
}
ToolCatalogResult readFile(QFile &file) {
    constexpr qint64 limit = 4 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) {
        return std::string("Cannot read catalogue or size exceeds 4 MiB");
    }
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit) {
        return std::string("Cannot read catalogue");
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) {
        return std::string("Invalid catalogue JSON");
    }
    const auto root = json.object();
    if (root.value("format") != QStringLiteral("bending-tool-catalog") ||
        (root.value("version").toDouble(-1) != 1 && root.value("version").toDouble(-1) != 2 &&
         root.value("version").toDouble(-1) != 3 && root.value("version").toDouble(-1) != 4) ||
        !root.value("tools").isArray() || root.value("tools").toArray().size() > 512) {
        return std::string("Unsupported catalogue format/version");
    }
    core::ToolCatalog tools;
    for (const auto &item : root.value("tools").toArray()) {
        core::ToolPreset preset;
        if (!item.isObject() || !readPreset(item.toObject(), preset)) {
            return std::string("Invalid tool definition");
        }
        if (root.value("version").toInt() < 3 && !preset.grooveSettings.empty())
            return std::string("Groove orientations require catalogue version 3");
        if (root.value("version").toInt() < 4 && preset.segments)
            return std::string("Segment stock requires catalogue version 4");
        tools.push_back(std::move(preset));
    }
    if (!core::validToolCatalog(tools)) {
        return std::string("Invalid catalogue or duplicate tool IDs");
    }
    return tools;
}
} // namespace
QJsonObject presetJson(const core::ToolPreset &t) {
    QJsonValue segments(QJsonValue::Null);
    if (t.segments) {
        QJsonArray rows;
        for (const auto &s : *t.segments)
            rows.append(
                QJsonObject{{"lengthMm", s.lengthMm},
                            {"quantity", static_cast<int>(s.quantity)},
                            {"shape", s.shape == core::SegmentShape::Straight   ? "straight"
                                      : s.shape == core::SegmentShape::LeftHorn ? "left-horn"
                                                                                : "right-horn"}});
        segments = rows;
    }
    QJsonArray grooves;
    for (const auto &g : t.grooveSettings)
        grooves.append(
            QJsonObject{{"angleRadians", g.angleRadians}, {"rotationRadians", g.rotationRadians}});
    QJsonArray openings;
    for (double v : t.openingsMm) {
        openings.append(v);
    }
    QJsonValue drawing(QJsonValue::Null);
    if (t.drawing) {
        const auto point = [](core::DrawingPoint p) { return QJsonArray{p.x, p.y}; };
        QJsonArray edges;
        for (const auto &e : t.drawing->edges)
            edges.append(QJsonObject{
                {"start", point(e.start)},
                {"end", point(e.end)},
                {"center", e.center ? QJsonValue(point(*e.center)) : QJsonValue(QJsonValue::Null)},
                {"sweepRadians", e.sweepRadians}});
        drawing = QJsonObject{{"sourceFile", QString::fromStdString(t.drawing->sourceFile)},
                              {"sha256", QString::fromStdString(t.drawing->sha256)},
                              {"millimeters", t.drawing->millimeters},
                              {"edges", edges}};
    }
    return {{"id", QString::fromStdString(t.id)},
            {"manufacturer", QString::fromStdString(t.manufacturer)},
            {"code", QString::fromStdString(t.code)},
            {"kind", t.kind == core::ToolKind::Punch ? "punch" : "die"},
            {"angleRadians", t.angleRadians},
            {"lengthMm", t.lengthMm},
            {"heightMm", number(t.heightMm)},
            {"noseRadiusMm", number(t.noseRadiusMm)},
            {"openingsMm", openings},
            {"grooveSettings", grooves},
            {"source", QString::fromStdString(t.source)},
            {"notes", QString::fromStdString(t.notes)},
            {"teaching", t.teaching},
            {"parametersConfirmed", t.parametersConfirmed},
            {"drawing", drawing},
            {"segments", segments}};
}
bool readPreset(const QJsonObject &o, core::ToolPreset &t) {
    t.segments.reset();
    if (o.contains("segments") && !o.value("segments").isNull()) {
        if (!o.value("segments").isArray() || o.value("segments").toArray().size() > 64)
            return false;
        t.segments.emplace();
        for (const auto &value : o.value("segments").toArray()) {
            const auto s = value.toObject();
            const auto q = s.value("quantity").toDouble(-1);
            const auto kind = s.value("shape").toString();
            if (!value.isObject() || !s.value("lengthMm").isDouble() || q < 0 || q > 1000 ||
                std::floor(q) != q ||
                (kind != "straight" && kind != "left-horn" && kind != "right-horn"))
                return false;
            t.segments->push_back({s.value("lengthMm").toDouble(), static_cast<unsigned>(q),
                                   kind == "straight"    ? core::SegmentShape::Straight
                                   : kind == "left-horn" ? core::SegmentShape::LeftHorn
                                                         : core::SegmentShape::RightHorn});
        }
    }
    for (const auto &[name, destination] : {std::pair{"id", &t.id},
                                            {"manufacturer", &t.manufacturer},
                                            {"code", &t.code},
                                            {"source", &t.source},
                                            {"notes", &t.notes}}) {
        if (!o.value(QLatin1String(name)).isString()) {
            return false;
        }
        *destination = o.value(QLatin1String(name)).toString().toStdString();
    }
    const auto kind = o.value("kind").toString();
    if (kind != "punch" && kind != "die") {
        return false;
    }
    t.kind = kind == "punch" ? core::ToolKind::Punch : core::ToolKind::Die;
    if (!o.value("angleRadians").isDouble() || !o.value("lengthMm").isDouble() ||
        !o.value("teaching").isBool() || !o.value("openingsMm").isArray() ||
        !readNumber(o.value("heightMm"), t.heightMm) ||
        !readNumber(o.value("noseRadiusMm"), t.noseRadiusMm)) {
        return false;
    }
    t.angleRadians = o.value("angleRadians").toDouble();
    t.lengthMm = o.value("lengthMm").toDouble();
    t.teaching = o.value("teaching").toBool();
    t.parametersConfirmed = true;
    if (o.contains("parametersConfirmed")) {
        if (!o.value("parametersConfirmed").isBool())
            return false;
        t.parametersConfirmed = o.value("parametersConfirmed").toBool();
    }
    t.drawing.reset();
    if (o.contains("drawing") && !o.value("drawing").isNull()) {
        if (!o.value("drawing").isObject())
            return false;
        const auto d = o.value("drawing").toObject();
        if (!d.value("sourceFile").isString() || !d.value("sha256").isString() ||
            !d.value("millimeters").isBool() || !d.value("edges").isArray() ||
            d.value("edges").toArray().size() > 512)
            return false;
        core::ToolDrawing drawing;
        drawing.sourceFile = d.value("sourceFile").toString().toStdString();
        drawing.sha256 = d.value("sha256").toString().toStdString();
        drawing.millimeters = d.value("millimeters").toBool();
        const auto point = [](QJsonValue value, core::DrawingPoint &p) {
            const auto a = value.toArray();
            if (!value.isArray() || a.size() != 2 || !a[0].isDouble() || !a[1].isDouble())
                return false;
            p = {a[0].toDouble(), a[1].toDouble()};
            return true;
        };
        for (const auto &item : d.value("edges").toArray()) {
            const auto edge = item.toObject();
            core::DrawingEdge e;
            if (!item.isObject() || !point(edge.value("start"), e.start) ||
                !point(edge.value("end"), e.end) || !edge.value("sweepRadians").isDouble())
                return false;
            e.sweepRadians = edge.value("sweepRadians").toDouble();
            if (!edge.value("center").isNull()) {
                core::DrawingPoint c;
                if (!point(edge.value("center"), c))
                    return false;
                e.center = c;
            }
            drawing.edges.push_back(e);
        }
        if (!core::validToolDrawing(drawing))
            return false;
        t.drawing = std::move(drawing);
    }
    t.openingsMm.clear();
    t.grooveSettings.clear();
    if (o.contains("grooveSettings")) {
        if (!o.value("grooveSettings").isArray() || o.value("grooveSettings").toArray().size() > 16)
            return false;
        for (const auto &item : o.value("grooveSettings").toArray()) {
            const auto g = item.toObject();
            if (!item.isObject() || !g.value("angleRadians").isDouble() ||
                !g.value("rotationRadians").isDouble())
                return false;
            t.grooveSettings.push_back(
                {g.value("angleRadians").toDouble(), g.value("rotationRadians").toDouble()});
        }
    }
    if (o.value("openingsMm").toArray().size() > 16) {
        return false;
    }
    for (const auto &v : o.value("openingsMm").toArray()) {
        if (!v.isDouble()) {
            return false;
        }
        t.openingsMm.push_back(v.toDouble());
    }
    return core::validToolPreset(t);
}
QJsonValue assignmentJson(const std::optional<core::ToolAssignment> &a) {
    if (!a) {
        return QJsonValue(QJsonValue::Null);
    }
    const auto assembly = [](const std::optional<core::ToolAssembly> &value) {
        if (!value)
            return QJsonValue(QJsonValue::Null);
        QJsonArray rows;
        for (const auto row : value->pieces)
            rows.append(static_cast<int>(row));
        return QJsonValue(QJsonObject{{"pieces", rows}, {"gapMm", value->gapMm}});
    };
    return QJsonObject{{"punch", presetJson(a->punch)},
                       {"die", presetJson(a->die)},
                       {"groove", static_cast<int>(a->groove)},
                       {"clearanceMm", a->clearanceMm},
                       {"machineName", QString::fromStdString(a->machineName)},
                       {"machineModel", machineModelJson(a->machineModel)},
                       {"punchReversed", a->punchReversed},
                       {"useDrawingGeometry", a->useDrawingGeometry},
                       {"punchAssembly", assembly(a->punchAssembly)},
                       {"dieAssembly", assembly(a->dieAssembly)}};
}
QString machineModelJson(core::MachineModel model) {
    if (model == core::MachineModel::DemoPressV3)
        return QStringLiteral("demo-press-v3");
    if (model == core::MachineModel::DemoPressV2)
        return QStringLiteral("demo-press-v2");
    return model == core::MachineModel::DemoPressV1 ? QStringLiteral("demo-press-v1")
                                                    : QStringLiteral("none");
}
bool readMachineModel(const QJsonValue &value, core::MachineModel &model) {
    if (value.isString() && value.toString() == QStringLiteral("demo-press-v3")) {
        model = core::MachineModel::DemoPressV3;
        return true;
    }
    if (value.isUndefined() || (value.isString() && value.toString() == QStringLiteral("none"))) {
        model = core::MachineModel::None;
        return true;
    }
    if (value.isString() && value.toString() == QStringLiteral("demo-press-v1")) {
        model = core::MachineModel::DemoPressV1;
        return true;
    }
    if (value.isString() && value.toString() == QStringLiteral("demo-press-v2")) {
        model = core::MachineModel::DemoPressV2;
        return true;
    }
    return false;
}
bool readAssignment(const QJsonValue &value, std::optional<core::ToolAssignment> &assignment) {
    if (value.isNull()) {
        assignment.reset();
        return true;
    }
    if (!value.isObject()) {
        return false;
    }
    const auto o = value.toObject();
    core::ToolAssignment a;
    if (!readMachineModel(o.value("machineModel"), a.machineModel))
        return false;
    if (o.contains("punchReversed") && !o.value("punchReversed").isBool())
        return false;
    a.punchReversed = o.value("punchReversed").toBool(false);
    const auto readAssembly = [](const QJsonValue &v, std::optional<core::ToolAssembly> &out) {
        if (v.isNull() || v.isUndefined())
            return true;
        if (!v.isObject() || !v.toObject().value("pieces").isArray() ||
            v.toObject().value("pieces").toArray().isEmpty() ||
            v.toObject().value("pieces").toArray().size() > 512 ||
            !v.toObject().value("gapMm").isDouble())
            return false;
        out.emplace();
        out->gapMm = v.toObject().value("gapMm").toDouble();
        for (const auto &row : v.toObject().value("pieces").toArray()) {
            const double n = row.toDouble(-1);
            if (n < 0 || n >= 64 || std::floor(n) != n)
                return false;
            out->pieces.push_back(static_cast<std::size_t>(n));
        }
        return true;
    };
    if (!readAssembly(o.value("punchAssembly"), a.punchAssembly) ||
        !readAssembly(o.value("dieAssembly"), a.dieAssembly))
        return false;
    const double groove = o.value("groove").toDouble(-1);
    if (!o.value("punch").isObject() || !o.value("die").isObject() ||
        !readPreset(o.value("punch").toObject(), a.punch) ||
        !readPreset(o.value("die").toObject(), a.die) || !std::isfinite(groove) || groove < 0 ||
        groove >= 16 || std::floor(groove) != groove || !o.value("clearanceMm").isDouble() ||
        !o.value("machineName").isString()) {
        return false;
    }
    if (o.contains("useDrawingGeometry") && !o.value("useDrawingGeometry").isBool())
        return false;
    a.useDrawingGeometry = o.value("useDrawingGeometry").toBool(false);
    a.groove = static_cast<std::size_t>(groove);
    a.clearanceMm = o.value("clearanceMm").toDouble();
    a.machineName = o.value("machineName").toString().toStdString();
    if (!core::toolParameters(a)) {
        return false;
    }
    assignment = std::move(a);
    return true;
}
ToolCatalogResult readToolCatalog(const std::filesystem::path &path) {
#ifdef _WIN32
    QFile file(QString::fromStdWString(path.native()));
#else
    QFile file(QString::fromUtf8(path.native().c_str()));
#endif
    return readFile(file);
}
std::optional<std::string> writeToolCatalog(const std::filesystem::path &path,
                                            const core::ToolCatalog &catalog) {
    if (!core::validToolCatalog(catalog))
        return "Некорректный каталог или повторяющиеся идентификаторы.";
    QJsonArray tools;
    for (const auto &tool : catalog)
        tools.append(presetJson(tool));
    const auto bytes =
        QJsonDocument(
            QJsonObject{{"format", "bending-tool-catalog"}, {"version", 4}, {"tools", tools}})
            .toJson();
    if (bytes.size() > 4 * 1024 * 1024)
        return "Библиотека превышает 4 МиБ. Инструмент не добавлен.";
#ifdef _WIN32
    QSaveFile file(QString::fromStdWString(path.native()));
#else
    QSaveFile file(QString::fromUtf8(path.native().c_str()));
#endif
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return "Не удалось сохранить библиотеку. Проверьте доступ к папке настроек.";
    return {};
}
} // namespace bending::project
