#include "bending/project/DxfToolImporter.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
QByteArray tag(int code, double v) {
    return QByteArray::number(code) + '\n' + QByteArray::number(v, 'g', 16) + '\n';
}
QByteArray block(const QByteArray &name, const QByteArray &contents, double x = 0, double y = 0,
                 double z = 0, int flags = 0) {
    return "0\nBLOCK\n2\n" + name + '\n' + tag(70, flags) + tag(10, x) + tag(20, y) + tag(30, z) +
           contents + "0\nENDBLK\n";
}
QByteArray insert(const QByteArray &name, double x = 0, double y = 0,
                  const QByteArray &extra = {}) {
    return "0\nINSERT\n2\n" + name + '\n' + tag(10, x) + tag(20, y) + extra;
}
QByteArray wrap(const QByteArray &blocks, const QByteArray &entities, int units = 4) {
    return "0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n" + tag(70, units) +
           "0\nENDSEC\n0\nSECTION\n2\nBLOCKS\n" + blocks + "0\nENDSEC\n0\nSECTION\n2\nENTITIES\n" +
           entities + "0\nENDSEC\n0\nEOF\n";
}
const QByteArray triangle =
    "0\nLWPOLYLINE\n90\n3\n70\n1\n10\n0\n20\n0\n10\n10\n20\n0\n10\n0\n20\n5\n";
