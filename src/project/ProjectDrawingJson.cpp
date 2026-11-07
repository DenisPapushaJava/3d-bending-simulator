#include "ProjectDrawingJson.h"
#include <QJsonArray>
#include <QJsonObject>

namespace bending::project {
namespace {
using namespace application;
QJsonArray groupJson(const std::vector<std::size_t> &indices) {
    QJsonArray values;
    for (const auto index : indices)
        values.append(static_cast<int>(index));
    return values;
}
bool readGroup(const QJsonValue &value, std::vector<std::size_t> &out) {
    if (!value.isArray() || value.toArray().size() > 512)
        return false;
    for (const auto &v : value.toArray()) {
        const int index = v.toInt(-1);
        if (index < 0 || index >= 512)
            return false;
        out.push_back(static_cast<std::size_t>(index));
    }
    return true;
}
} // namespace

QJsonValue drawingSettingsJson(const std::optional<ProjectDrawingSettings> &settings) {
    if (!settings)
        return QJsonValue::Null;
    const auto &s = *settings;
    QJsonArray edits, views;
    for (const auto &e : s.sectionEdits)
        edits.append(QJsonObject{
            {"bends", groupJson(e.bends)},
            {"fraction", e.fraction ? QJsonValue(*e.fraction) : QJsonValue(QJsonValue::Null)},
            {"reversed", e.reversed}});
    for (const auto &v : s.views) {
        QJsonArray groups;
        for (const auto &group : v.identity.groups)
            groups.append(groupJson(group));
        views.append(
            QJsonObject{{"kind", static_cast<int>(v.identity.kind)},
                        {"index", static_cast<int>(v.identity.index)},
                        {"groups", groups},
                        {"variant", v.identity.variant},
                        {"visible", v.visible},
                        {"scale", v.scale ? QJsonValue(*v.scale) : QJsonValue(QJsonValue::Null)}});
    }
    return QJsonObject{{"name", s.name ? QJsonValue(QString::fromStdString(*s.name))
                                       : QJsonValue(QJsonValue::Null)},
                       {"designation", QString::fromStdString(s.designation)},
                       {"material", QString::fromStdString(s.material)},
                       {"execution", QString::fromStdString(s.execution)},
                       {"quantity", s.quantity},
                       {"baseFace", s.baseFace ? QJsonValue(static_cast<int>(*s.baseFace))
                                               : QJsonValue(QJsonValue::Null)},
                       {"quarterTurns", s.quarterTurns},
                       {"reverseView", s.reverseView},
                       {"allowBreaks", s.allowBreaks},
                       {"flatSide", static_cast<int>(s.flatSide)},
                       {"sectionEdits", edits},
                       {"views", views}};
}

bool readDrawingSettings(const QJsonValue &value, std::optional<ProjectDrawingSettings> &out) {
    if (value.isNull()) {
        out.reset();
        return true;
    }
    if (!value.isObject())
        return false;
    const auto o = value.toObject();
    ProjectDrawingSettings s;
    const auto name = o.value("name");
    if (name.isString())
        s.name = name.toString().toStdString();
    else if (!name.isNull())
        return false;
    for (const auto &[key, field] : {std::pair{"designation", &s.designation},
                                     {"material", &s.material},
                                     {"execution", &s.execution}}) {
        if (!o.value(QLatin1String(key)).isString())
            return false;
        *field = o.value(QLatin1String(key)).toString().toStdString();
    }
    s.quantity = o.value("quantity").toInt(-1);
    const auto face = o.value("baseFace");
    if (!face.isNull()) {
        const int index = face.toInt(-1);
        if (index <= 0)
            return false;
        s.baseFace = static_cast<std::size_t>(index);
    }
    s.quarterTurns = o.value("quarterTurns").toInt(-2);
    if (!o.value("reverseView").isBool() || !o.value("allowBreaks").isBool())
        return false;
    s.reverseView = o.value("reverseView").toBool();
    s.allowBreaks = o.value("allowBreaks").toBool();
    const auto side = o.value("flatSide").toInt(-1);
    if (side < 0 || side > static_cast<int>(DrawingFlatSide::Opposite))
        return false;
    s.flatSide = static_cast<DrawingFlatSide>(side);
    const auto edits = o.value("sectionEdits");
    const auto views = o.value("views");
    if (!edits.isArray() || edits.toArray().size() > 128 || !views.isArray() ||
        views.toArray().size() > 1024)
        return false;
    for (const auto &item : edits.toArray()) {
        const auto e = item.toObject();
        DrawingSectionEdit edit;
        const auto fraction = e.value("fraction");
        if (!readGroup(e.value("bends"), edit.bends) || !e.value("reversed").isBool() ||
            (!fraction.isDouble() && !fraction.isNull()))
            return false;
        if (fraction.isDouble())
            edit.fraction = fraction.toDouble();
        edit.reversed = e.value("reversed").toBool();
        s.sectionEdits.push_back(std::move(edit));
    }
    for (const auto &item : views.toArray()) {
        const auto v = item.toObject();
        const int kind = v.value("kind").toInt(-1), index = v.value("index").toInt(-1);
        const auto groups = v.value("groups");
        const auto scale = v.value("scale");
        if (kind < 0 || kind > static_cast<int>(DrawingViewKind::Isometric) || index < 0 ||
            !v.value("variant").isBool() || !v.value("visible").isBool() || !groups.isArray() ||
            groups.toArray().size() > 128 || (!scale.isDouble() && !scale.isNull()))
            return false;
        ProjectDrawingView entry{{static_cast<DrawingViewKind>(kind),
                                  static_cast<std::size_t>(index),
                                  {},
                                  v.value("variant").toBool()},
                                 v.value("visible").toBool(),
                                 {}};
        for (const auto &group : groups.toArray()) {
            if (!readGroup(group, entry.identity.groups.emplace_back()))
                return false;
        }
        if (scale.isDouble())
            entry.scale = scale.toDouble();
        s.views.push_back(std::move(entry));
    }
    if (!validProjectDrawingSettings(s))
        return false;
    out = std::move(s);
    return true;
}
} // namespace bending::project
