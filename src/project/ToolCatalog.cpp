#include "bending/project/ToolCatalog.h"
#include "ToolJson.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QResource>
#include <cmath>
#include <limits>

static void initializeToolCatalog() {
    Q_INIT_RESOURCE(tool_catalog);
}
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
        (root.value("version").toDouble(-1) != 1 && root.value("version").toDouble(-1) != 2) ||
        !root.value("tools").isArray() || root.value("tools").toArray().size() > 512) {
        return std::string("Unsupported catalogue format/version");
    }
    core::ToolCatalog tools;
    for (const auto &item : root.value("tools").toArray()) {
        core::ToolPreset preset;
        if (!item.isObject() || !readPreset(item.toObject(), preset)) {
            return std::string("Invalid tool definition");
        }
        tools.push_back(std::move(preset));
    }
    if (!core::validToolCatalog(tools)) {
        return std::string("Invalid catalogue or duplicate tool IDs");
    }
    return tools;
}
} // namespace
QJsonObject presetJson(const core::ToolPreset &t) {
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
            {"source", QString::fromStdString(t.source)},
            {"notes", QString::fromStdString(t.notes)},
            {"teaching", t.teaching},
            {"parametersConfirmed", t.parametersConfirmed},
            {"drawing", drawing}};
}
bool readPreset(const QJsonObject &o, core::ToolPreset &t) {
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
    return QJsonObject{{"punch", presetJson(a->punch)},
                       {"die", presetJson(a->die)},
                       {"groove", static_cast<int>(a->groove)},
                       {"clearanceMm", a->clearanceMm},
                       {"machineName", QString::fromStdString(a->machineName)}};
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
    const double groove = o.value("groove").toDouble(-1);
    if (!o.value("punch").isObject() || !o.value("die").isObject() ||
        !readPreset(o.value("punch").toObject(), a.punch) ||
        !readPreset(o.value("die").toObject(), a.die) || !std::isfinite(groove) || groove < 0 ||
        groove >= 16 || std::floor(groove) != groove || !o.value("clearanceMm").isDouble() ||
        !o.value("machineName").isString()) {
        return false;
    }
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
ToolCatalogResult builtInToolCatalog() {
    static const bool initialized = [] {
        initializeToolCatalog();
        return true;
    }();
    (void)initialized;
    QFile file(QStringLiteral(":/bending/tool-catalog.json"));
    return readFile(file);
}
} // namespace bending::project
