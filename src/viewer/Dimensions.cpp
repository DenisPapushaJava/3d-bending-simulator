#include "Dimensions.h"

#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <Font_NameOfFont.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_DimensionAspect.hxx>
#include <PrsDim_LengthDimension.hxx>
#include <gp_Pln.hxx>

#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_set>

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
gp_Vec placedOffset(DimensionAnnotations &result, const DimensionLayout *layout,
                    const gp_Pnt &anchor, gp_Vec offset, const QString &text, double height) {
    if (!layout || !layout->project || !layout->unproject)
        return offset;
    QFont font(QStringLiteral("sans-serif"));
    font.setPixelSize(qRound(height));
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text) + 12;
    const double h = metrics.height() + 8;
    const auto preferred = layout->project(anchor.Translated(offset));
    // Stay local to the measured geometry. A free slot at a distant window
    // border is not a reason to create a metre-long extension line.
    for (int row = 0; row < 9; ++row)
        for (int column = 0; column < 3; ++column) {
            auto position = preferred;
            position.rx() += column == 1 ? -24 : column == 2 ? 24 : 0;
            position.ry() += ((row + 1) / 2) * h * (row % 2 ? 1 : -1);
            const QRectF box(position.x() - width / 2, position.y() - h, width, h + 4);
            const auto intersects = [&](const auto &boxes) {
                return std::any_of(boxes.begin(), boxes.end(),
                                   [&](const auto &b) { return box.intersects(b); });
            };
            if (!layout->viewport.contains(box) || intersects(layout->excluded) ||
                intersects(result.occupied))
                continue;
            result.occupied.push_back(box);
            return gp_Vec(anchor, layout->unproject(position, anchor));
        }
    result.crowded = true;
    return offset; // Preserve the measurement, report insufficient space instead of deleting it.
}
void callout(DimensionAnnotations &, const gp_Pnt &, const gp_Vec &, const QString &, double,
             const gp_Vec &, const DimensionLayout *);
