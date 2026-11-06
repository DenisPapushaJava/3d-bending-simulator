#include "Dimensions.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_DimensionAspect.hxx>
#include <PrsDim_LengthDimension.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <QFont>
#include <QFontMetricsF>
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
             const QString &caption, double height, const gp_Vec &arrowSide = {}) {
    const auto end = anchor.Translated(offset);
    Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(anchor, end).Edge());
    line->SetColor(bendColor);
    line->SetWidth(1.5);
    append(result, line);
    if (arrowSide.SquareMagnitude() > 1.e-14) {
        // A small screen-sized arrow points to the actual prepared bend line.
        const auto back = offset * .08;
        Handle(AIS_Shape) arrow = new AIS_Shape(
            BRepBuilderAPI_MakePolygon(anchor.Translated(back + arrowSide * .08), anchor,
                                       anchor.Translated(back - arrowSide * .08))
                .Shape());
        arrow->SetColor(bendColor);
        arrow->SetWidth(1.5);
        append(result, arrow);
    }
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
    return QStringLiteral("Угол %1° · %2 · t %3 мм")
        .arg(number(180.0 - angle * 180.0 / std::numbers::pi),
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
    if (!bend) {
        length(result, point(lo), {hi.x, lo.y, lo.z}, {0, -gap, 0},
               QStringLiteral("X %1 мм").arg(number(hi.x - lo.x)), height);
        length(result, point(lo), {lo.x, hi.y, lo.z}, {-gap, 0, 0},
               QStringLiteral("Y %1 мм").arg(number(hi.y - lo.y)), height);
        length(result, {hi.x, hi.y, lo.z}, point(hi), {gap, 0, 0},
               QStringLiteral("Z %1 мм").arg(number(hi.z - lo.z)), height);
    }
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

std::optional<std::size_t> flatBendAt(const application::FlatPatternInfo &info,
                                      const DimensionLayout &layout, QPointF position,
                                      double tolerancePixels) {
    if (!layout.project)
        return std::nullopt;
    std::optional<std::size_t> result;
    double best = tolerancePixels * tolerancePixels;
    for (const auto &bend : info.bends) {
        const auto line = bend.centreLine();
        const auto a = layout.project({line[0].x, line[0].y, info.bounds.maximum.z});
        const auto b = layout.project({line[1].x, line[1].y, info.bounds.maximum.z});
        const auto ab = b - a;
        const auto length2 = QPointF::dotProduct(ab, ab);
        if (length2 < 1.)
            continue;
        const auto t = std::clamp(QPointF::dotProduct(position - a, ab) / length2, 0., 1.);
        const auto delta = position - (a + ab * t);
        const auto distance2 = QPointF::dotProduct(delta, delta);
        if (distance2 < best) {
            best = distance2;
            result = bend.bendIndex;
        }
    }
    return result;
}
DimensionAnnotations flatPatternDimensions(const application::FlatPatternInfo &info, double height,
                                           const DimensionLayout &layout,
                                           std::optional<std::size_t> selected, double facing) {
    DimensionAnnotations result;
    const auto &hi = info.bounds.maximum;
    const auto gap = std::max(hi.x, hi.y) * .12;
    length(result, {0, 0, hi.z}, {hi.x, 0, hi.z}, {0, -gap, 0},
           QStringLiteral("Ширина %1 мм").arg(number(hi.x)), height);
    length(result, {0, 0, hi.z}, {0, hi.y, hi.z}, {-gap, 0, 0},
           QStringLiteral("Развёртка %1 мм").arg(number(hi.y)), height);
    // Directions are relative to the physical flat side facing the viewer. Do not
    // pretend that a nearly edge-on sheet has readable bend annotations.
    if (std::abs(facing) < .15)
        return result;
    const auto bendId = [](std::size_t index) {
        return QStringLiteral("B%1").arg(index + 1, 2, 10, QLatin1Char('0'));
    };
    const auto active = std::find_if(info.bends.begin(), info.bends.end(), [&](const auto &bend) {
        return selected && bend.bendIndex == *selected;
    });
    if (active != info.bends.end()) {
        const auto line = active->centreLine();
        const gp_Pnt anchor((line[0].x + line[1].x) / 2, (line[0].y + line[1].y) / 2, hi.z);
        const auto direction = active->direction * (facing > 0 ? 1 : -1);
        callout(result, anchor, layout.up * 1.8,
                QStringLiteral("%1 · %2 · %3° · R %4 · L %5 мм")
                    .arg(bendId(active->bendIndex),
                         direction > 0 ? QStringLiteral("ВВЕРХ") : QStringLiteral("ВНИЗ"),
                         number(180 - active->turnRadians * 180 / std::numbers::pi),
                         number(active->radiusMm), number(active->lengthMm())),
                height, layout.right);
        // Only a confirmed axis-aligned coordinate is presented. A midpoint of
        // an oblique bend is not a distance from the stock edge or a gauge X.
        if (std::abs(line[0].y - line[1].y) < 1.e-5)
            length(result, {hi.x, 0, hi.z}, {hi.x, line[0].y, hi.z}, {gap, 0, 0},
                   QStringLiteral("Y %1 мм").arg(number(line[0].y)), height);
        else if (std::abs(line[0].x - line[1].x) < 1.e-5)
            length(result, {0, hi.y, hi.z}, {line[0].x, hi.y, hi.z}, {0, gap, 0},
                   QStringLiteral("X %1 мм").arg(number(line[0].x)), height);
        return result;
    }
    // Overview: compact IDs only. Dense/occluded markers remain accessible via
    // line selection and the existing source-bend list, without shrinking text.
    QFont font(QStringLiteral("sans-serif"));
    font.setPixelSize(qRound(height));
    const QFontMetricsF metrics(font);
    std::vector<QRectF> occupied = layout.excluded;
    for (const auto &bend : info.bends) {
        const auto line = bend.centreLine();
        const gp_Pnt anchor((line[0].x + line[1].x) / 2, (line[0].y + line[1].y) / 2, hi.z);
        const auto position = anchor.Translated(layout.up * .3);
        const auto caption = bendId(bend.bendIndex);
        if (layout.project) {
            const auto screen = layout.project(position);
            const QRectF box(screen.x() - metrics.horizontalAdvance(caption) / 2 - 4,
                             screen.y() - metrics.height() - 4,
                             metrics.horizontalAdvance(caption) + 8, metrics.height() + 8);
            if (!layout.viewport.contains(box) ||
                std::any_of(occupied.begin(), occupied.end(),
                            [&](const auto &other) { return box.intersects(other); }))
                continue;
            occupied.push_back(box);
        }
        Handle(AIS_TextLabel) label = new AIS_TextLabel;
        label->SetPosition(position);
        label->SetText(extended(caption));
        label->SetColor(dimensionColor);
        label->SetHeight(height);
        label->SetFont(Font_NOF_SANS_SERIF);
        label->SetZoomable(false);
        label->SetHJustification(Graphic3d_HTA_CENTER);
        label->SetVJustification(Graphic3d_VTA_BOTTOM);
        append(result, label);
        result.labels.append(caption);
    }
    return result;
}
DimensionAnnotations previewDimensions(const cad::PreviewFrame &frame,
                                       const simulation::SingleBend &model, double height,
                                       const DimensionLayout &layout, bool compact) {
    DimensionAnnotations result;
    if (!frame.dimensions) {
        return result;
    }
    const auto &anchors = *frame.dimensions;
    if (!compact) {
        length(result, anchors.fixed[0], anchors.fixed[1], -layout.right * 2.2 - layout.up * 0.6,
               QStringLiteral("П1 прям. %1 мм").arg(number(model.fixedFlangeMm)), height);
        length(result, anchors.moving[0], anchors.moving[1], layout.right * 2.2 - layout.up * 0.6,
               QStringLiteral("П2 прям. %1 мм").arg(number(model.movingFlangeMm)), height);
        length(result, anchors.fixed[1], anchors.fixed[1].Translated(anchors.widthVector),
               -layout.up * 1.8, QStringLiteral("Охват по оси %1 мм").arg(number(model.widthMm)),
               height);
    }
    callout(result, anchors.bendMiddle, layout.up * 1.8,
            compact ? QStringLiteral("Угол %1°")
                          .arg(number(180 - anchors.angleRadians * 180 / std::numbers::pi))
                    : bendCaption(anchors.angleRadians, anchors.innerRadiusMm, model.thicknessMm),
            height);
    return result;
}
} // namespace bending::viewer