QByteArray arc(double x, double y, double radius, double start, double end, double z = 0) {
    return "0\nARC\n" + tag(10, x) + tag(20, y) + tag(30, z) + tag(40, radius) + tag(50, start) +
           tag(51, end);
}
QByteArray line(double x1, double y1, double x2, double y2, double z = 0) {
    return "0\nLINE\n" + tag(10, x1) + tag(20, y1) + tag(30, z) + tag(11, x2) + tag(21, y2) +
           tag(31, z);
}
void equalPoint(core::DrawingPoint a, core::DrawingPoint b) {
    require(std::hypot(a.x - b.x, a.y - b.y) < 1e-10, "Transformed coordinates changed");
}
void equalEdges(const core::ToolDrawing &a, const core::ToolDrawing &b) {
    require(a.edges.size() == b.edges.size(), "Edge count changed");
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        const auto &x = a.edges[i], &y = b.edges[i];
        equalPoint(x.start, y.start);
        equalPoint(x.end, y.end);
        require(x.center.has_value() == y.center.has_value(), "Arc was flattened");
        if (x.center) {
            equalPoint(*x.center, *y.center);
            require(std::abs(x.sweepRadians - y.sweepRadians) < 1e-12,
                    "Reflected arc sweep changed");
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "No temporary folder");
        const auto path = dir.path() + "/blocks.dxf";
#ifdef _WIN32
        const std::filesystem::path native(path.toStdWString());
#else
        const std::filesystem::path native(path.toUtf8().constData());
#endif
        project::DxfToolImporter importer;
        const auto run = [&](const QByteArray &bytes) {
            QFile f(path);
            require(f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                        f.write(bytes) == bytes.size(),
                    "Fixture write failed");
            f.close();
            return importer.run(native, {});
        };
        const auto load = [&](const QByteArray &bytes) {
            auto result = run(bytes);
            if (const auto *e = std::get_if<application::ToolProfileError>(&result))
                throw std::runtime_error(e->message);
            return std::get<application::ToolProfileFile>(std::move(result));
        };
        const auto checked = [&](core::ToolDrawing drawing) {
            drawing.millimeters = true;
            require(
                std::holds_alternative<application::CheckedToolProfile>(importer.run(drawing, {})),
                "Transformed contour does not produce a valid solid");
        };
        const auto rejected = [&](const QByteArray &bytes, const char *reason) {
            auto r = run(bytes);
            const auto *e = std::get_if<application::ToolProfileError>(&r);
            require(e && e->message.find(reason) != std::string::npos,
                    "Unsupported block was accepted or failed for a different reason");
        };
        const auto definitions = block("A", {}) + block("Tool", triangle);
        const auto original = wrap(definitions, insert("A") + insert("tOoL"), 0);
        auto f = load(original);
        require(!f.millimetersPerUnit && f.contours.size() == 1,
                "Empty block or unknown units mishandled");
        equalEdges(f.contours[0].drawing, load(wrap({}, triangle)).contours[0].drawing);
        checked(f.contours[0].drawing);
        require(f.contours[0].drawing.sha256 ==
                    QCryptographicHash::hash(original, QCryptographicHash::Sha256)
                        .toHex()
                        .toStdString(),
                "SHA no longer identifies the original block-based file");
        require(f.contours[0].drawing.sourceFile == "blocks.dxf", "Source basename lost");

        // A root-level closing line proves base point, insertion, and elevation, which
        // normalizing a complete isolated contour would otherwise hide.
        const auto half = block("Half", arc(2, 3, 2, 0, 180, 5), 2, 3, 5);
        f = load(
            wrap(half, insert("Half", 10, 20, tag(30, 7) + tag(41, 2) + tag(42, 2) + tag(50, 90)) +
                           line(10, 16, 10, 24, 7)));
        require(f.contours.size() == 1, "Positioned block did not join the root edge");
        equalEdges(f.contours[0].drawing,
                   load(wrap({}, arc(10, 20, 4, 90, 270, 7) + line(10, 16, 10, 24, 7)))
                       .contours[0]
                       .drawing);
        checked(f.contours[0].drawing);

        const auto nested =
            half + block("Outer",
                         insert("Half", 4, 6,
                                tag(30, 8) + tag(41, -2) + tag(42, 2) + tag(43, 2) + tag(50, 90)),
                         1, 2, 3);
        f = load(
            wrap(nested, insert("Outer", 30, 40,
                                tag(30, 9) + tag(41, 3) + tag(42, 3) + tag(43, 2) + tag(50, 90)) +
                             line(6, 49, 30, 49, 19)));
        const auto &e = f.contours.at(0).drawing.edges.at(0);
        equalPoint(e.start, {0, 0});
        equalPoint(e.end, {-24, 0});
        require(e.center && std::abs(e.sweepRadians + std::numbers::pi) < 1e-12,
                "Nested reflection did not reverse the analytic arc");
        equalPoint(*e.center, {-12, 0});
        checked(f.contours[0].drawing);
        // -Z OCS reverses X and Z but preserves Y, even with a rotated block.
        f = load(wrap(half, insert("Half", 10, 20, tag(230, -1) + tag(30, 7) + tag(50, 90)) +
                                line(-10, 18, -10, 22, -7)));
        require(f.contours.size() == 1, "Negative extrusion OCS misplaced the insert");
        checked(f.contours[0].drawing);
        require(f.contours[0].drawing.edges[0].sweepRadians < 0, "OCS reflection lost");

        const auto circle = "0\nCIRCLE\n10\n0\n20\n0\n40\n5\n";
        f = load(wrap(block("C", circle), insert("C", 0, 0, tag(41, -2) + tag(42, 2))));
        require(f.contours[0].drawing.edges.size() == 2, "Circle lost its analytic edges");
        checked(f.contours[0].drawing);
        const QByteArray old = "0\nPOLYLINE\n70\n1\n0\nVERTEX\n10\n0\n20\n0\n0\nVERTEX\n10\n10\n20"
                               "\n0\n0\nVERTEX\n10\n0\n20\n5\n0\nSEQEND\n";
        f = load(wrap(block("Old", old), insert("Old", 0, 0, "8\nINHERITED\n")));
        require(f.contours.at(0).layer == "INHERITED", "Old polyline layer not inherited");
        checked(f.contours[0].drawing);
        const QByteArray bulge =
            "0\nLWPOLYLINE\n90\n2\n70\n1\n10\n2\n20\n0\n42\n1\n10\n-2\n20\n0\n";
        f = load(wrap(block("B", bulge), insert("B", 0, 0, tag(41, -1) + tag(42, 1))));
        require(f.contours[0].drawing.edges[0].sweepRadians < 0, "Bulge reflection lost");
        checked(f.contours[0].drawing);

        auto explicitLayer = triangle;
        explicitLayer.replace("0\nLWPOLYLINE\n", "0\nLWPOLYLINE\n8\nEXPLICIT\n");
        const auto layers = block("T", triangle + explicitLayer) + block("N", insert("T"));
        f = load(wrap(layers, insert("N", 0, 0, "8\nINHERITED\n")));
        require(f.contours.size() == 2 && f.contours[0].layer == "EXPLICIT" &&
                    f.contours[1].layer == "INHERITED",
                "Nested layer inheritance failed");
        f = load(wrap(block("T", triangle), insert("T") + insert("T", 30, 0)));
        require(f.contours.size() == 2, "Repeated block occurrences merged");
        f = load(wrap(block("Unused", "0\nSPLINE\n"),
                      triangle + insert("Missing", 0, 0, "67\n1\n66\n1\n") +
                          "0\nATTRIB\n1\nLabel\n0\nSEQEND\n"));
        require(f.contours.size() == 1, "Unused definition or paper-space INSERT affected body");
        f = load(wrap(block("T", triangle + "0\nATTDEF\n1\nName\n"),
                      insert("T", 0, 0, "66\n1\n") + "0\nATTRIB\n1\nValue\n0\nSEQEND\n"));
        require(f.contours.size() == 1, "Attached text attributes polluted the contour");
        f = load(wrap(block("Профиль", triangle), insert("профиль")));
        require(f.contours.size() == 1, "UTF-8 block lookup failed");
        f = load(wrap("999\nBlock definitions\n" + block("T", triangle), insert("T")));
        require(f.contours.size() == 1, "DXF comment interpreted as an entity");

        // The valid unrelated contour must never hide an invalid referenced block.
        rejected(wrap({}, triangle + insert("Missing")), "Не найден блок");
        rejected(wrap(block("A", triangle) + block("a", triangle), insert("A")),
                 "Повторяющееся определение");
        rejected(wrap(block("A", insert("A")), triangle + insert("A")), "Циклическая");
        rejected(wrap(block("A", insert("B")) + block("B", insert("A")), insert("A")),
                 "Циклическая");
        rejected(wrap(block("A", triangle + "0\nSPLINE\n8\nOTHER\n"), triangle + insert("A")),
                 "Неподдержанная сущность");
        rejected(wrap(block("A", triangle), triangle + insert("A", 0, 0, tag(41, 2))),
                 "равномерный масштаб");
        rejected(wrap(block("A", triangle), insert("A", 0, 0, tag(41, 0))), "масштаб");
        rejected(wrap(block("A", triangle), insert("A", 0, 0, tag(210, 1))), "нормалью");
        rejected(wrap(block("A", triangle, 0, 0, 0, 4), triangle + insert("A")), "XREF");
        rejected(wrap(block("A", triangle), triangle + insert("A", 0, 0, "360\nABC\n")), "XCLIP");
        rejected(wrap(block("A", triangle), triangle + insert("A", 0, 0, tag(70, 2))), "MINSERT");
        rejected(wrap(block("A", triangle), insert("A", 0, 0, "66\n1\n")), "SEQEND");
        rejected(wrap("0\nBLOCK\n2\nA\n" + triangle, insert("A")), "ENDBLK");
        rejected(wrap("0\nENDBLK\n", triangle), "ENDBLK");
        rejected(wrap(block("A", triangle), insert("A", 1e6, 0, tag(41, 1e6) + tag(42, 1e6))),
                 "диапазон");
        rejected(wrap(half, insert("Half", 10, 20, tag(30, 7)) + line(8, 20, 12, 20, 8)),
                 "одной плоскости");
        QByteArray deep;
        for (int i = 0; i < 17; ++i)
            deep += block(QByteArray::number(i),
                          i == 16 ? triangle : insert(QByteArray::number(i + 1)));
        rejected(wrap(deep, insert("0")), "предел вложенности");
        // A tiny source can otherwise expand exponentially, including empty inserts.
        QByteArray bomb = block("0", {});
        for (int i = 1; i < 14; ++i)
            bomb += block(QByteArray::number(i),
                          insert(QByteArray::number(i - 1)) + insert(QByteArray::number(i - 1)));
        rejected(wrap(bomb, triangle + insert("13")), "предел вложенности или числа");
        QByteArray many;
        for (int i = 0; i < 4097; ++i)
            many += insert("E");
        rejected(wrap(block("E", {}), triangle + many), "предел вложенности или числа");
        QByteArray repeated;
        for (int i = 0; i < 1400; ++i)
            repeated += insert("T", 30 * i, 0);
        rejected(wrap(block("T", triangle), repeated), "много рёбер");
        QByteArray labels;
        for (int i = 0; i < 6; ++i)
            labels += "0\nTEXT\n1\nLabel\n";
        many.clear();
        for (int i = 0; i < 3000; ++i)
            many += insert("Labels");
        rejected(wrap(block("Labels", labels), triangle + many), "после раскрытия");
        // Exhausting the budget inside a root POLYLINE must abort the import, not
        // merely exclude its layer and accept an earlier valid contour elsewhere.
        many.clear();
        for (int i = 0; i < 2856; ++i)
            many += insert("Labels");
        rejected(wrap(block("Labels", labels),
                      explicitLayer + many + "0\nTEXT\n0\nTEXT\n0\nTEXT\n0\nTEXT\n" + old),
                 "после раскрытия");
        std::stop_source stop;
        stop.request_stop();
        require(std::holds_alternative<application::ToolProfileError>(
                    importer.run(native, stop.get_token())),
                "Cancellation ignored");
        std::cout
            << "DXF blocks: transforms, geometry, layers, provenance and rejection limits passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
