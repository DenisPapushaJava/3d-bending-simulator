#include "Dimensions.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepTools.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_DimensionAspect.hxx>
#include <PrsDim_LengthDimension.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <QLocale>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bending::viewer {
namespace {
const Quantity_Color dimensionColor(0.76, 0.87, 0.94, Quantity_TOC_sRGB);
const Quantity_Color bendColor(1.0, 0.79, 0.32, Quantity_TOC_sRGB);

QString number(double value) {
    const auto locale = QLocale::system();
    if (std::abs(value) > 0.0 && std::abs(value) < 0.01) {
        return locale.toString(value, 'g', 3);
    }
    auto result = locale.toString(value, 'f', 2);
    while (result.endsWith(QLatin1Char('0'))) {
        result.chop(1);
    }
    if (result.endsWith(locale.decimalPoint())) {
        result.chop(locale.decimalPoint().size());
    }
    return result;
}
TCollection_ExtendedString extended(const QString &value) {
    return {value.toUtf8().constData(), true};
}
gp_Pnt point(application::PointMm p) {
    return {p.x, p.y, p.z};
}
void append(DimensionAnnotations &result, const Handle(AIS_InteractiveObject) & object) {
    // Decorations neither affect FitAll nor intercept CAD face selection.
    object->SetInfiniteState(true);
    object->SetZLayer(Graphic3d_ZLayerId_Topmost);
    result.objects.push_back(object);
}
void length(DimensionAnnotations &result, const gp_Pnt &a, const gp_Pnt &b, const gp_Vec &offset,
            const QString &caption, double height, const Quantity_Color &color = dimensionColor) {
    if (a.Distance(b) <= 1.0e-7 || gp_Vec(a, b).Crossed(offset).SquareMagnitude() <= 1.0e-14) {
        return;
    }
    Handle(PrsDim_LengthDimension) dimension =
        new PrsDim_LengthDimension(a, b, gp_Pln(a, gp_Dir(gp_Vec(a, b).Crossed(offset))));
    Handle(Prs3d_DimensionAspect) aspect = new Prs3d_DimensionAspect;
    aspect->MakeText3d(false);
    aspect->MakeArrows3d(false);
    aspect->SetCommonColor(color);
    aspect->TextAspect()->SetHeight(height);
    aspect->TextAspect()->SetFont(Font_NOF_SANS_SERIF);
    aspect->ArrowAspect()->SetLength(std::min(a.Distance(b) * 0.1, offset.Magnitude() * 0.06));
    aspect->SetExtensionSize(offset.Magnitude() * 0.2);
    dimension->SetDimensionAspect(aspect);
    dimension->SetCustomValue(extended(caption));
    dimension->SetTextPosition(gp_Pnt((a.XYZ() + b.XYZ()) * 0.5).Translated(offset));
    append(result, dimension);
    result.labels.append(caption);
}
void callout(DimensionAnnotations &result, const gp_Pnt &anchor, const gp_Vec &offset,
             const QString &caption, double height) {
    const auto end = anchor.Translated(offset);
    Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(anchor, end).Edge());
    line->SetColor(bendColor);
    line->SetWidth(1.5);
    append(result, line);
    Handle(AIS_TextLabel) label = new AIS_TextLabel;
    label->SetPosition(end);
    label->SetText(extended(caption));
    label->SetColor(bendColor);
    label->SetHeight(height);
    label->SetFont(Font_NOF_SANS_SERIF);
    label->SetZoomable(false);
    label->SetHJustification(Graphic3d_HTA_CENTER);
    label->SetVJustification(Graphic3d_VTA_BOTTOM);
    label->SetDisplayType(Aspect_TODT_SUBTITLE);
    label->SetColorSubTitle(Quantity_Color(0.125, 0.165, 0.21, Quantity_TOC_sRGB));
    append(result, label);
    result.labels.append(caption);
}
QString bendCaption(double angle, std::optional<double> radius, double thickness) {
    return QStringLiteral("Поворот %1° · %2 · t %3 мм")
        .arg(number(angle * 180.0 / std::numbers::pi),
             radius ? QStringLiteral("R %1 мм").arg(number(*radius)) : QStringLiteral("плоско"),
             number(thickness));
}
} // namespace

