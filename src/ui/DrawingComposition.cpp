#include "DrawingSheetInternal.h"
#include <QImage>
#include <QStringList>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
namespace bending::ui {
using namespace application;
DrawingFlatOrientation DrawingSheet::flatOrientation(const FlatPatternInfo &info,
                                                     DrawingFlatSide side) {
    DrawingFlatOrientation result;
    std::size_t positive = 0, negative = 0;
    for (const auto &bend : info.bends) {
        if (bend.direction != 1 && bend.direction != -1)
            throw std::runtime_error("Не определено направление гиба развёртки");
        (bend.direction > 0 ? positive : negative)++;
    }
    const bool firstReversed = info.positiveZIsSideA && !*info.positiveZIsSideA;
    result.reversed = side == DrawingFlatSide::Automatic
                          ? (negative == positive ? firstReversed : negative > positive)
                          : (side == DrawingFlatSide::First ? firstReversed : !firstReversed);
    if (info.positiveZIsSideA)
        result.sideA = *info.positiveZIsSideA != result.reversed;
    result.up = result.reversed ? negative : positive;
    result.down = result.reversed ? positive : negative;
    return result;
}
QString DrawingSheet::viewName(const BendingDrawing &d, DrawingViewKey key) {
    switch (key.kind) {
    case DrawingViewKind::Projection:
        if (key.index == 0)
            return QStringLiteral("Главный вид");
        if (key.index == 1)
            return (d.classicalViews ? (*d.classicalViews)[1] : d.views[1]).reverseProjection
                       ? QStringLiteral("Вид снизу")
                       : QStringLiteral("Вид сверху");
        return (d.classicalViews ? (*d.classicalViews)[2] : d.views[2]).reverseProjection
                   ? QStringLiteral("Вид справа")
                   : QStringLiteral("Вид слева");
    case DrawingViewKind::Section: {
        const auto &s = d.sections.at(key.index);
        const auto name = QString::fromStdString(s.designation);
        return QStringLiteral("Сечение %1–%1%2%3%4")
            .arg(name,
                 s.repeatCount ? QStringLiteral(" · волна ×%1").arg(s.repeatCount) : QString{},
                 s.locations.size() > 1 ? QStringLiteral(" · %1 места").arg(s.locations.size())
                                        : QString{},
                 s.rotated ? QStringLiteral(" · повёрнуто") : QString{});
    }
    case DrawingViewKind::Profile:
        return QStringLiteral("Профиль %1")
            .arg(QString::fromStdString(d.profiles.at(key.index).designation));
    case DrawingViewKind::Detail:
        return QStringLiteral("Элемент %1")
            .arg(QString::fromStdString(d.details.at(key.index).designation));
    case DrawingViewKind::Auxiliary:
        return QStringLiteral("Вид %1").arg(
            QString::fromStdString(d.auxiliaryViews.at(key.index).designation));
    case DrawingViewKind::Flat:
        return QStringLiteral("Развёртка");
    case DrawingViewKind::Isometric:
        return QStringLiteral("Изометрия");
    }
    throw std::runtime_error("Unknown drawing view");
}
std::vector<DrawingViewChoice> DrawingSheet::defaultViews(const BendingDrawing &d) {
    std::vector<DrawingViewChoice> result;
    for (std::size_t i = 0; i < 3; ++i)
        result.push_back({{DrawingViewKind::Projection, i}});
    for (std::size_t i = 0; i < d.sections.size(); ++i)
        result.push_back({{DrawingViewKind::Section, i}});
    for (std::size_t i = 0; i < d.profiles.size(); ++i)
        result.push_back({{DrawingViewKind::Profile, i}, false});
    for (std::size_t i = 0; i < d.details.size(); ++i)
        result.push_back({{DrawingViewKind::Detail, i}, false});
    for (std::size_t i = 0; i < d.auxiliaryViews.size(); ++i)
        result.push_back({{DrawingViewKind::Auxiliary, i}, false});
    if (d.flatPattern)
        result.push_back({{DrawingViewKind::Flat}});
    result.push_back({{DrawingViewKind::Isometric}});
    return result;
}
namespace {
struct Item {
    std::vector<DrawingViewChoice> choices;
    bool compactDimensions = false;
    bool essentialDimensions = false;
    bool smallOptionalText = false;
};
struct FitKey {
    // Source/side are constant. A stable full item ID identifies immutable
    // choices, scale and annotation policy, not its position in a trial order.
    // Fit uses paper coordinates; restoreFit supplies the actual page.
    std::size_t item;
    std::array<std::uint64_t, 4> area;
    auto operator<=>(const FitKey &) const = default;
};
FitKey fitKey(std::size_t item, const QRectF &area) {
    return {item,
            {std::bit_cast<std::uint64_t>(area.x()), std::bit_cast<std::uint64_t>(area.y()),
             std::bit_cast<std::uint64_t>(area.width()),
             std::bit_cast<std::uint64_t>(area.height())}};
}
struct FitPlacement {
    DrawingViewKey key;
    QRectF area;
    QPointF centre;
    double scale;
    QRectF selectionBounds;
    bool compactDimensions;
};
// No CAD, paths, dimension recordings or painter state retained by the cache.
// nullopt is an ordinary fit failure, never cancellation or a spent budget.
struct CachedFit {
    std::optional<std::vector<FitPlacement>> placements;
    std::string failure; // Kept only for a whole-sheet fit.
};
std::vector<FitPlacement> cacheFit(const std::vector<DrawingPlacedView> &views) {
    std::vector<FitPlacement> result;
    result.reserve(views.size());
    for (const auto &view : views)
        result.push_back({view.key, view.area, view.centre, view.scale, view.selectionBounds,
                          view.compactDimensions});
    return result;
}
std::optional<std::vector<DrawingPlacedView>> restoreFit(const CachedFit &cached, int page) {
    if (!cached.placements)
        return {};
    std::vector<DrawingPlacedView> result;
    result.reserve(cached.placements->size());
    for (const auto &view : *cached.placements) {
        result.push_back(
            {view.key, page, view.area, view.centre, view.scale, view.selectionBounds});
        result.back().compactDimensions = view.compactDimensions;
    }
    return result;
}
double width(const DrawingView &v) {
    return v.maximum.x - v.minimum.x - (v.omittedX ? (*v.omittedX)[1] - (*v.omittedX)[0] : 0);
}
double height(const DrawingView &v) {
    return v.maximum.y - v.minimum.y;
}
double dimensionPadding(const DrawingView *view, bool verticalSpace, bool compact = false,
                        bool essential = false) {
    if (!view)
        return 44;
    std::size_t count = 0;
    const auto usesSpace = [verticalSpace](const DrawingDimension &d) {
        return (std::abs(d.end.x - d.start.x) >= std::abs(d.end.y - d.start.y)) == verticalSpace;
    };
    for (const auto *dimensions : {&view->dimensions, &view->sectionDimensions})
        count += std::count_if(dimensions->begin(), dimensions->end(), usesSpace);
    count += std::count_if(
        view->holeDimensions.begin(), view->holeDimensions.end(), [&](const auto &dimension) {
            return (!essential || dimension.openingPatternReference) && usesSpace(dimension);
        });
    // Axial spans share a chain line. Other dimensions need individual lanes,
    // distributed over two sides; this is only an allocation hint, never a fit proof.
    count += std::any_of(view->axialDimensions.begin(), view->axialDimensions.end(), usesSpace);
    // A third allocation can use the renderer's two-sided horizontal lane bound:
    // 9 mm caption + 15/12 mm first lanes + 7 mm for each further dimension.
    // It is only a paper-space hint; paintPlaced still proves the complete fit.
    return std::max(44., (compact && verticalSpace ? 22. : 44.) + count * 7.);
}
double minimumScale(const BendingDrawing &d, DrawingViewKey key, const DrawingView &v,
                    QSizeF referenceSize) {
    if (key.kind == DrawingViewKind::Projection)
        return .01;
    if (key.kind == DrawingViewKind::Flat)
        return standardDrawingScale(std::min({1., 122. / width(v), 46. / height(v)})).value_or(.01);
    double readable = 0;
    auto measured = v.dimensionedBends;
    if (key.kind == DrawingViewKind::Detail && measured.empty())
        measured.push_back(d.details.at(key.index).bendIndex);
    for (auto i : measured) {
        const auto &b = d.bends.at(i);
        readable =
            std::max({readable, .75 / b.thicknessMm, 1.5 / (b.innerRadiusMm + b.thicknessMm)});
    }
    const double reference = standardDrawingScale(std::min(referenceSize.width() / width(v),
                                                           referenceSize.height() / height(v)))
                                 .value_or(.001);
    return std::min(reference, readable > 0 ? readable : reference * .5);
}
struct SingleViewFit {
    double width;
    double height;
    double minimum;
    std::optional<double> fixed;
    double automaticCap;
    double maximum(QRectF area) const {
        return std::min((area.width() - 70) / width, (area.height() - 48) / height);
    }
    std::optional<double> initial(double maximumScale) const {
        return fixed ? fixed : standardDrawingScale(std::min(maximumScale, automaticCap));
    }
};
SingleViewFit singleViewFit(const BendingDrawing &d, const DrawingViewChoice &choice,
                            const DrawingView &view, QRectF fullArea, DrawingPaperFormat format) {
    // Reserve physical space for annotations around large sections on A4.
    const QSizeF referenceSize =
        format == DrawingPaperFormat::A4
            ? QSizeF((fullArea.width() - 70) * .75, (fullArea.height() - 48) * .75)
            : QSizeF(160, 120);
    return {width(view), height(view),
            choice.scale ? .00001 : minimumScale(d, choice.key, view, referenceSize), choice.scale,
            choice.key.kind == DrawingViewKind::Flat ? 1. : 100.};
}
std::vector<DrawingPlacedView> fitItem(const BendingDrawing &d, const Item &item, QRectF area,
                                       QRectF fullArea, DrawingPaperFormat format,
                                       DrawingFlatSide side, int page,
                                       drawing::LayoutContext &context) {
    context.checkpoint();
    std::vector<DrawingPlacedView> placed;
    std::string lastError;
    const bool family = item.choices.front().key.kind == DrawingViewKind::Projection;
    if (family) {
        std::array<const DrawingView *, 3> views{};
        std::optional<double> fixed;
        for (const auto &choice : item.choices) {
            views.at(choice.key.index) = &context.view(d, choice.key);
            if (choice.scale) {
                if (fixed && *fixed != *choice.scale)
                    throw std::runtime_error("Масштабы связанных видов должны совпадать");
                fixed = choice.scale;
            }
        }
        const double primaryW =
            std::max(views[0] ? width(*views[0]) : 0., views[1] ? width(*views[1]) : 0.);
        const double primaryH =
            std::max(views[0] ? height(*views[0]) : 0., views[2] ? height(*views[2]) : 0.);
        const bool top = views[1] && (views[0] || views[2]);
        const bool lateral = views[2] && (views[0] || views[1]);
        constexpr double margin = 22, gap = 6;
        const double rawW =
            (primaryW > 0 ? primaryW : width(*views[2])) + (lateral ? width(*views[2]) : 0.);
        const double rawH =
            (primaryH > 0 ? primaryH : height(*views[1])) + (top ? height(*views[1]) : 0.);
        const double fit = std::min(
            (area.width() - (lateral ? 4 * margin + gap : 2 * margin) - 8) / std::max(rawW, 1.e-8),
            (area.height() - (top ? 4 * margin + gap : 2 * margin)) / std::max(rawH, 1.e-8));
        const double fullFit = std::min(
            (fullArea.width() - (lateral ? 4 * margin + gap : 2 * margin) - 8) /
                std::max(rawW, 1.e-8),
            (fullArea.height() - (top ? 4 * margin + gap : 2 * margin)) / std::max(rawH, 1.e-8));
        const double minimum =
            fixed ? .001 : std::max(.001, standardDrawingScale(fullFit * .5).value_or(.001));
        auto scale = fixed ? fixed : standardDrawingScale(fit);
        for (int attempt = 0; attempt < 7 && scale && *scale + 1.e-9 >= minimum; ++attempt) {
            if (*scale > fit + 1.e-9)
                throw std::runtime_error("Связанные виды не помещаются в выбранном масштабе");
            // Keep the established split first. If it fails, account for dimension
            // density before reducing scale: short/thin views can reserve paper
            // that the heavily dimensioned main view actually needs.
            for (int allocation = 0; allocation < 3; ++allocation) {
                const auto padding = [&](std::size_t index, bool vertical) {
                    return dimensionPadding(views[index], vertical, allocation == 2,
                                            item.essentialDimensions);
                };
                const double mainW =
                    allocation ? std::max(padding(0, false), padding(1, false)) : 2 * margin;
                const double mainH =
                    allocation ? std::max(padding(0, true), padding(2, true)) : 2 * margin;
                const double sidePadding = allocation ? padding(2, false) : 2 * margin;
                const double topPadding = allocation ? padding(1, true) : 2 * margin;
                const double sideW = lateral ? (area.width() - gap) *
                                                   (width(*views[2]) * *scale + sidePadding) /
                                                   (rawW * *scale + mainW + sidePadding)
                                             : 0;
                const double topH = top ? (area.height() - gap) *
                                              (height(*views[1]) * *scale + topPadding) /
                                              (rawH * *scale + mainH + topPadding)
                                        : 0;
                const bool sideLeft = lateral && views[2]->reverseProjection;
                const bool topAbove = top && views[1]->reverseProjection;
                QRectF main(area.left() + (sideLeft ? sideW + gap : 0),
                            area.top() + (topAbove ? topH + gap : 0),
                            area.width() - (lateral ? sideW + gap : 0),
                            area.height() - (top ? topH + gap : 0));
                placed.clear();
                for (const auto &choice : item.choices) {
                    auto cell = main;
                    if (choice.key.index == 1 && top)
                        cell = {main.left(), topAbove ? area.top() : main.bottom() + gap,
                                main.width(), topH};
                    if (choice.key.index == 2 && lateral)
                        cell = {sideLeft ? area.left() : main.right() + gap, main.top(), sideW,
                                main.height()};
                    // Equal shifts retain projection alignment. The balanced inset
                    // is (9 mm caption + 15 mm top lane - 12 mm bottom lane) / 2.
                    placed.push_back({choice.key, page, cell,
                                      cell.center() + QPointF(0, allocation == 2 ? 6 : 0), *scale});
                }
                for (auto &at : placed) {
                    at.compactDimensions = item.compactDimensions;
                    at.essentialDimensions = item.essentialDimensions;
                    at.smallOptionalText = item.smallOptionalText;
                }
                QImage scratch(1, 1, QImage::Format_RGB32);
                QPainter painter(&scratch);
                std::vector<QRectF> occupied;
                auto painting = placed.front().key;
                try {
                    for (auto &p : placed) {
                        painting = p.key;
                        drawing::paintPlaced(painter, d, p, side, occupied, &p.selectionBounds,
                                             context);
                    }
                    return placed;
                } catch (const DrawingLayoutInterrupted &) {
                    throw;
                } catch (const std::runtime_error &e) {
                    lastError = (DrawingSheet::viewName(d, painting) + QStringLiteral(": ") +
                                 QString::fromUtf8(e.what()))
                                    .toStdString();
                    if (fixed && allocation == 2)
                        throw std::runtime_error(lastError);
                }
            }
            scale = standardDrawingScale(*scale * .8);
        }
    } else {
        const auto &choice = item.choices.front();
        const auto &v = context.view(d, choice.key);
        const auto geometry = singleViewFit(d, choice, v, fullArea, format);
        const double fit = geometry.maximum(area);
        auto scale = geometry.initial(fit);
        for (int attempt = 0; attempt < 8 && scale && *scale + 1.e-9 >= geometry.minimum;
             ++attempt) {
            if (*scale > fit + 1.e-9)
                throw std::runtime_error("Вид не помещается в выбранном масштабе");
            DrawingPlacedView at{choice.key, page, area, area.center() + QPointF(0, 4), *scale};
            at.compactDimensions = item.compactDimensions;
            at.essentialDimensions = item.essentialDimensions;
            at.smallOptionalText = item.smallOptionalText;
            QImage scratch(1, 1, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            try {
                drawing::paintPlaced(painter, d, at, side, occupied, &at.selectionBounds, context);
                return {at};
            } catch (const DrawingLayoutInterrupted &) {
                throw;
            } catch (const std::runtime_error &e) {
                lastError = e.what();
                if (choice.scale)
                    throw;
            }
            scale = standardDrawingScale(*scale * .8);
        }
    }
    throw std::runtime_error(lastError.empty() ? "Недостаточно места для читаемого вида"
                                               : lastError);
}
std::vector<std::vector<QRectF>> cells(std::size_t count, QRectF area) {
    std::vector<std::vector<QRectF>> result;
    for (int columns : {1, 2, 3}) {
        if (static_cast<std::size_t>(columns) > count)
            continue;
        const int rows = (static_cast<int>(count) + columns - 1) / columns;
        const double w = (area.width() - 6 * (columns - 1)) / columns,
                     h = (area.height() - 6 * (rows - 1)) / rows;
        std::vector<QRectF> grid;
        for (std::size_t i = 0; i < count; ++i)
            grid.emplace_back(area.left() + (i % columns) * (w + 6),
                              area.top() + (i / columns) * (h + 6), w, h);
        result.push_back(std::move(grid));
    }
    if (count > 1)
        for (bool horizontal : {false, true})
            for (double fraction : {.4, .5, .6, .65, .7, .75})
                for (int columns : {1, 2}) {
                    if (static_cast<std::size_t>(columns) >= count)
                        continue;
                    QRectF first = area, rest = area;
                    if (horizontal) {
                        first.setHeight(area.height() * fraction - 3);
                        rest.setTop(first.bottom() + 6);
                    } else {
                        first.setWidth(area.width() * fraction - 3);
                        rest.setLeft(first.right() + 6);
                    }
                    std::vector<QRectF> grid{first};
                    const int rows = (static_cast<int>(count) - 2 + columns) / columns;
                    const double w = (rest.width() - 6 * (columns - 1)) / columns,
                                 h = (rest.height() - 6 * (rows - 1)) / rows;
                    for (std::size_t i = 0; i + 1 < count; ++i)
                        grid.emplace_back(rest.left() + (i % columns) * (w + 6),
                                          rest.top() + (i / columns) * (h + 6), w, h);
                    result.push_back(std::move(grid));
                }
    return result;
}

struct UnplacedItem {
    std::size_t item;
    std::string reason;
};
template <typename FitsCell>
std::optional<std::vector<int>> matchCells(std::size_t count, const FitsCell &fitsCell) {
    std::vector<int> owner(count, -1);
    const auto match = [&](auto &&self, std::size_t i, std::vector<bool> &visited) -> bool {
        for (std::size_t j = 0; j < count; ++j)
            if (!visited[j] && fitsCell(i, j)) {
                visited[j] = true;
                if (owner[j] < 0 || self(self, static_cast<std::size_t>(owner[j]), visited)) {
                    owner[j] = static_cast<int>(i);
                    return true;
                }
            }
        return false;
    };
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<bool> visited(count);
        if (!match(match, i, visited))
            return {};
    }
    return owner;
}
std::optional<UnplacedItem> packItems(const std::vector<Item> &items,
                                      const std::vector<std::size_t> &order, DrawingSheetPlan &plan,
                                      std::map<FitKey, CachedFit> &fitCache,
                                      drawing::LayoutContext &context,
                                      bool allowEssential = false) {
    struct FitAttempt {
        std::optional<std::vector<DrawingPlacedView>> placed;
        bool checked = false;
    };
    const auto cacheLimit = std::min(context.limits.maximumCachedFits, std::size_t{4096});
    const auto fullArea = drawing::contentArea(DrawingSheet::paperSizeMm(plan.paperFormat));
    std::vector<std::optional<SingleViewFit>> singleViews(items.size());
    std::size_t begin = 0;
    while (begin < order.size()) {
        context.checkpoint();
        bool packed = false;
        std::string fullAreaFailure;
        for (auto count = std::min(std::size_t{6}, order.size() - begin); count && !packed; --count)
            for (const auto &areas : cells(count, fullArea)) {
                context.checkpoint();
                // Reject a grid only when its cells cannot even accommodate the
                // standalone views at the scales that fitItem is allowed to try.
                const auto canFit = [&](std::size_t i, std::size_t j) {
                    const auto id = order[begin + i];
                    const auto &choice = items[id].choices.front();
                    if (choice.key.kind == DrawingViewKind::Projection)
                        return true;
                    auto &geometry = singleViews[id];
                    if (!geometry)
                        geometry = singleViewFit(*plan.source, choice,
                                                 context.view(*plan.source, choice.key), fullArea,
                                                 plan.paperFormat);
                    const auto maximum = geometry->maximum(areas[j]);
                    const auto scale = geometry->initial(maximum);
                    return scale && *scale + 1.e-9 >= geometry->minimum &&
                           *scale <= maximum + 1.e-9;
                };
                if (count > 1 && !matchCells(count, canFit))
                    continue;
                std::vector fits(count, std::vector<FitAttempt>(count));
                // Matching explores cells in the same order, evaluating only the pairs it visits.
                const auto fitsCell = [&](std::size_t i, std::size_t j) {
                    context.checkpoint();
                    auto &attempt = fits[i][j];
                    if (attempt.checked)
                        return attempt.placed.has_value();
                    attempt.checked = true;
                    const auto key = fitKey(order[begin + i], areas[j]);
                    const bool wholeSheet = count == 1 && areas[j] == fullArea;
                    if (const auto cached = fitCache.find(key); cached != fitCache.end()) {
                        attempt.placed = restoreFit(cached->second, plan.pages);
                        if (wholeSheet)
                            fullAreaFailure = cached->second.failure;
                        ++plan.fitCacheHits;
                        return attempt.placed.has_value();
                    }
                    ++plan.fitCalls;
                    try {
                        attempt.placed =
                            fitItem(*plan.source, items[order[begin + i]], areas[j], fullArea,
                                    plan.paperFormat, plan.flatSide, plan.pages, context);
                        if (fitCache.size() < cacheLimit)
                            fitCache.emplace(key, CachedFit{cacheFit(*attempt.placed), {}});
                    } catch (const DrawingLayoutInterrupted &) {
                        throw;
                    } catch (const std::runtime_error &error) {
                        if (wholeSheet)
                            fullAreaFailure = error.what();
                        if (fitCache.size() < cacheLimit)
                            fitCache.emplace(key, CachedFit{{}, wholeSheet ? fullAreaFailure : ""});
                    }
                    return attempt.placed.has_value();
                };
                const auto owner = matchCells(count, fitsCell);
                if (!owner)
                    continue;
                for (std::size_t j = 0; j < count; ++j) {
                    const auto &at = *fits[static_cast<std::size_t>((*owner)[j])][j].placed;
                    plan.views.insert(plan.views.end(), at.begin(), at.end());
                }
                begin += count;
                ++plan.pages;
                packed = true;
                break;
            }
        if (!packed) {
            const auto &item = items[order[begin]];
            if (allowEssential &&
                std::any_of(item.choices.begin(), item.choices.end(), [&](const auto &choice) {
                    const auto &v = context.view(*plan.source, choice.key);
                    return !v.holeDimensions.empty() ||
                           (!v.sectionDimensions.empty() &&
                            (plan.source->bends.empty() ||
                             choice.key.kind == DrawingViewKind::Flat)) ||
                           (choice.key.kind == DrawingViewKind::Flat && !v.axialDimensions.empty());
                })) {
                auto fallback = item;
                fallback.compactDimensions = true;
                fallback.smallOptionalText = true;
                std::vector<DrawingPlacedView> at;
                const auto now = std::chrono::steady_clock::now();
                const auto deadline = std::min(now + std::chrono::milliseconds(120),
                                               now + (context.deadline - now) / 10);
                const auto remaining = context.limits.maximumDimensionProbes - context.probes;
                if (deadline > now && remaining >= 8) {
                    context.retentionBudget = {
                        deadline, context.probes + std::min<std::size_t>(2048, remaining / 8)};
                    try {
                        at = fitItem(*plan.source, fallback, fullArea, fullArea, plan.paperFormat,
                                     plan.flatSide, plan.pages, context);
                    } catch (const drawing::LayoutContext::RetentionLimit &) {
                    } catch (const DrawingLayoutInterrupted &) {
                        throw;
                    } catch (const std::runtime_error &) {
                    }
                    context.retentionBudget.reset();
                }
                if (at.empty()) {
                    fallback.essentialDimensions = true;
                    try {
                        at = fitItem(*plan.source, fallback, fullArea, fullArea, plan.paperFormat,
                                     plan.flatSide, plan.pages, context);
                    } catch (const DrawingLayoutInterrupted &) {
                        throw;
                    } catch (const std::runtime_error &) {
                        // Keep the detailed failure from the original layout.
                    }
                }
                if (!at.empty()) {
                    plan.views.insert(plan.views.end(), at.begin(), at.end());
                    ++begin;
                    ++plan.pages;
                    continue;
                }
            }
            const auto name =
                items[order[begin]].choices.size() > 1
                    ? QStringLiteral("Связанные виды")
                    : DrawingSheet::viewName(*plan.source, items[order[begin]].choices.front().key);
            return UnplacedItem{order[begin],
                                (name +
                                 QStringLiteral(": размеры или выноски не помещаются на %1. "
                                                "Выберите «Авто» или меньший масштаб. ")
                                     .arg(DrawingSheet::paperName(plan.paperFormat)) +
                                 QString::fromStdString(fullAreaFailure))
                                    .toStdString()};
        }
    }
    return {};
}
// Stable IDs describe immutable inputs, including the annotation policy. Never
// overwrite an item that already participated in a FitKey.
std::size_t internItem(std::vector<Item> &items, Item item) {
    const auto same = [&](const auto &old) {
        return old.compactDimensions == item.compactDimensions &&
               old.choices.size() == item.choices.size() &&
               std::equal(old.choices.begin(), old.choices.end(), item.choices.begin(),
                          [](const auto &a, const auto &b) {
                              return a.key == b.key && a.visible == b.visible && a.scale == b.scale;
                          });
    };
    const auto found = std::find_if(items.begin(), items.end(), same);
    if (found != items.end())
        return static_cast<std::size_t>(found - items.begin());
    items.push_back(std::move(item));
    return items.size() - 1;
}
std::vector<double> familyScales(const BendingDrawing &drawing, const Item &family,
                                 drawing::LayoutContext &context, DrawingPaperFormat format) {
    std::array<const DrawingView *, 3> views{};
    std::optional<double> fixed;
    for (const auto &choice : family.choices) {
        views.at(choice.key.index) = &context.view(drawing, choice.key);
        if (choice.scale) {
            if (fixed && fixed != choice.scale)
                throw std::runtime_error("Масштабы связанных видов должны совпадать");
            fixed = choice.scale;
        }
    }
    if (fixed)
        return {*fixed};
    const double primaryW =
        std::max(views[0] ? width(*views[0]) : 0., views[1] ? width(*views[1]) : 0.);
    const double primaryH =
        std::max(views[0] ? height(*views[0]) : 0., views[2] ? height(*views[2]) : 0.);
    const bool top = views[1] && (views[0] || views[2]);
    const bool lateral = views[2] && (views[0] || views[1]);
    const double rawW =
        (primaryW > 0 ? primaryW : width(*views[2])) + (lateral ? width(*views[2]) : 0.);
    const double rawH =
        (primaryH > 0 ? primaryH : height(*views[1])) + (top ? height(*views[1]) : 0.);
    constexpr double margin = 22, gap = 6;
    const auto fullArea = drawing::contentArea(DrawingSheet::paperSizeMm(format));
    const double fit = std::min(
        (fullArea.width() - (lateral ? 4 * margin + gap : 2 * margin) - 8) / std::max(rawW, 1.e-8),
        (fullArea.height() - (top ? 4 * margin + gap : 2 * margin)) / std::max(rawH, 1.e-8));
    // On A4, the common scale for separated projections can exceed the scale
    // estimated for all three together. Start from the largest individual view.
    double start = fit;
    if (format == DrawingPaperFormat::A4) {
        double largestWidth = 0., largestHeight = 0.;
        for (const auto &view : views)
            if (view) {
                largestWidth = std::max(largestWidth, width(*view));
                largestHeight = std::max(largestHeight, height(*view));
            }
        start = std::min((fullArea.width() - 2 * margin - 8) / largestWidth,
                         (fullArea.height() - 2 * margin) / largestHeight);
    }
    const double minimum = std::max(.001, standardDrawingScale(fit * .5).value_or(.001));
    std::vector<double> result;
    for (auto scale = standardDrawingScale(start);
         scale && result.size() < 7 && *scale + 1.e-9 >= minimum;
         scale = standardDrawingScale(*scale * .8))
        result.push_back(*scale);
    return result;
}
std::vector<std::vector<unsigned>> familyPartitions(const Item &family) {
    unsigned mask = 0;
    for (const auto &choice : family.choices)
        mask |= 1u << choice.key.index;
    if (mask == 7)
        return {{5, 2}, {3, 4}, {1, 6}, {1, 2, 4}};
    if (std::popcount(mask) == 2) {
        std::vector<unsigned> parts;
        for (unsigned i = 0; i < 3; ++i)
            if (mask & (1u << i))
                parts.push_back(1u << i);
        return {parts};
    }
    return {};
}
void linkProjectionSheets(DrawingSheetPlan &plan, drawing::LayoutContext &context) {
    std::vector<std::size_t> projections;
    for (std::size_t i = 0; i < plan.views.size(); ++i)
        if (plan.views[i].key.kind == DrawingViewKind::Projection)
            projections.push_back(i);
    std::sort(projections.begin(), projections.end(),
              [&](auto a, auto b) { return plan.views[a].key.index < plan.views[b].key.index; });
    if (projections.size() < 2)
        return;
    auto &main = plan.views[projections.front()];
    const auto reference = [&](const auto &at) {
        return DrawingSheet::viewName(*plan.source, at.key) +
               QStringLiteral(" — лист %1").arg(at.page + 1);
    };
    QStringList links;
    for (std::size_t i = 1; i < projections.size(); ++i) {
        auto &at = plan.views[projections[i]];
        if (at.scale != main.scale)
            throw std::runtime_error("Масштабы связанных видов должны совпадать");
        if (at.page != main.page) {
            at.sheetLinks = reference(main);
            links << reference(at);
        }
    }
    main.sheetLinks = links.join(QStringLiteral("; "));
    if (links.isEmpty())
        return;
    // Prove the complete captions/annotations before recording any vector plan.
    // No late overlay may invalidate an otherwise accepted sheet.
    for (int page = 0; page < plan.pages; ++page) {
        QImage scratch(1, 1, QImage::Format_RGB32);
        QPainter painter(&scratch);
        std::vector<QRectF> occupied;
        for (auto &at : plan.views)
            if (at.page == page)
                drawing::paintPlaced(painter, *plan.source, at, plan.flatSide, occupied,
                                     &at.selectionBounds, context);
    }
}

void retainOptionalDimensions(DrawingSheetPlan &plan, drawing::LayoutContext &context) {
    if (std::none_of(plan.views.begin(), plan.views.end(),
                     [](const auto &at) { return at.essentialDimensions; }))
        return;
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto deadline =
        std::min(now + std::chrono::milliseconds(400), now + (context.deadline - now) / 10);
    const auto remaining = context.limits.maximumDimensionProbes - context.probes;
    const auto probeEnd = context.probes + std::min<std::size_t>(4096, remaining / 4);
    // The fallback is already valid. Improving it must leave budget for recording.
    struct ResetBudget {
        drawing::LayoutContext &context;
        ~ResetBudget() {
            context.retentionBudget.reset();
        }
    } reset{context};
    struct Candidate {
        DrawingPlacedView *placed;
        DrawingDimensionId id;
        int priority;
    };
    constexpr std::size_t maximumTrials = 32;
    std::vector<Candidate> candidates;
    const auto order = [](const Candidate &a, const Candidate &b) {
        return std::pair{a.priority, a.id.second} < std::pair{b.priority, b.id.second};
    };
    for (auto &at : plan.views) {
        if (!at.essentialDimensions)
            continue;
        const auto &v = context.view(*plan.source, at.key);
        const bool flat = at.key.kind == DrawingViewKind::Flat;
        const bool plate = plan.source->bends.empty();
        const auto add = [&](DrawingDimensionSource source, const auto &list, int priority) {
            std::size_t considered = 0;
            for (std::size_t i = 0; i < list.size() && considered < maximumTrials; ++i)
                if (drawing::optionalDimension(source, flat, plate, list[i])) {
                    candidates.push_back({&at, {source, i}, priority});
                    ++considered;
                }
        };
        add(DrawingDimensionSource::Section, v.sectionDimensions, 0);
        add(DrawingDimensionSource::Hole, v.holeDimensions, 1);
        add(DrawingDimensionSource::Axial, v.axialDimensions, 2);
    }
    std::stable_sort(candidates.begin(), candidates.end(), order);
    for (std::size_t i = 0; i < std::min(candidates.size(), maximumTrials); ++i) {
        if (Clock::now() >= deadline || context.probes >= probeEnd)
            break;
        const auto &candidate = candidates[i];
        auto &at = *candidate.placed;
        context.retentionBudget = {
            deadline, context.probes + std::min<std::size_t>(384, probeEnd - context.probes)};
        at.retainedDimensions.push_back(candidate.id);
        try {
            QImage scratch(1, 1, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            for (const auto &other : plan.views)
                if (other.page == at.page)
                    drawing::paintPlaced(painter, *plan.source, other, plan.flatSide, occupied,
                                         nullptr, context);
        } catch (const drawing::LayoutContext::RetentionLimit &) {
            at.retainedDimensions.pop_back();
        } catch (const DrawingLayoutInterrupted &) {
            throw;
        } catch (const std::runtime_error &) {
            at.retainedDimensions.pop_back();
        }
    }
}
} // namespace
std::shared_ptr<const DrawingSheetPlan>
DrawingSheet::compose(BendingDrawingPtr source, const std::vector<DrawingViewChoice> &choices,
                      DrawingFlatSide side, std::stop_token stop, DrawingLayoutLimits limits,
                      DrawingPaperFormat format) {
    drawing::LayoutContext context(stop, limits);
    context.checkpoint();
    if (!source)
        throw std::runtime_error("Нет данных чертежа");
    const auto available = defaultViews(*source);
    auto plan = std::make_shared<DrawingSheetPlan>();
    plan->source = source;
    plan->choices = choices;
    context.choices = &plan->choices;
    plan->flatSide = side;
    plan->paperFormat = format;
    const auto paper = paperSizeMm(format);
    const auto frame = drawing::frameArea(paper);
    const auto titleLeft = frame.right() - drawing::titleBlockWidth;
    const QRectF isoArea(25, frame.bottom() - 41, titleLeft - 35, 38);
    const auto isoFit = [&](const DrawingView &v) {
        return std::min((isoArea.width() - 20) / width(v), 24. / height(v));
    };
    std::vector<Item> items;
    Item family;
    std::optional<DrawingViewChoice> iso;
    std::vector<DrawingViewKey> seen;
    for (const auto &choice : choices) {
        if (std::find(seen.begin(), seen.end(), choice.key) != seen.end() ||
            std::none_of(available.begin(), available.end(),
                         [&](const auto &v) { return v.key == choice.key; }))
            throw std::runtime_error("Недействительный или повторный вид");
        seen.push_back(choice.key);
        if (choice.scale && (!std::isfinite(*choice.scale) || *choice.scale <= 0 ||
                             standardDrawingScale(*choice.scale) != choice.scale))
            throw std::runtime_error("Выберите стандартный масштаб");
        if (!choice.visible)
            continue;
        if (choice.key.kind == DrawingViewKind::Projection)
            family.choices.push_back(choice);
        else if (choice.key.kind == DrawingViewKind::Isometric)
            iso = choice;
        else
            items.push_back({{choice}});
    }
    if (!family.choices.empty())
        items.insert(items.begin(), family);
    if (items.empty())
        throw std::runtime_error("Выберите хотя бы один размерный вид");
    if (iso && iso->scale) {
        const auto &v = context.view(*source, iso->key);
        const double compactFit = isoFit(v);
        if (*iso->scale > compactFit + 1.e-9) {
            // An explicit larger isometric view participates in normal sheet packing.
            items.push_back({{*iso}});
            iso.reset();
        }
    }
    std::map<FitKey, CachedFit> fitCache;
    std::vector<std::size_t> originalOrder(items.size());
    std::iota(originalOrder.begin(), originalOrder.end(), 0);
    const auto reset = [&] {
        plan->views.clear();
        plan->pages = 0;
    };
    const auto fullLayout = [&]() -> std::optional<UnplacedItem> {
        auto failure = packItems(items, originalOrder, *plan, fitCache, context);
        if (!failure)
            return {};
        const auto originalFailure = *failure;
        if (family.choices.empty() || failure->item != 0 || plan->pages != 0)
            return failure;
        // Validate the common explicit scale before considering any new policy.
        const auto scales = familyScales(*source, family, context, format);
        auto compactFamily = family;
        compactFamily.compactDimensions = true;
        const auto compactId = internItem(items, std::move(compactFamily));
        auto order = originalOrder;
        order.front() = compactId;
        reset();
        failure = packItems(items, order, *plan, fitCache, context);
        if (failure && failure->item != compactId)
            return failure;
        if (failure) {
            bool accepted = false;
            const auto partitions = familyPartitions(family);
            for (double scale : scales) {
                for (const auto &partition : partitions) {
                    for (bool compact : {false, true}) {
                        context.checkpoint();
                        order.clear();
                        for (auto mask : partition) {
                            Item part;
                            part.compactDimensions = compact;
                            for (auto choice : family.choices)
                                if (mask & (1u << choice.key.index)) {
                                    choice.scale = scale;
                                    part.choices.push_back(choice);
                                }
                            order.push_back(internItem(items, std::move(part)));
                        }
                        order.insert(order.end(), originalOrder.begin() + 1, originalOrder.end());
                        reset();
                        failure = packItems(items, order, *plan, fitCache, context);
                        if (failure) {
                            if (items[failure->item].choices.front().key.kind !=
                                DrawingViewKind::Projection)
                                return failure;
                            continue;
                        }
                        try {
                            linkProjectionSheets(*plan, context);
                            accepted = true;
                        } catch (const DrawingLayoutInterrupted &) {
                            throw;
                        } catch (const std::runtime_error &) {
                            continue;
                        }
                        break;
                    }
                    if (accepted)
                        break;
                }
                if (accepted)
                    break;
            }
            if (!accepted)
                return originalFailure;
        }
        return {};
    };
    if (fullLayout()) {
        reset();
        if (const auto failure = packItems(items, originalOrder, *plan, fitCache, context, true))
            throw std::runtime_error(failure->reason);
    }
    if (iso)
        for (int page = 0; page < plan->pages; ++page) {
            const auto &v = context.view(*source, iso->key);
            const double fit = isoFit(v);
            const auto scale = iso->scale ? iso->scale : standardDrawingScale(fit);
            if (!scale || *scale > fit + 1.e-9)
                throw std::runtime_error("Изометрия не помещается в выбранном масштабе");
            plan->views.push_back(
                {iso->key, page, isoArea, isoArea.center() + QPointF(0, 4), *scale});
            auto &at = plan->views.back();
            const QSizeF size(width(v) * *scale, height(v) * *scale);
            at.selectionBounds =
                QRectF(at.centre - QPointF(size.width() / 2, size.height() / 2), size)
                    .adjusted(-2, -2, 2, 2);
        }
    std::set<std::size_t> covered;
    for (const auto &at : plan->views)
        if (at.key.kind != DrawingViewKind::Flat && at.key.kind != DrawingViewKind::Isometric) {
            const auto &v = context.view(*source, at.key);
            covered.insert(v.dimensionedBends.begin(), v.dimensionedBends.end());
        }
    for (std::size_t i = 0; i < source->bends.size(); ++i)
        if (!source->bends[i].diagnostic) {
            ++plan->coverage.total;
            if (covered.contains(i))
                ++plan->coverage.dimensioned;
            else
                plan->coverage.missing.push_back(i);
        }
    retainOptionalDimensions(*plan, context);
    drawing::recordViews(*plan, context);
    plan->dimensionProbes = context.probes;
    plan->recordingBytes = context.bytes;
    plan->fitCacheEntries = fitCache.size();
    return plan;
}
namespace drawing {
void paintComposition(QPainter &p, const BendingDrawing &d, const DrawingTitle &title, int page) {
    const auto &plan = *title.plan;
    if (plan.source.get() != &d || page < 0 || page >= plan.pages)
        throw std::runtime_error("Устаревшая компоновка чертежа");
    paintCompositionFrame(p, d, title, plan, page);
    for (const auto &at : plan.views)
        if (at.page == page)
            paintRecorded(p, at, plan.flatSide);
}
} // namespace drawing
} // namespace bending::ui