void length(DimensionAnnotations &result, const gp_Pnt &a, const gp_Pnt &b, const gp_Vec &offset,
            const QString &caption, double height, const Quantity_Color &color = dimensionColor,
            const DimensionLayout *layout = nullptr) {
    if (a.Distance(b) <= 1.0e-7)
        return;
    if (layout && layout->project && layout->unproject && !layout->viewport.isEmpty()) {
        const bool firstVisible = layout->viewport.contains(layout->project(a));
        const bool secondVisible = layout->viewport.contains(layout->project(b));
        if (firstVisible != secondVisible) {
            callout(result, firstVisible ? a : b, layout->up * .7, caption, height, layout->right,
                    layout);
            return;
        }
    }
    auto initial = offset;
    if (gp_Vec(a, b).Crossed(initial).SquareMagnitude() <= 1.0e-14) {
        if (!layout)
            return;
        initial = gp_Vec(a, b).Crossed(layout->up).SquareMagnitude() > 1.e-14 ? layout->up
                                                                              : layout->right;
    }
    const auto middle = gp_Pnt((a.XYZ() + b.XYZ()) * 0.5);
    auto placed = placedOffset(result, layout, middle, initial, caption, height);
    if (gp_Vec(a, b).Crossed(placed).SquareMagnitude() <= 1.e-14)
        placed = initial;
    Handle(PrsDim_LengthDimension) dimension =
        new PrsDim_LengthDimension(a, b, gp_Pln(a, gp_Dir(gp_Vec(a, b).Crossed(placed))));
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
    dimension->SetTextPosition(middle.Translated(placed));
    append(result, dimension);
    result.labels.append(caption);
}
void callout(DimensionAnnotations &result, const gp_Pnt &anchor, const gp_Vec &offset,
             const QString &caption, double height, const gp_Vec &arrowSide = {},
             const DimensionLayout *layout = nullptr) {
    auto actualOffset = placedOffset(result, layout, anchor, offset, caption, height);
    if (actualOffset.SquareMagnitude() <= 1.e-14)
        actualOffset = offset.SquareMagnitude() > 1.e-14 ? offset : gp_Vec(0, 1, 0);
    const auto end = anchor.Translated(actualOffset);
    Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(anchor, end).Edge());
    line->SetColor(bendColor);
    line->SetWidth(1.5);
    append(result, line);
    if (arrowSide.SquareMagnitude() > 1.e-14) {
        // A small screen-sized arrow points to the actual prepared bend line.
        const auto back = actualOffset.Normalized() * arrowSide.Magnitude() * .12;
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
void selectedMeasurements(DimensionAnnotations &result, const application::ModelMeasurements &data,
                          MeasurementSelection selection, double height,
                          const DimensionLayout &layout, bool flat = false) {
    using namespace application;
    if (selection.kind == MeasurementKind::None)
        return;
    if (selection.kind == MeasurementKind::Opening) {
        const auto opening = std::find_if(data.openings.begin(), data.openings.end(),
                                          [&](const auto &v) { return v.id == selection.id; });
        if (opening == data.openings.end())
            return;
        const auto center = point(opening->center);
        const auto major = gp_Vec(opening->major.x, opening->major.y, opening->major.z);
        const auto minor = gp_Vec(opening->minor.x, opening->minor.y, opening->minor.z);
        const auto caption =
            opening->circular ? QStringLiteral("Отверстие · ⌀%1 мм").arg(number(opening->lengthMm))
                              : QStringLiteral("Вырез · %1 × %2 мм")
                                    .arg(number(opening->lengthMm), number(opening->widthMm));
        callout(result, center, layout.up * 1.0, caption, height, layout.right, &layout);
        if (!opening->circular) {
            length(result, center.Translated(-major), center.Translated(major), layout.up,
                   QStringLiteral("Длина %1 мм").arg(number(opening->lengthMm)), height, bendColor,
                   &layout);
            length(result, center.Translated(-minor), center.Translated(minor), layout.right,
                   QStringLiteral("Ширина %1 мм").arg(number(opening->widthMm)), height, bendColor,
                   &layout);
        }
        std::vector<MeasuredSpan> locations = opening->locations;
        if (!selection.bases.empty()) {
            locations.clear();
            for (auto id : selection.bases) {
                const auto edge =
                    std::find_if(data.edges.begin(), data.edges.end(),
                                 [&](const auto &e) { return e.id == id && e.straight; });
                if (edge == data.edges.end() || edge->bendBoundary ||
                    (std::find(data.datumEdges.begin(), data.datumEdges.end(), id) ==
                         data.datumEdges.end() &&
                     std::none_of(data.faces.begin(), data.faces.end(), [&](const auto &face) {
                         return std::find(opening->hostFaces.begin(), opening->hostFaces.end(),
                                          face.id) != opening->hostFaces.end() &&
                                std::find(face.outerEdges.begin(), face.outerEdges.end(), id) !=
                                    face.outerEdges.end();
                     })))
                    continue;
                const gp_Vec ab(point(edge->first), point(edge->second));
                const gp_Vec normal(opening->normal.x, opening->normal.y, opening->normal.z);
                if (ab.SquareMagnitude() <= 1.e-12 || normal.SquareMagnitude() <= 1.e-12)
                    continue;
                const auto projectedCenter =
                    center.Translated(normal * (gp_Vec(center, point(edge->first)).Dot(normal) /
                                                normal.SquareMagnitude()));
                const double t =
                    gp_Vec(point(edge->first), projectedCenter).Dot(ab) / ab.SquareMagnitude();
                if (t < -1.e-8 || t > 1. + 1.e-8)
                    continue;
                const auto foot = point(edge->first).Translated(ab * t);
                locations.push_back(
                    {{foot.X(), foot.Y(), foot.Z()},
                     {projectedCenter.X(), projectedCenter.Y(), projectedCenter.Z()},
                     foot.Distance(projectedCenter),
                     edge->faces});
            }
        }
        for (std::size_t i = 0; i < locations.size(); ++i)
            length(
                result, point(locations[i].first), point(locations[i].second),
                (i ? layout.right : -layout.up) * 1.2,
                QStringLiteral("До базы %1 · %2 мм").arg(i + 1).arg(number(locations[i].valueMm)),
                height, bendColor, &layout);
        if (opening->outline.size() > 2) {
            BRepBuilderAPI_MakePolygon polygon;
            for (const auto &p : opening->outline)
                polygon.Add(point(p));
            polygon.Close();
            if (polygon.IsDone()) {
                Handle(AIS_Shape) outline = new AIS_Shape(polygon.Shape());
                outline->SetColor(bendColor);
                outline->SetWidth(2.5);
                append(result, outline);
            }
        }
    } else if (selection.kind == MeasurementKind::Face) {
        const auto face = std::find_if(data.faces.begin(), data.faces.end(),
                                       [&](const auto &v) { return v.id == selection.id; });
        if (face == data.faces.end())
            return;
        callout(result, point(face->anchor), layout.up * 1.0,
                QStringLiteral("%1 · S %2 мм²")
                    .arg(face->planar ? (flat ? QStringLiteral("Плоский участок")
                                              : QStringLiteral("Плоская грань"))
                                      : QStringLiteral("Поверхность"),
                         number(face->areaMm2)),
                height, {}, &layout);
        if (flat && face->outline.size() > 2) {
            BRepBuilderAPI_MakePolygon boundary;
            for (const auto &p : face->outline)
                boundary.Add(point(p));
            boundary.Close();
            if (boundary.IsDone()) {
                Handle(AIS_Shape) outline = new AIS_Shape(boundary.Shape());
                outline->SetColor(bendColor);
                outline->SetWidth(2.5);
                append(result, outline);
            }
        }
        for (std::size_t i = 0; i < face->sizes.size(); ++i)
            length(result, point(face->sizes[i].first), point(face->sizes[i].second),
                   (i ? layout.right : -layout.up) * 1.8,
                   QStringLiteral("Грань %1 мм").arg(number(face->sizes[i].valueMm)), height,
                   bendColor, &layout);
    } else if (selection.kind == MeasurementKind::Edge) {
        const auto edge = std::find_if(data.edges.begin(), data.edges.end(),
                                       [&](const auto &v) { return v.id == selection.id; });
        if (edge == data.edges.end())
            return;
        if (edge->straight)
            length(result, point(edge->first), point(edge->second), layout.up * 1.0,
                   QStringLiteral("Ребро %1 мм").arg(number(edge->lengthMm)), height, bendColor,
                   &layout);
        else
            callout(result, point(edge->first), layout.up * 1.0,
                    (edge->radiusMm ? QStringLiteral("R %1 мм · ").arg(number(*edge->radiusMm))
                                    : QString{}) +
                        QStringLiteral("Длина %1 мм").arg(number(edge->lengthMm)),
                    height, layout.right, &layout);
    }
}
// Perpendicular to an actual finite external stock edge. Bounds are only
// paper/viewport extents; their empty corners are not measurement datums.
std::optional<std::array<gp_Pnt, 2>> flatBendEdgeSpan(const application::FlatPatternInfo &info,
                                                      const application::FlatBend &bend) {
    if (!info.measurements)
        return {};
    const auto line = bend.centreLine();
    auto along = gp_Vec(point(line[0]), point(line[1]));
    along.SetZ(0);
    if (along.SquareMagnitude() <= 1.e-12)
        return {};
    along.Normalize();
    if (along.X() < -1.e-8 || (std::abs(along.X()) <= 1.e-8 && along.Y() < 0))
        along.Reverse();
    const auto station = [&](application::PointMm p) { return p.x * along.X() + p.y * along.Y(); };
    const double low = std::min(station(line[0]), station(line[1]));
    const double high = std::max(station(line[0]), station(line[1]));
    const std::unordered_set<std::size_t> datums(info.measurements->datumEdges.begin(),
                                                 info.measurements->datumEdges.end());
    std::optional<std::array<gp_Pnt, 2>> result;
    double best = 0;
    for (const auto &edge : info.measurements->edges) {
        if (!edge.straight || !datums.contains(edge.id))
            continue;
        auto edgeDirection = gp_Vec(point(edge.first), point(edge.second));
        edgeDirection.SetZ(0);
        const double squaredLength = edgeDirection.SquareMagnitude();
        if (squaredLength <= 1.e-12 ||
            edgeDirection.Crossed(along).SquareMagnitude() > squaredLength * 1.e-14)
            continue;
        const double start = std::max(low, std::min(station(edge.first), station(edge.second)));
        const double end = std::min(high, std::max(station(edge.first), station(edge.second)));
        if (end - start <= 1.e-7)
            continue; // Never prolong a short edge to meet the selected line.
        auto atBend = point(line[0]).Translated(along * (start - station(line[0])));
        const auto a = point(edge.first);
        const double foot = gp_Vec(a, atBend).Dot(edgeDirection) / squaredLength;
        if (foot < -1.e-8 || foot > 1. + 1.e-8)
            continue;
        auto atEdge = a.Translated(edgeDirection * std::clamp(foot, 0., 1.));
        atEdge.SetZ(info.bounds.maximum.z);
        atBend.SetZ(info.bounds.maximum.z);
        const double distance = atEdge.Distance(atBend);
        if (distance <= 1.e-7)
            continue;
        // Stable tie break: changing camera/physical sheet side cannot change the datum.
        if (!result || distance < best - 1.e-7 ||
            (std::abs(distance - best) <= 1.e-7 &&
             std::pair{atEdge.X(), atEdge.Y()} < std::pair{(*result)[0].X(), (*result)[0].Y()})) {
            result = std::array{atEdge, atBend};
            best = distance;
        }
    }
    return result;
}
} // namespace

