#include "bending/ui/DrawingSheet.h"
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

static void drawingResources() {
    Q_INIT_RESOURCE(drawing_assets);
}
namespace bending::ui {
namespace {
using namespace application;
QString family() {
    static const QString name = [] {
        drawingResources();
        const int id =
            QFontDatabase::addApplicationFont(QStringLiteral(":/drawing/fonts/GOST-Type-A.ttf"));
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (id < 0 || names.isEmpty())
            throw std::runtime_error("Cannot load the drawing font");
        return names.front();
    }();
    return name;
}
void text(QPainter &p, QRectF area, const QString &value, double height = 3.5,
          int flags = Qt::AlignCenter) {
    QFont font(family());
    font.setPixelSize(1000);
    const QFontMetricsF metrics(font);
    const double factor = height / metrics.capHeight();
    p.save();
    p.setPen(Qt::black);
    p.setFont(font);
    p.translate(area.topLeft());
    p.scale(factor, factor);
    p.drawText(QRectF(0, 0, area.width() / factor, area.height() / factor), flags, value);
    p.restore();
}
QString number(double value) {
    if (std::abs(value) < .0005)
        value = 0;
    QString result = QString::number(value, 'f', 2);
    while (result.endsWith(QLatin1Char('0')))
        result.chop(1);
    if (result.endsWith(QLatin1Char('.')))
        result.chop(1);
    return result.replace(QLatin1Char('.'), QLatin1Char(','));
}
QString scaleName(double scale) {
    return scale >= 1 ? number(scale) + QStringLiteral(":1")
                      : QStringLiteral("1:") + number(1 / scale);
}
void pen(QPainter &p, double width = .25, Qt::PenStyle style = Qt::SolidLine) {
    QPen line(Qt::black, width, style);
    if (style == Qt::DashLine)
        line.setDashPattern({3 / width, 1.5 / width});
    else if (style == Qt::DashDotLine)
        line.setDashPattern({5 / width, 3 / width, .25 / width, 3 / width});
    p.setPen(line);
    p.setBrush(Qt::NoBrush);
}
void arrow(QPainter &p, QPointF tip, QPointF from) {
    const auto delta = from - tip;
    const double length = std::hypot(delta.x(), delta.y());
    if (length < 1e-9)
        return;
    const auto d = delta / length;
    const QPointF n(-d.y(), d.x());
    p.save();
    p.setBrush(Qt::black);
    p.setPen(Qt::NoPen);
    p.drawPolygon(QPolygonF{tip, tip + d * 3 + n * .45, tip + d * 3 - n * .45});
    p.restore();
}
bool overlaps(const QRectF &box, const std::vector<QRectF> &occupied) {
    return std::any_of(occupied.begin(), occupied.end(), [&](const auto &old) {
        return old.adjusted(-.8, -.8, .8, .8).intersects(box);
    });
}
void dimension(QPainter &p, QPointF a, QPointF b, QRectF bounds, const DrawingDimension &d,
               std::vector<QRectF> &occupied) {
    const auto vector = b - a;
    const double length = std::hypot(vector.x(), vector.y());
    if (length < 1e-7)
        return;
    const auto direction = vector / length;
    QPointF normal(-direction.y(), direction.x());
    const auto middle = (a + b) / 2;
    if (QPointF::dotProduct(middle - bounds.center(), normal) < 0)
        normal = -normal;
    double maximum = -1e30;
    for (auto corner :
         {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()})
        maximum = std::max(maximum, QPointF::dotProduct(corner - middle, normal));
    const QString value =
        (d.label.empty() ? QString{} : QString::fromStdString(d.label) + QLatin1Char('=')) +
        number(d.valueMm);
    const double textWidth = std::max(9., value.size() * 2.2);
    const bool outside = length < textWidth + 7;
    double angle = std::atan2(direction.y(), direction.x()) * 180 / std::numbers::pi;
    if (angle >= 90 - 1e-8 || angle < -90)
        angle += 180;
    QPointF da, db, label;
    bool found = false;
    for (int lane = 0; lane < 8 && !found; ++lane) {
        const double offset = std::max(10.0, maximum + 10) + lane * 7;
        da = a + normal * offset;
        db = b + normal * offset;
        for (int position = 0; position < 2; ++position) {
            label = outside ? (position == 0 ? db + direction * (textWidth / 2 + 7)
                                             : da - direction * (textWidth / 2 + 7))
                            : (da + db) / 2;
            QTransform transform;
            transform.translate(label.x(), label.y());
            transform.rotate(angle);
            const auto rect = transform.mapRect(QRectF(-textWidth / 2, -5, textWidth, 4.5));
            if (!overlaps(rect, occupied) && QRectF(25, 20, 385, 193).contains(rect)) {
                occupied.push_back(rect);
                found = true;
                break;
            }
            if (!outside)
                break;
        }
    }
    if (!found)
        throw std::runtime_error("Dimension labels do not fit; choose another orientation");
    pen(p);
    p.drawLine(a + normal * .8, da + normal * 2);
    p.drawLine(b + normal * .8, db + normal * 2);
    p.drawLine(da - direction * (outside ? 6 : 0), db + direction * (outside ? 6 : 0));
    arrow(p, da, outside ? da - direction * 4 : db);
    arrow(p, db, outside ? db + direction * 4 : da);
    p.save();
    p.translate(label);
    p.rotate(angle);
    text(p, QRectF(-textWidth / 2, -5, textWidth, 4.5), value);
    p.restore();
}
void view(QPainter &p, const DrawingView &v, QPointF center, double scale, const QString &caption,
          bool isometric = false) {
    const QPointF midpoint((v.minimum.x + v.maximum.x) / 2, (v.minimum.y + v.maximum.y) / 2);
    const double removed = v.omittedX ? (*v.omittedX)[1] - (*v.omittedX)[0] : 0;
    const double displayedWidth =
        (v.maximum.x - v.minimum.x - removed) * scale + (v.omittedX ? 8 : 0);
    const auto map = [&](DrawingPoint a) {
        const bool right = v.omittedX && a.x >= (*v.omittedX)[1] - 1e-8;
        const double x = a.x - (right ? removed : 0);
        return center + QPointF((x - v.minimum.x) * scale + (right ? 8 : 0) - displayedWidth / 2,
                                -(a.y - midpoint.y()) * scale);
    };
    const QRectF bounds(map({v.minimum.x, v.maximum.y}), map({v.maximum.x, v.minimum.y}));
    // Hidden lines first, visible geometry stays legible on top.
    for (bool hidden : {true, false}) {
        pen(p, hidden ? .25 : .5, hidden ? Qt::DashLine : Qt::SolidLine);
        for (const auto &path : v.paths) {
            if (path.hidden != hidden || path.points.size() < 2)
                continue;
            QPainterPath q;
            for (std::size_t i = 1; i < path.points.size(); ++i) {
                const auto a = path.points[i - 1], b = path.points[i];
                const auto piece = [&](double low, double high) {
                    double from = 0, to = 1;
                    const double dx = b.x - a.x;
                    if (std::abs(dx) < 1e-12) {
                        if (a.x < low - 1e-8 || a.x > high + 1e-8)
                            return;
                    } else {
                        const double t0 = (low - a.x) / dx, t1 = (high - a.x) / dx;
                        from = std::max(0., std::min(t0, t1));
                        to = std::min(1., std::max(t0, t1));
                        if (to <= from + 1e-12)
                            return;
                    }
                    const auto start = map({a.x + dx * from, a.y + (b.y - a.y) * from});
                    if (q.elementCount() == 0 || QLineF(q.currentPosition(), start).length() > 1e-7)
                        q.moveTo(start);
                    q.lineTo(map({a.x + dx * to, a.y + (b.y - a.y) * to}));
                };
                if (v.omittedX) {
                    piece(v.minimum.x - 1e-5, (*v.omittedX)[0]);
                    piece((*v.omittedX)[1], v.maximum.x + 1e-5);
                } else
                    piece(-1e100, 1e100);
            }
            p.drawPath(q);
        }
    }
    pen(p, .25, Qt::DashDotLine);
    for (const auto &[c, radius] : v.circleCenters) {
        if (v.omittedX && c.x > (*v.omittedX)[0] && c.x < (*v.omittedX)[1])
            continue;
        const auto at = map(c);
        const double r = radius * scale + 2;
        p.drawLine(at - QPointF(r, 0), at + QPointF(r, 0));
        p.drawLine(at - QPointF(0, r), at + QPointF(0, r));
    }
    if (v.omittedX) {
        pen(p);
        for (const double x : *v.omittedX) {
            const double px = map({x, v.minimum.y}).x();
            QPainterPath wave;
            wave.moveTo(px, bounds.top() - 1);
            const int count = std::max(2, static_cast<int>(bounds.height() / 3));
            for (int i = 0; i < count; ++i) {
                const double y = bounds.top() - 1 + (bounds.height() + 2) * i / count;
                const double step = (bounds.height() + 2) / count;
                wave.cubicTo(px + .7, y + step / 3, px - .7, y + step * 2 / 3, px, y + step);
            }
            p.drawPath(wave);
        }
    }
    std::vector<QRectF> occupied{QRectF(center.x() - 70, center.y() - 48, 140, 7)};
    for (const auto &d : v.dimensions)
        dimension(p, map(d.start), map(d.end), bounds, d, occupied);
    int labelLane = 0;
    for (const auto &[anchor, value] : v.labels) {
        const auto a = map(anchor);
        auto delta = a - bounds.center();
        const double distance = std::hypot(delta.x(), delta.y());
        delta = distance > 1e-6 ? delta / distance : QPointF(1, -1) / std::sqrt(2.);
        auto end = a + delta * (12 + labelLane++ * 7);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const QRectF box(end.x() - 6, end.y() - 5, 12, 5);
            if (!overlaps(box, occupied) && !box.intersects(bounds) &&
                QRectF(25, 20, 385, 193).contains(box)) {
                occupied.push_back(box);
                break;
            }
            if (attempt == 7)
                throw std::runtime_error("Bend labels do not fit; choose another orientation");
            end += QPointF(7, 7);
        }
        pen(p);
        p.drawLine(a, end);
        arrow(p, a, end);
        text(p, QRectF(end.x() - 6, end.y() - 5, 12, 5), QString::fromStdString(value), 3.5);
    }
    if (isometric) {
        const auto a = map(v.sideAnchor);
        auto b = map(v.sideTip);
        const auto delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        if (length > 1e-8 && length < 12)
            b = a + delta * (12 / length);
        pen(p);
        p.drawLine(a, b);
        arrow(p, a, b);
        text(p, QRectF(b.x() - 4, b.y() - 5, 8, 5), QStringLiteral("A"), 5);
    }
    text(p, QRectF(center.x() - 70, center.y() - 48, 140, 7), caption, 3.5);
}
void titleBlock(QPainter &p, const DrawingTitle &t, double scale, int page, int pages) {
    p.save();
    p.translate(230, 237);
    pen(p, .5);
    p.drawRect(QRectF(0, 0, 185, 55));
    p.drawLine(QPointF(65, 0), QPointF(65, 55));
    p.drawLine(QPointF(65, 15), QPointF(185, 15));
    p.drawLine(QPointF(135, 15), QPointF(135, 55));
    p.drawLine(QPointF(65, 40), QPointF(185, 40));
    p.drawLine(QPointF(135, 20), QPointF(185, 20));
    p.drawLine(QPointF(135, 35), QPointF(185, 35));
    p.drawLine(QPointF(150, 15), QPointF(150, 35));
    p.drawLine(QPointF(167, 15), QPointF(167, 35));
    p.drawLine(QPointF(155, 35), QPointF(155, 40));
    p.drawLine(QPointF(175, 0), QPointF(175, 15));
    p.drawLine(QPointF(175, 5), QPointF(185, 5));
    pen(p);
    for (double y : {5., 10., 15., 20., 25., 30., 35., 40., 45., 50.})
        p.drawLine(QPointF(0, y), QPointF(65, y));
    for (double x : {7., 17., 40., 55.})
        p.drawLine(QPointF(x, 0), QPointF(x, 20));
    for (double x : {17., 40., 55.})
        p.drawLine(QPointF(x, 20), QPointF(x, 55));
    for (int i = 1; i < 3; ++i)
        p.drawLine(QPointF(135 + i * 5, 20), QPointF(135 + i * 5, 35));
    const std::array<QString, 5> headings{QStringLiteral("Изм."), QStringLiteral("Лист"),
                                          QStringLiteral("№ докум."), QStringLiteral("Подп."),
                                          QStringLiteral("Дата")};
    const std::array<double, 6> x{0, 7, 17, 40, 55, 65};
    for (int i = 0; i < 5; ++i)
        text(p, QRectF(x[i], 15, x[i + 1] - x[i], 5), headings[i], 2.5);
    const std::array<QString, 7> roles{QStringLiteral("Разраб."),   QStringLiteral("Пров."),
                                       QStringLiteral("Т. контр."), QString{},
                                       QStringLiteral("Н. контр."), QString{},
                                       QStringLiteral("Утв.")};
    for (int i = 0; i < 7; ++i)
        text(p, QRectF(.8, 20 + 5 * i, 16, 5), roles[i], 2.5, Qt::AlignLeft | Qt::AlignVCenter);
    text(p, QRectF(66, 1, 108, 13), t.designation, 5);
    text(p, QRectF(175, 0, 10, 5), QStringLiteral("Вер."), 2.5);
    text(p, QRectF(67, 16, 66, 23), t.name, 5, Qt::AlignCenter | Qt::TextWrapAnywhere);
    text(p, QRectF(67, 41, 66, 13), t.material, 3.5, Qt::AlignCenter | Qt::TextWrapAnywhere);
    text(p, QRectF(135, 15, 15, 5), QStringLiteral("Лит."), 2.5);
    text(p, QRectF(150, 15, 17, 5), QStringLiteral("Масса"), 2.5);
    text(p, QRectF(167, 15, 18, 5), QStringLiteral("Масштаб"), 2.5);
    text(p, QRectF(168, 21, 16, 13), scaleName(scale), 3.5);
    text(p, QRectF(135, 35, 20, 5), QStringLiteral("Лист %1").arg(page + 1), 2.5);
    text(p, QRectF(155, 35, 30, 5), QStringLiteral("Листов %1").arg(pages), 2.5);
    p.restore();
    text(p, QRectF(365, 292.3, 48, 4.5), QStringLiteral("Формат A3"), 2.5,
         Qt::AlignRight | Qt::AlignVCenter);
    // Additional designation field 26, rotated as in form 1. Other archival fields stay blank.
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 70, 14));
    p.save();
    p.translate(90, 19);
    p.rotate(180);
    text(p, QRectF(1, 1, 68, 12), t.designation, 3.5);
    p.restore();
}
QString bendId(const BendingDrawing &d, std::size_t index) {
    int n = 0;
    for (std::size_t i = 0; i <= index; ++i)
        if (d.bends[i].diagnostic == d.bends[index].diagnostic)
            ++n;
    return QStringLiteral("%1%2")
        .arg(d.bends[index].diagnostic ? QLatin1Char('D') : QLatin1Char('B'))
        .arg(n, 2, 10, QLatin1Char('0'));
}
void bendTable(QPainter &p, const BendingDrawing &d, std::size_t first, std::size_t count, double x,
               double y, double width, double rowHeight) {
    const std::array<double, 7> columns{0, .13, .30, .45, .60, .80, 1};
    const std::array<QString, 6> captions{QStringLiteral("Зона"),      QStringLiteral("Поворот, °"),
                                          QStringLiteral("R внутр."),  QStringLiteral("L оси"),
                                          QStringLiteral("H1 наруж."), QStringLiteral("H2 наруж.")};
    pen(p);
    p.drawRect(QRectF(x, y, width, (count + 1) * rowHeight));
    for (double c : columns)
        p.drawLine(QPointF(x + width * c, y), QPointF(x + width * c, y + (count + 1) * rowHeight));
    for (std::size_t row = 0; row <= count; ++row) {
        if (row)
            p.drawLine(QPointF(x, y + row * rowHeight), QPointF(x + width, y + row * rowHeight));
        for (std::size_t col = 0; col < captions.size(); ++col) {
            QString value = captions[col];
            if (row) {
                const auto index = first + row - 1;
                const auto &b = d.bends[index];
                const std::array<QString, 6> cells{
                    bendId(d, index),
                    number(b.turnRadians * 180 / std::numbers::pi),
                    number(b.innerRadiusMm),
                    number(b.axialLengthMm) + (b.trimmedEnds ? QStringLiteral("!") : QString{}),
                    b.outerFlangeMm[0] ? number(*b.outerFlangeMm[0]) : QStringLiteral("-"),
                    b.outerFlangeMm[1] ? number(*b.outerFlangeMm[1]) : QStringLiteral("-")};
                value = cells[col];
            }
            text(p,
                 QRectF(x + width * columns[col] + .5, y + row * rowHeight,
                        width * (columns[col + 1] - columns[col]) - 1, rowHeight),
                 value, rowHeight < 8 ? 2.5 : 3.5);
        }
    }
}
} // namespace
int DrawingSheet::pageCount(const BendingDrawing &d) {
    return 1 + static_cast<int>((d.bends.size() > 4 ? d.bends.size() - 4 + 23 : 0) / 24);
}
void DrawingSheet::paint(QPainter &p, const BendingDrawing &d, const DrawingTitle &t, int page) {
    const auto scale = drawingScale(d);
    if (!scale || page < 0 || page >= pageCount(d))
        throw std::runtime_error("Invalid drawing layout");
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(0, 0, widthMm, heightMm), Qt::white);
    pen(p, .5);
    p.drawRect(QRectF(20, 5, 395, 287));
    titleBlock(p, t, *scale, page, pageCount(d));
    text(p, QRectF(96, 5, 312, 8), QStringLiteral("ЛИСТ ГИБКИ"), 5);
    text(p, QRectF(96, 13, 312, 6), QStringLiteral("Размеры для справок, мм. Не утверждён."), 3.5);
    if (page == 0) {
        view(p, d.views[0], {128, 72}, *scale,
             d.reverseView ? QStringLiteral("Обратная сторона A")
                           : QStringLiteral("Вид со стороны A"));
        view(p, d.views[1], {128, 174}, *scale, QStringLiteral("Вид сверху"));
        view(p, d.views[2], {328, 72}, *scale, QStringLiteral("Вид слева"));
        const auto &iso = d.views[3];
        const double isoFit =
            std::min(150 / (iso.maximum.x - iso.minimum.x), 65 / (iso.maximum.y - iso.minimum.y));
        const double isoScale = standardDrawingScale(std::min(*scale, isoFit)).value_or(*scale);
        view(p, iso, {328, 172}, isoScale,
             QStringLiteral("Общий вид · %1").arg(scaleName(isoScale)), true);
        QString thickness =
            d.thicknessMm
                ? QStringLiteral("t = %1 мм%2")
                      .arg(number(*d.thicknessMm),
                           d.manualThickness ? QStringLiteral(" (задано вручную)") : QString{})
                : QStringLiteral("Толщина не определена");
        if (d.thicknessMm && d.declaredThicknessMm && !d.manualThickness &&
            std::abs(*d.thicknessMm - *d.declaredThicknessMm) > 1e-5)
            thickness +=
                QStringLiteral(" · вручную %1 (расхождение)").arg(number(*d.declaredThicknessMm));
        if (!t.execution.trimmed().isEmpty())
            thickness += QStringLiteral(" · ") + t.execution.trimmed();
        text(p, QRectF(25, 216, 380, 6), thickness, 3.5, Qt::AlignLeft | Qt::AlignVCenter);
        text(p, QRectF(25, 222, 380, 6),
             d.bends.empty()
                 ? QString{}
                 : QStringLiteral(
                       "H1, H2 — наружные размеры полок. Поворот — от плоского состояния."),
             2.5, Qt::AlignLeft | Qt::AlignVCenter);
        QStringList notes;
        if (std::any_of(d.bends.begin(), d.bends.end(), [](const auto &b) { return b.diagnostic; }))
            notes << QStringLiteral("D — кандидат гиба");
        if (std::any_of(d.bends.begin(), d.bends.end(),
                        [](const auto &b) { return b.trimmedEnds; }))
            notes << QStringLiteral("! — подрезанные торцы");
        if (d.unrecognizedCylindricalFaces)
            notes << QStringLiteral("Не распознано цилиндрических граней: %1")
                         .arg(d.unrecognizedCylindricalFaces);
        text(p, QRectF(25, 228, 380, 6), notes.join(QStringLiteral(" · ")), 2.5,
             Qt::AlignLeft | Qt::AlignVCenter);
        pen(p, .5);
        p.drawRect(QRectF(25, 237, 195, 13));
        p.drawLine(QPointF(160, 237), QPointF(160, 250));
        text(p, QRectF(27, 237, 130, 13), QStringLiteral("Количество, шт."), 3.5,
             Qt::AlignLeft | Qt::AlignVCenter);
        text(p, QRectF(161, 237, 58, 13),
             t.quantity ? QString::number(*t.quantity) : QStringLiteral("-"), 5);
        bendTable(p, d, 0, std::min<std::size_t>(4, d.bends.size()), 25, 253, 195, 6);
    } else {
        const std::size_t first = 4 + static_cast<std::size_t>(page - 1) * 24;
        bendTable(p, d, first, std::min<std::size_t>(24, d.bends.size() - first), 30, 30, 375, 8);
        text(p, QRectF(30, 239, 190, 7),
             QStringLiteral("Количество, шт.: %1")
                 .arg(t.quantity ? QString::number(*t.quantity) : QStringLiteral("-")),
             3.5, Qt::AlignLeft | Qt::AlignVCenter);
    }
}
QImage DrawingSheet::preview(const BendingDrawing &d, const DrawingTitle &t, int page, int width) {
    QImage image(width, static_cast<int>(width * heightMm / widthMm), QImage::Format_RGB32);
    QPainter p(&image);
    p.scale(width / widthMm, width / widthMm);
    paint(p, d, t, page);
    return image;
}
QString DrawingSheet::writePdf(const QString &path, const BendingDrawing &d,
                               const DrawingTitle &t) {
    if ((t.quantity && (*t.quantity < 1 || *t.quantity > 1000000)) || !drawingScale(d))
        return QStringLiteral("Некорректное количество или размеры листа");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    try {
        {
            QPdfWriter writer(&file);
            writer.setPageSize(QPageSize(QSizeF(widthMm, heightMm), QPageSize::Millimeter));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
            writer.setResolution(254); // Exactly 10 device units / mm.
            writer.setTitle(t.name);
            writer.setCreator(QStringLiteral("3D Bending Simulator"));
            QPainter painter(&writer);
            if (!painter.isActive())
                return QStringLiteral("Не удалось начать запись PDF");
            painter.scale(writer.resolution() / 25.4, writer.resolution() / 25.4);
            for (int page = 0; page < pageCount(d); ++page) {
                if (page && !writer.newPage())
                    return QStringLiteral("Не удалось создать страницу PDF");
                paint(painter, d, t, page);
            }
            if (!painter.end())
                return QStringLiteral("Не удалось завершить PDF");
        }
        if (!file.commit())
            return file.errorString();
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    }
    return {};
}
} // namespace bending::ui