DimensionAnnotations documentDimensions(const application::DocumentMetadata &metadata,
                                        const TopTools_IndexedMapOfShape &faces,
                                        const std::optional<application::RecognizedBend> &bend,
                                        double height, const DimensionLayout &layout) {
    DimensionAnnotations result;
    const auto &lo = metadata.bounds.minimum;
    const auto &hi = metadata.bounds.maximum;
    const auto gap = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 0.14;
    if (!(gap > 0.0) || !std::isfinite(gap)) {
        return result;
    }
    length(result, point(lo), {hi.x, lo.y, lo.z}, {0, -gap, 0},
           QStringLiteral("X %1 мм").arg(number(hi.x - lo.x)), height);
    length(result, point(lo), {lo.x, hi.y, lo.z}, {-gap, 0, 0},
           QStringLiteral("Y %1 мм").arg(number(hi.y - lo.y)), height);
    length(result, {hi.x, hi.y, lo.z}, point(hi), {gap, 0, 0},
           QStringLiteral("Z %1 мм").arg(number(hi.z - lo.z)), height);
    if (!bend || bend->innerFace == 0 ||
        bend->innerFace > static_cast<std::size_t>(faces.Extent())) {
        return result;
    }
    const auto face = TopoDS::Face(faces(static_cast<int>(bend->innerFace)));
    double u1, u2, v1, v2;
    BRepTools::UVBounds(face, u1, u2, v1, v2);
    const auto anchor = BRepAdaptor_Surface(face).Value((u1 + u2) * 0.5, (v1 + v2) * 0.5);
    const auto start = point(bend->axisStart);
    const auto end = point(bend->axisEnd);
    const auto middle = gp_Pnt((start.XYZ() + end.XYZ()) * 0.5);
    const auto axis = gp_Vec(start, end);
    if (axis.SquareMagnitude() <= 1.0e-14) {
        return result;
    }
    auto radial = gp_Vec(middle, anchor);
    radial -= axis * (radial.Dot(axis) / axis.SquareMagnitude());
    if (radial.SquareMagnitude() <= 1.0e-14) {
        return result;
    }
    const auto caption = bend->trimmedEnds ? QStringLiteral("Осевой габарит %1 мм")
                                           : QStringLiteral("Длина гиба %1 мм");
    auto side = -layout.right;
    side -= axis * (side.Dot(axis) / axis.SquareMagnitude());
    if (side.Magnitude() < layout.right.Magnitude() * 0.1) {
        side = radial.Normalized() * layout.right.Magnitude();
    }
    length(result, start.Translated(radial), end.Translated(radial), side * 2.0,
           caption.arg(number(bend->lengthMm)), height, bendColor);
    callout(result, anchor, layout.up * 2.3,
            bendCaption(bend->angleRadians, bend->innerRadiusMm, bend->thicknessMm), height);
    return result;
}