DimensionAnnotations documentDimensions(const application::DocumentMetadata &metadata,
                                        const TopTools_IndexedMapOfShape &faces,
                                        const std::optional<application::RecognizedBend> &bend,
                                        double height, const DimensionLayout &layout,
                                        const application::ModelMeasurements *measurements,
                                        MeasurementSelection selection) {
    DimensionAnnotations result;
    const auto &lo = metadata.bounds.minimum;
    const auto &hi = metadata.bounds.maximum;
    const auto gap = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 0.14;
    if (!(gap > 0.0) || !std::isfinite(gap)) {
        return result;
    }
    {
        length(result, point(lo), {hi.x, lo.y, lo.z}, {0, -gap, 0},
               QStringLiteral("X %1 мм").arg(number(hi.x - lo.x)), height, dimensionColor, &layout);
        length(result, point(lo), {lo.x, hi.y, lo.z}, {-gap, 0, 0},
               QStringLiteral("Y %1 мм").arg(number(hi.y - lo.y)), height, dimensionColor, &layout);
        length(result, {hi.x, hi.y, lo.z}, point(hi), {gap, 0, 0},
               QStringLiteral("Z %1 мм").arg(number(hi.z - lo.z)), height, dimensionColor, &layout);
    }
    if (measurements) {
        struct FlangeGroup {
            const application::MeasuredSpan *span;
            std::vector<std::vector<std::size_t>> walls;
        };
        std::vector<FlangeGroup> groups;
        const double tolerance = std::max(1.e-5, metadata.bendAnalysis.lengthToleranceMm);
        for (const auto &span : measurements->flanges) {
            const std::array first{span.first.x, span.first.y, span.first.z};
            const std::array second{span.second.x, span.second.y, span.second.z};
            const std::array minimum{lo.x, lo.y, lo.z}, maximum{hi.x, hi.y, hi.z};
            bool covered = false;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                bool parallel = true;
                for (std::size_t other = 0; other < 3; ++other)
                    if (other != axis && std::abs(first[other] - second[other]) > tolerance)
                        parallel = false;
                covered =
                    covered ||
                    (parallel &&
                     std::abs(std::min(first[axis], second[axis]) - minimum[axis]) < tolerance &&
                     std::abs(std::max(first[axis], second[axis]) - maximum[axis]) < tolerance &&
                     std::abs(span.valueMm - (maximum[axis] - minimum[axis])) < tolerance);
            }
            if (covered)
                continue;
            const auto same = std::find_if(groups.begin(), groups.end(), [&](const auto &group) {
                return std::abs(group.span->valueMm - span.valueMm) < tolerance;
            });
            if (same != groups.end()) {
                if (std::find(same->walls.begin(), same->walls.end(), span.faces) ==
                    same->walls.end())
                    same->walls.push_back(span.faces);
            } else
                groups.push_back({&span, {span.faces}});
        }
        for (const auto &group : groups) {
            const auto &span = *group.span;
            const auto caption = group.walls.size() == 1
                                     ? QStringLiteral("Полка %1 мм").arg(number(span.valueMm))
                                     : QStringLiteral("Полки %1 мм · %2 шт.")
                                           .arg(number(span.valueMm))
                                           .arg(group.walls.size());
            length(result, point(span.first), point(span.second), -layout.up * .7, caption, height,
                   dimensionColor, &layout);
        }
        selectedMeasurements(result, *measurements, selection, height, layout);
    }
    if (!bend || bend->innerFace == 0 ||
        bend->innerFace > static_cast<std::size_t>(faces.Extent())) {
        return result;
    }
    const auto start = point(bend->axisStart), end = point(bend->axisEnd);
    const auto middle = gp_Pnt((start.XYZ() + end.XYZ()) * .5);
    auto anchor = middle;
    if (measurements) {
        const auto face = std::find_if(measurements->faces.begin(), measurements->faces.end(),
                                       [&](const auto &f) { return f.id == bend->innerFace; });
        if (face != measurements->faces.end())
            anchor = point(face->anchor);
    }
    const gp_Vec axis(start, end);
    if (axis.SquareMagnitude() <= 1.e-14)
        return result;
    const auto caption = bend->trimmedEnds ? QStringLiteral("Осевой габарит %1 мм")
                                           : QStringLiteral("Длина гиба %1 мм");
    auto side = -layout.right;
    side -= axis * (side.Dot(axis) / axis.SquareMagnitude());
    if (side.SquareMagnitude() < 1.e-14)
        side = layout.up;
    length(result, start, end, side, caption.arg(number(bend->lengthMm)), height, bendColor,
           &layout);
    callout(result, anchor, layout.up * 1.0,
            bendCaption(bend->angleRadians, bend->innerRadiusMm, bend->thicknessMm), height, {},
            &layout);
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
MeasurementSelection flatMeasurementAt(const application::ModelMeasurements &data,
                                       const DimensionLayout &layout, QPointF position) {
    if (!layout.project)
        return {};
    const auto inside = [&](const auto &outline) {
        QPolygonF polygon;
        for (const auto &p : outline)
            polygon << layout.project(point(p));
        return polygon.containsPoint(position, Qt::OddEvenFill);
    };
    for (const auto &opening : data.openings)
        if (inside(opening.outline))
            return {MeasurementKind::Opening, opening.id, {}};
    double best = 64;
    MeasurementSelection result;
    for (const auto &edge : data.edges)
        for (std::size_t i = 1; i < edge.outline.size(); ++i) {
            const auto a = layout.project(point(edge.outline[i - 1])),
                       b = layout.project(point(edge.outline[i]));
            const auto ab = b - a;
            const double q = QPointF::dotProduct(ab, ab);
            if (q <= 1.e-12)
                continue;
            const double t = std::clamp(QPointF::dotProduct(position - a, ab) / q, 0., 1.);
            const auto delta = position - (a + ab * t);
            const double distance = QPointF::dotProduct(delta, delta);
            if (distance < best) {
                best = distance;
                result = {MeasurementKind::Edge, edge.id, {}};
            }
        }
    if (result.kind != MeasurementKind::None)
        return result;
    for (const auto &face : data.faces)
        if (inside(face.outline))
            return {MeasurementKind::Face, face.id, {}};
    return {};
}
DimensionAnnotations flatPatternDimensions(const application::FlatPatternInfo &info, double height,
                                           const DimensionLayout &layout,
                                           std::optional<std::size_t> selected, double facing,
                                           MeasurementSelection selection) {
    DimensionAnnotations result;
    const auto &hi = info.bounds.maximum;
    const auto gap = std::max(hi.x, hi.y) * .12;
    length(result, {0, 0, hi.z}, {hi.x, 0, hi.z}, {0, -gap, 0},
           QStringLiteral("Ширина %1 мм").arg(number(hi.x)), height, dimensionColor, &layout);
    length(result, {0, 0, hi.z}, {0, hi.y, hi.z}, {-gap, 0, 0},
           QStringLiteral("Развёртка %1 мм").arg(number(hi.y)), height, dimensionColor, &layout);
    // Directions are relative to the physical flat side facing the viewer. Do not
    // pretend that a nearly edge-on sheet has readable bend annotations.
    if (std::abs(facing) < .15)
        return result;
    const auto bendId = [](std::size_t index) {
        return QStringLiteral("B%1").arg(index + 1, 2, 10, QLatin1Char('0'));
    };
    if (info.measurements)
        selectedMeasurements(result, *info.measurements, selection, height, layout, true);
    const auto active = std::find_if(info.bends.begin(), info.bends.end(), [&](const auto &bend) {
        return selected && bend.bendIndex == *selected;
    });
    if (active != info.bends.end()) {
        const auto line = active->centreLine();
        const gp_Pnt anchor((line[0].x + line[1].x) / 2, (line[0].y + line[1].y) / 2, hi.z);
        const auto direction = active->direction * (facing > 0 ? 1 : -1);
        if (const auto span = flatBendEdgeSpan(info, *active)) {
            gp_Vec along(point(line[0]), point(line[1]));
            if (along.X() < -1.e-8 || (std::abs(along.X()) <= 1.e-8 && along.Y() < 0))
                along.Reverse();
            const auto offset = -along.Normalized() * layout.right.Magnitude() * .7;
            length(result, (*span)[0], (*span)[1], offset,
                   QStringLiteral("%1 мм").arg(number((*span)[0].Distance((*span)[1]))), height,
                   bendColor, &layout);
        }
        callout(result, anchor, layout.up * 1.0,
                QStringLiteral("%1 · %2 · %3° · R %4 · L %5 мм")
                    .arg(bendId(active->bendIndex),
                         direction > 0 ? QStringLiteral("ВВЕРХ") : QStringLiteral("ВНИЗ"),
                         number(180 - active->turnRadians * 180 / std::numbers::pi),
                         number(active->radiusMm), number(active->lengthMm())),
                height, layout.right, &layout);
    }
    // The overview identifies lines; distances appear only for the selected bend.
    struct Marker {
        int axis; // 0: horizontal, 1: vertical, 2: oblique (not grouped).
        double coordinate;
        gp_Pnt anchor;
        QStringList ids;
    };
    std::vector<Marker> markers;
    for (const auto &bend : info.bends) {
        const auto line = bend.centreLine();
        const int axis = std::abs(line[0].y - line[1].y) < 1.e-5   ? 0
                         : std::abs(line[0].x - line[1].x) < 1.e-5 ? 1
                                                                   : 2;
        const double coordinate = axis == 0 ? line[0].y : line[0].x;
        const auto group = std::find_if(markers.begin(), markers.end(), [&](const auto &marker) {
            return axis != 2 && marker.axis == axis &&
                   std::abs(marker.coordinate - coordinate) < 1.e-5;
        });
        if (group != markers.end())
            group->ids << bendId(bend.bendIndex);
        else
            markers.push_back(
                {axis,
                 coordinate,
                 gp_Pnt((line[0].x + line[1].x) / 2, (line[0].y + line[1].y) / 2, hi.z),
                 {bendId(bend.bendIndex)}});
    }
    QFont font(QStringLiteral("sans-serif"));
    font.setPixelSize(qRound(height));
    const QFontMetricsF metrics(font);
    std::vector<QRectF> occupied = result.occupied;
    occupied.insert(occupied.end(), layout.excluded.begin(), layout.excluded.end());
    for (const auto &marker : markers) {
        // The full selected callout already identifies this line.
        if (selected && marker.ids.size() == 1 && marker.ids.front() == bendId(*selected))
            continue;
        const auto position = marker.anchor.Translated(layout.up * .3);
        const auto caption = marker.ids.join('/');
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
    if (anchors.outsideFlanges) {
        const auto &p = *anchors.outsideFlanges;
        const auto first = p[0].Distance(p[1]), second = p[1].Distance(p[2]);
        if (std::abs(first - second) < 1.e-6)
            length(result, p[0], p[1], -layout.up * .9,
                   QStringLiteral("Полки %1 мм · 2 шт.").arg(number(first)), height, dimensionColor,
                   &layout);
        else {
            length(result, p[0], p[1], -layout.right * .9 - layout.up * .3,
                   QStringLiteral("Полка 1 · %1 мм").arg(number(first)), height, dimensionColor,
                   &layout);
            length(result, p[1], p[2], layout.right * .9 - layout.up * .3,
                   QStringLiteral("Полка 2 · %1 мм").arg(number(second)), height, dimensionColor,
                   &layout);
        }
    } else if (anchors.blankEnds) {
        const auto &p = *anchors.blankEnds;
        length(result, p[0], p[1], -layout.up * 1.0,
               QStringLiteral("Габарит %1 мм").arg(number(p[0].Distance(p[1]))), height,
               dimensionColor, &layout);
    }
    length(result, anchors.fixed[1], anchors.fixed[1].Translated(anchors.widthVector),
           -layout.up * 1.0, QStringLiteral("Охват по оси %1 мм").arg(number(model.widthMm)),
           height, dimensionColor, &layout);
    callout(result, anchors.bendMiddle, layout.up * 1.0,
            compact ? QStringLiteral("Угол %1°")
                          .arg(number(180 - anchors.angleRadians * 180 / std::numbers::pi))
                    : bendCaption(anchors.angleRadians, anchors.innerRadiusMm, model.thicknessMm),
            height, {}, &layout);
    return result;
}
} // namespace bending::viewer
