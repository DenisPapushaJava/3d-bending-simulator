#include "bending/project/DxfToolImporter.h"
#include "bending/cad/ToolProfileValidation.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace bending::project {
namespace {
using namespace core;
using namespace application;
constexpr double pi = std::numbers::pi;
struct Tag {
    int code;
    QByteArray value;
};
struct Record {
    QByteArray type;
    std::vector<Tag> tags;
};
struct Layer {
    std::vector<DrawingEdge> edges;
    std::optional<double> z;
    std::string error;
};
[[noreturn]] void fail(const char *text) {
    throw std::runtime_error(text);
}
double number(const QByteArray &s) {
    bool ok = false;
    const double value = s.toDouble(&ok);
    if (!ok || !std::isfinite(value) || std::abs(value) > 1e6)
        fail("Некорректное или слишком большое число в DXF.");
    return value;
}
double value(const Record &r, int code, std::optional<double> fallback = {}) {
    std::optional<double> result;
    for (const auto &tag : r.tags)
        if (tag.code == code) {
            if (result)
                fail("Повторяющийся одиночный параметр DXF.");
            result = number(tag.value);
        }
    if (!result && !fallback)
        fail("В DXF не хватает координат или параметров.");
    return result.value_or(fallback.value_or(0));
}
int flags(const Record &r, int code = 70, int fallback = 0) {
    const double v = value(r, code, fallback);
    if (v < 0 || v > 65535 || std::floor(v) != v)
        fail("Некорректные флаги DXF.");
    return static_cast<int>(v);
}
std::string layerName(const Record &r) {
    for (const auto &t : r.tags)
        if (t.code == 8)
            return t.value.toStdString();
    return "0";
}
void plane(const Record &r) {
    if (value(r, 210, 0) != 0 || value(r, 220, 0) != 0 || value(r, 230, 1) != 1 ||
        value(r, 39, 0) != 0)
        fail("Поддерживается плоское сечение XY без толщины сущностей и нестандартного OCS.");
}
void elevation(Layer &layer, double z) {
    if (layer.z && std::abs(*layer.z - z) > 1e-7)
        fail("Геометрия слоя не лежит в одной плоскости XY.");
    layer.z = z;
}
double distance(DrawingPoint a, DrawingPoint b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
DrawingEdge segment(DrawingPoint a, DrawingPoint b, double bulge = 0) {
    if (distance(a, b) < 1e-8)
        fail("Нулевое ребро в сечении DXF.");
    DrawingEdge e{a, b, {}, 0};
    if (bulge != 0) {
        const double k = (1 - bulge * bulge) / (4 * bulge);
        e.center = {(a.x + b.x) / 2 - (b.y - a.y) * k, (a.y + b.y) / 2 + (b.x - a.x) * k};
        e.sweepRadians = 4 * std::atan(bulge);
    }
    return e;
}
struct Vertex {
    DrawingPoint p;
    double bulge = 0;
    bool y = false;
    bool hasBulge = false;
};
void polyline(Layer &layer, const std::vector<Vertex> &vertices, bool closed) {
    if (vertices.size() < 2 || vertices.size() > 512)
        fail("В полилинии нужно от 2 до 512 вершин.");
    const auto count = closed ? vertices.size() : vertices.size() - 1;
    for (std::size_t i = 0; i < count; ++i) {
        if (!vertices[i].y)
            fail("Отсутствует Y вершины полилинии.");
        layer.edges.push_back(
            segment(vertices[i].p, vertices[(i + 1) % vertices.size()].p, vertices[i].bulge));
    }
    if (!vertices.back().y)
        fail("Отсутствует Y вершины полилинии.");
}
std::vector<Tag> tags(const QByteArray &bytes) {
    if (bytes.contains('\0') || bytes.startsWith("AutoCAD Binary DXF"))
        fail("Нужен текстовый ASCII DXF. Сохраните сечение из CAD в этом формате.");
    auto lines = bytes.split('\n');
    if (!lines.empty() && lines.back().trimmed().isEmpty())
        lines.removeLast();
    if (lines.size() % 2 || lines.size() > 400000)
        fail("Неверная структура или слишком большой DXF.");
    std::vector<Tag> result;
    for (qsizetype i = 0; i < lines.size(); i += 2) {
        bool ok = false;
        const int code = lines[i].trimmed().toInt(&ok);
        if (!ok || code < 0 || code > 1071)
            fail("Некорректный код группы DXF.");
        result.push_back({code, lines[i + 1].trimmed()});
    }
    return result;
}
std::vector<Record> records(const std::vector<Tag> &all, ToolProfileFile &out) {
    QByteArray section;
    std::vector<Record> result;
    bool eof = false;
    bool unitsSeen = false;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto &t = all[i];
        if (t.code == 0 && t.value == "SECTION") {
            if (!section.isEmpty() || ++i == all.size() || all[i].code != 2)
                fail("Неверная секция DXF.");
            section = all[i].value;
        } else if (t.code == 0 && t.value == "ENDSEC") {
            if (section.isEmpty())
                fail("Неверное завершение секции DXF.");
            section.clear();
        } else if (t.code == 0 && t.value == "EOF") {
            if (!section.isEmpty() || i + 1 != all.size())
                fail("Неверное завершение DXF.");
            eof = true;
        } else if (section == "HEADER" && t.code == 9 && t.value == "$INSUNITS") {
            if (unitsSeen)
                fail("Единицы DXF указаны более одного раза.");
            unitsSeen = true;
            if (++i == all.size() || all[i].code != 70)
                fail("Неверные единицы DXF.");
            const auto unit = number(all[i].value);
            if (unit == 4)
                out.millimetersPerUnit = 1;
            else if (unit == 1)
                out.millimetersPerUnit = 25.4;
            else if (unit == 5)
                out.millimetersPerUnit = 10;
            else if (unit == 6)
                out.millimetersPerUnit = 1000;
            else
                out.notices.push_back("Единицы в DXF не заданы или не поддержаны.");
        } else if (section == "ENTITIES") {
            if (t.code == 0) {
                if (result.size() >= 20000)
                    fail("Слишком много сущностей DXF.");
                result.push_back({t.value, {}});
            } else {
                if (result.empty())
                    fail("Сущность DXF без типа.");
                result.back().tags.push_back(t);
            }
        }
    }
    if (!eof || !section.isEmpty())
        fail("DXF оборван: не найдены ENDSEC/EOF.");
    return result;
}
void entities(const std::vector<Record> &rs, std::map<std::string, Layer> &layers,
              ToolProfileFile &out, std::stop_token stop) {
    std::size_t total = 0, annotations = 0;
    for (std::size_t i = 0; i < rs.size(); ++i) {
        if (stop.stop_requested())
            fail("Импорт отменён.");
        const auto &r = rs[i];
        const auto name = layerName(r);
        auto &layer = layers[name];
        const auto before = layer.edges.size();
        try {
            if (value(r, 67, 0) != 0) {
                if (r.type == "POLYLINE") {
                    while (i + 1 < rs.size() && rs[i + 1].type == "VERTEX")
                        ++i;
                    if (i + 1 < rs.size() && rs[i + 1].type == "SEQEND")
                        ++i;
                }
                ++annotations;
                continue;
            }
            if (r.type == "TEXT" || r.type == "MTEXT" || r.type == "DIMENSION" ||
                r.type == "POINT" || r.type == "ATTDEF") {
                ++annotations;
                continue;
            }
            plane(r);
            if (r.type == "LINE") {
                elevation(layer, value(r, 30, 0));
                elevation(layer, value(r, 31, 0));
                layer.edges.push_back(
                    segment({value(r, 10), value(r, 20)}, {value(r, 11), value(r, 21)}));
            } else if (r.type == "ARC" || r.type == "CIRCLE") {
                elevation(layer, value(r, 30, 0));
                const DrawingPoint c{value(r, 10), value(r, 20)};
                const double radius = value(r, 40);
                if (radius <= 0)
                    fail("Нулевой радиус дуги DXF.");
                const auto at = [&](double a) {
                    return DrawingPoint{c.x + radius * std::cos(a), c.y + radius * std::sin(a)};
                };
                if (r.type == "CIRCLE") {
                    layer.edges.push_back({at(0), at(pi), c, pi});
                    layer.edges.push_back({at(pi), at(0), c, pi});
                } else {
                    const double start = value(r, 50) * pi / 180;
                    double sweep = std::fmod(value(r, 51) - value(r, 50), 360.);
                    if (sweep < 0)
                        sweep += 360;
                    if (sweep < 1e-9)
                        fail("Нулевая дуга DXF.");
                    sweep *= pi / 180;
                    layer.edges.push_back({at(start), at(start + sweep), c, sweep});
                }
            } else if (r.type == "LWPOLYLINE") {
                if ((flags(r) & ~129) != 0 || value(r, 43, 0) != 0)
                    fail("Неподдержанная полилиния или её ширина.");
                elevation(layer, value(r, 38, 0));
                std::vector<Vertex> vs;
                for (const auto &t : r.tags) {
                    if (t.code == 10)
                        vs.push_back({{number(t.value), 0}, 0, false});
                    else if (t.code == 20 || t.code == 42) {
                        if (vs.empty())
                            fail("Неверный порядок координат полилинии.");
                        if (t.code == 20) {
                            if (vs.back().y)
                                fail("Повтор Y вершины.");
                            vs.back().p.y = number(t.value);
                            vs.back().y = true;
                        } else {
                            if (vs.back().hasBulge)
                                fail("Повтор bulge вершины.");
                            vs.back().bulge = number(t.value);
                            vs.back().hasBulge = true;
                        }
                    } else if ((t.code == 40 || t.code == 41) && number(t.value) != 0)
                        fail("Широкая полилиния не является контуром сечения.");
                }
                if (value(r, 90) != static_cast<double>(vs.size()))
                    fail("Число вершин DXF не совпадает с записью.");
                polyline(layer, vs, (flags(r) & 1) != 0);
            } else if (r.type == "POLYLINE") {
                if ((flags(r) & ~129) != 0 || value(r, 40, 0) != 0 || value(r, 41, 0) != 0)
                    fail("Нужна простая 2D-полилиния без ширины.");
                elevation(layer, value(r, 30, 0));
                std::vector<Vertex> vs;
                while (++i < rs.size() && rs[i].type == "VERTEX") {
                    const auto &v = rs[i];
                    plane(v);
                    if (flags(v) != 0 || value(v, 30, 0) != 0 || value(v, 40, 0) != 0 ||
                        value(v, 41, 0) != 0)
                        fail("Неподдержанные параметры вершины POLYLINE.");
                    if (layerName(v) != name && layerName(v) != "0")
                        fail("Вершины полилинии на разных слоях.");
                    vs.push_back({{value(v, 10), value(v, 20)}, value(v, 42, 0), true});
                    if (vs.size() > 512)
                        fail("Слишком много вершин полилинии.");
                }
                if (i == rs.size() || rs[i].type != "SEQEND")
                    fail("Полилиния DXF оборвана.");
                polyline(layer, vs, (flags(r) & 1) != 0);
            } else {
                layer.error = "Неподдержанная сущность: " + r.type.toStdString() +
                              ". Экспортируйте контур линиями/дугами.";
            }
        } catch (const std::exception &e) {
            layer.error = e.what();
        }
        total += layer.edges.size() - before;
        if (total > 4096)
            fail("Слишком много рёбер DXF.");
    }
    if (annotations)
        out.notices.push_back("Подписи, размеры, точки и листы оформления пропущены: " +
                              std::to_string(annotations) + ".");
}
void contours(const std::string &name, const Layer &layer, ToolProfileFile &out,
              const std::string &source, const std::string &hash, std::stop_token stop) {
    if (!layer.error.empty()) {
        out.notices.push_back("Слой «" + name + "» исключён: " + layer.error);
        return;
    }
    std::vector<DrawingPoint> nodes;
    std::vector<std::vector<std::size_t>> adjacent;
    std::vector<std::pair<std::size_t, std::size_t>> ends;
    const auto node = [&](DrawingPoint p) {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (distance(p, nodes[i]) <= 1e-6)
                return i;
        nodes.push_back(p);
        adjacent.emplace_back();
        return nodes.size() - 1;
    };
    for (std::size_t i = 0; i < layer.edges.size(); ++i) {
        const auto a = node(layer.edges[i].start), b = node(layer.edges[i].end);
        ends.emplace_back(a, b);
        adjacent[a].push_back(i);
        adjacent[b].push_back(i);
    }
    std::set<std::size_t> unseen;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        unseen.insert(i);
    std::size_t skipped = 0;
    while (!unseen.empty()) {
        if (stop.stop_requested())
            fail("Импорт отменён.");
        const auto first = *unseen.begin();
        std::vector<std::size_t> stack{first};
        std::set<std::size_t> group;
        while (!stack.empty()) {
            const auto n = stack.back();
            stack.pop_back();
            if (!group.insert(n).second)
                continue;
            unseen.erase(n);
            for (auto e : adjacent[n]) {
                stack.push_back(ends[e].first);
                stack.push_back(ends[e].second);
            }
        }
        if (!std::all_of(group.begin(), group.end(),
                         [&](auto n) { return adjacent[n].size() == 2; }) ||
            group.size() < 2 || group.size() > 512) {
            ++skipped;
            continue;
        }
        ToolDrawing drawing{source, hash, false, {}};
        std::set<std::size_t> used;
        auto current = first;
        do {
            const auto &a = adjacent[current];
            const auto e = used.contains(a[0]) ? a[1] : a[0];
            if (!used.insert(e).second)
                fail("Неверная связность контура DXF.");
            auto edge = layer.edges[e];
            if (ends[e].first != current) {
                std::swap(edge.start, edge.end);
                edge.sweepRadians = -edge.sweepRadians;
            }
            current = ends[e].first == current ? ends[e].second : ends[e].first;
            drawing.edges.push_back(edge);
        } while (current != first);
        const auto origin = drawing.edges.front().start;
        for (auto &edge : drawing.edges) {
            const auto shift = [&](DrawingPoint &p) {
                p.x -= origin.x;
                p.y -= origin.y;
            };
            shift(edge.start);
            shift(edge.end);
            if (edge.center)
                shift(*edge.center);
        }
        if (!validToolDrawing(drawing)) {
            ++skipped;
            continue;
        }
        if (out.contours.size() >= 64)
            fail("Найдено более 64 контуров. Экспортируйте отдельное сечение.");
        out.contours.push_back({name, std::move(drawing)});
    }
    if (skipped)
        out.notices.push_back(
            "Слой «" + name +
            "»: открытых, разветвлённых или некорректных компонентов пропущено: " +
            std::to_string(skipped) + ".");
}
ToolProfileResult read(const std::filesystem::path &path, std::stop_token stop) {
#ifdef _WIN32
    QFile file(QString::fromStdWString(path.native()));
#else
    QFile file(QString::fromUtf8(path.native().c_str()));
#endif
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 4 * 1024 * 1024)
        fail("Не удалось прочитать DXF. Максимальный размер — 4 МиБ.");
    const auto bytes = file.read(4 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 4 * 1024 * 1024)
        fail("Ошибка чтения DXF.");
    ToolProfileFile result;
    const auto rs = records(tags(bytes), result);
    std::map<std::string, Layer> layers;
    entities(rs, layers, result, stop);
    const auto hash =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
    const auto source = QFileInfo(file).fileName().toStdString();
    for (const auto &[name, layer] : layers)
        contours(name, layer, result, source, hash, stop);
    if (result.contours.empty()) {
        std::string error = "Не найден замкнутый контур сечения. Нужны LINE/ARC/CIRCLE или простые "
                            "2D POLYLINE/LWPOLYLINE.";
        for (const auto &notice : result.notices)
            error += '\n' + notice;
        return ToolProfileError{error};
    }
    return result;
}
} // namespace
application::ToolProfileResult DxfToolImporter::run(const application::ToolProfileInput &input,
                                                    std::stop_token stop) const {
    try {
        if (stop.stop_requested())
            return application::ToolProfileError{"Импорт отменён."};
        if (const auto *path = std::get_if<std::filesystem::path>(&input))
            return read(*path, stop);
        if (auto error = cad::validateToolProfile(std::get<core::ToolDrawing>(input), stop))
            return application::ToolProfileError{*error};
        return application::CheckedToolProfile{};
    } catch (const std::exception &e) {
        return application::ToolProfileError{e.what()};
    }
}
} // namespace bending::project