DimensionAnnotations flatPatternDimensions(const application::FlatPatternInfo &info, double height,
                                           const DimensionLayout &layout) {
    DimensionAnnotations result;
    const auto &hi = info.bounds.maximum;
    const double gap = std::max(hi.x, hi.y) * .12;
    struct LabelBox {
        double x, y, halfWidth;
    };
    std::vector<LabelBox> labels;
    const auto screen = [&](const gp_Pnt &p) {
        const gp_Vec v(gp_Pnt(0, 0, 0), p);
        return std::pair{v.Dot(layout.right) / layout.right.SquareMagnitude(),
                         v.Dot(layout.up) / layout.up.SquareMagnitude()};
    };
    const auto halfWidth = [](const QString &caption) { return .075 * caption.size() + .1; };
    const auto measure = [&](gp_Pnt a, gp_Pnt b, gp_Vec offset, const QString &caption,
                             const Quantity_Color &color) {
        length(result, a, b, offset, caption, height, color);
        const auto [x, y] = screen(gp_Pnt((a.XYZ() + b.XYZ()) * .5).Translated(offset));
        labels.push_back({x, y, halfWidth(caption)});
    };
    measure({0, 0, hi.z}, {hi.x, 0, hi.z}, {0, -gap, 0},
            QStringLiteral("Ширина %1 мм").arg(number(hi.x)), dimensionColor);
    measure({0, 0, hi.z}, {0, hi.y, hi.z}, {-gap, 0, 0},
            QStringLiteral("Развёртка %1 мм").arg(number(hi.y)), dimensionColor);
    const auto bendId = [](std::size_t index) {
        return QStringLiteral("B%1").arg(index + 1, 2, 10, QLatin1Char('0'));
    };
    struct CoordinateRow {
        double value;
        bool x;
        std::vector<std::size_t> ids;
    };
    std::vector<CoordinateRow> rows;
    const auto coordinate = [&](double value, bool x, std::size_t id) {
        auto row = std::find_if(rows.begin(), rows.end(), [&](const auto &r) {
            return r.x == x && std::abs(r.value - value) <= 1.e-5;
        });
        if (row == rows.end())
            rows.push_back({value, x, {id}});
        else
            row->ids.push_back(id);
    };
    for (const auto &b : info.bends) {
        const auto line = b.centreLine();
        if (std::abs(line[0].y - line[1].y) < 1.e-5)
            coordinate(line[0].y, false, b.bendIndex);
        else if (std::abs(line[0].x - line[1].x) < 1.e-5)
            coordinate(line[0].x, true, b.bendIndex);
        else {
            coordinate((line[0].x + line[1].x) / 2, true, b.bendIndex);
            coordinate((line[0].y + line[1].y) / 2, false, b.bendIndex);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return std::pair{a.x, a.value} < std::pair{b.x, b.value};
    });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto &row = rows[i];
        std::sort(row.ids.begin(), row.ids.end());
        QString ids;
        if (row.ids.size() > 1 && row.ids.back() - row.ids.front() + 1 == row.ids.size())
            ids = bendId(row.ids.front()) + QChar(0x2013) + bendId(row.ids.back());
        else {
            QStringList names;
            for (auto id : row.ids)
                names << bendId(id);
            ids = names.join(QLatin1Char('/'));
        }
        const double offset = gap * (1 + i * .65);
        measure(row.x ? gp_Pnt(0, hi.y, hi.z) : gp_Pnt(hi.x, 0, hi.z),
                row.x ? gp_Pnt(row.value, hi.y, hi.z) : gp_Pnt(hi.x, row.value, hi.z),
                row.x ? gp_Vec(0, offset, 0) : gp_Vec(offset, 0, 0),
                QStringLiteral("%1 · %2 %3 мм")
                    .arg(ids, row.x ? QStringLiteral("X") : QStringLiteral("Y"), number(row.value)),
                bendColor);
    }
    const auto [cx, cy] = screen({hi.x / 2, hi.y / 2, hi.z});
    for (const auto &b : info.bends) {
        const auto line = b.centreLine();
        const gp_Pnt anchor((line[0].x + line[1].x) / 2, (line[0].y + line[1].y) / 2, hi.z);
        const auto [ax, ay] = screen(anchor);
        const gp_Vec axis(point(line[0]), point(line[1]));
        double nx = -axis.Dot(layout.up), ny = axis.Dot(layout.right);
        const double norm = std::hypot(nx, ny);
        if (norm > 1.e-10) {
            nx /= norm;
            ny /= norm;
        } else {
            nx = 0;
            ny = 1;
        }
        // Put the caption towards the blank interior, away from the outside dimensions.
        if (nx * (cx - ax) + ny * (cy - ay) < 0) {
            nx = -nx;
            ny = -ny;
        }
        const auto caption =
            QStringLiteral("%1 %2 · %3° · L %4 мм")
                .arg(bendId(b.bendIndex),
                     b.direction > 0 ? QStringLiteral("↑") : QStringLiteral("↓"),
                     number(180 - b.turnRadians * 180 / std::numbers::pi), number(b.lengthMm()));
        const double distance = 1 + .8 * std::abs(nx);
        const double x = ax + nx * distance, initialY = ay + ny * distance;
        // Screen units are 50 px. Reserve a conservative text box and separate nearby bends.
        const double width = halfWidth(caption);
        double y = initialY;
        for (std::size_t row = 0; row < info.bends.size() * 2 + 1; ++row) {
            const double shift = (row + 1) / 2 * .6 * (row % 2 ? 1 : -1);
            y = initialY + shift;
            if (std::none_of(labels.begin(), labels.end(), [&](const auto &placed) {
                    return std::abs(x - placed.x) < width + placed.halfWidth &&
                           std::abs(y - placed.y) < .5;
                }))
                break;
        }
        labels.push_back({x, y, width});
        callout(result, anchor, layout.right * (x - ax) + layout.up * (y - ay), caption, height);
    }
    return result;
}
DimensionAnnotations previewDimensions(const cad::PreviewFrame &frame,
                                       const simulation::SingleBend &model, double height,
                                       const DimensionLayout &layout) {
    DimensionAnnotations result;
    if (!frame.dimensions) {
        return result;
    }
    const auto &anchors = *frame.dimensions;
    length(result, anchors.fixed[0], anchors.fixed[1], -layout.right * 2.2 - layout.up * 0.6,
           QStringLiteral("П1 прям. %1 мм").arg(number(model.fixedFlangeMm)), height);
    length(result, anchors.moving[0], anchors.moving[1], layout.right * 2.2 - layout.up * 0.6,
           QStringLiteral("П2 прям. %1 мм").arg(number(model.movingFlangeMm)), height);
    length(result, anchors.fixed[1], anchors.fixed[1].Translated(anchors.widthVector),
           -layout.up * 1.8, QStringLiteral("Ширина %1 мм").arg(number(model.widthMm)), height);
    callout(result, anchors.bendMiddle, layout.up * 1.8,
            bendCaption(anchors.angleRadians, anchors.innerRadiusMm, model.thicknessMm), height);
    return result;
}
} // namespace bending::viewer
