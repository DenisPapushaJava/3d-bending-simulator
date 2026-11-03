#include "DrawingSheetInternal.h"
#include <QImage>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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
        result.push_back({{DrawingViewKind::Profile, i}, d.sections.empty()});
    std::set<std::size_t> covered, profileCovered;
    for (const auto &choice : result)
        if (choice.visible) {
            const auto v = drawing::preparedView(d, choice.key);
            covered.insert(v.dimensionedBends.begin(), v.dimensionedBends.end());
            if (choice.key.kind == DrawingViewKind::Profile)
                profileCovered.insert(v.dimensionedBends.begin(), v.dimensionedBends.end());
        }
    const auto enlargements = drawing::requiredDetails(d);
    for (std::size_t i = 0; i < d.details.size(); ++i) {
        const auto &v = d.details[i].view;
        const bool missing = v.dimensionedBends.empty()
                                 ? !covered.contains(d.details[i].bendIndex)
                                 : std::any_of(v.dimensionedBends.begin(), v.dimensionedBends.end(),
                                               [&](auto b) { return !covered.contains(b); });
        const bool needed = missing || (!profileCovered.contains(d.details[i].bendIndex) &&
                                        std::find(enlargements.begin(), enlargements.end(), i) !=
                                            enlargements.end());
        result.push_back({{DrawingViewKind::Detail, i}, needed});
        if (needed) {
            covered.insert(v.dimensionedBends.begin(), v.dimensionedBends.end());
        }
    }
    for (std::size_t i = 0; i < d.auxiliaryViews.size(); ++i)
        result.push_back({{DrawingViewKind::Auxiliary, i}});
    if (d.flatPattern)
        result.push_back({{DrawingViewKind::Flat}});
    result.push_back({{DrawingViewKind::Isometric}});
    return result;
}
namespace {
struct Item {
    std::vector<DrawingViewChoice> choices;
};
double width(const DrawingView &v) {
    return v.maximum.x - v.minimum.x - (v.omittedX ? (*v.omittedX)[1] - (*v.omittedX)[0] : 0);
}
double height(const DrawingView &v) {
    return v.maximum.y - v.minimum.y;
}
double minimumScale(const BendingDrawing &d, DrawingViewKey key, const DrawingView &v) {
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
    const double reference =
        standardDrawingScale(std::min(160. / width(v), 120. / height(v))).value_or(1.);
    return std::min(reference, readable > 0 ? readable : reference * .5);
}
std::vector<DrawingPlacedView> fitItem(const BendingDrawing &d, const Item &item, QRectF area,
                                       DrawingFlatSide side, int page) {
    std::vector<DrawingPlacedView> placed;
    std::string lastError;
    const bool family = item.choices.front().key.kind == DrawingViewKind::Projection;
    if (family) {
        std::array<std::optional<DrawingView>, 3> views;
        std::optional<double> fixed;
        for (const auto &choice : item.choices) {
            views.at(choice.key.index) = drawing::preparedView(d, choice.key);
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
            (drawing::contentArea.width() - (lateral ? 4 * margin + gap : 2 * margin) - 8) /
                std::max(rawW, 1.e-8),
            (drawing::contentArea.height() - (top ? 4 * margin + gap : 2 * margin)) /
                std::max(rawH, 1.e-8));
        const double minimum =
            fixed ? .001 : std::max(.001, standardDrawingScale(fullFit * .5).value_or(.001));
        auto scale = fixed ? fixed : standardDrawingScale(fit);
        for (int attempt = 0; attempt < 7 && scale && *scale + 1.e-9 >= minimum; ++attempt) {
            if (*scale > fit + 1.e-9)
                throw std::runtime_error("Связанные виды не помещаются в выбранном масштабе");
            // Share spare paper space too: thin long views may need many dimension lanes.
            const double sideW = lateral ? (area.width() - gap) *
                                               (width(*views[2]) * *scale + 2 * margin) /
                                               (rawW * *scale + 4 * margin)
                                         : 0;
            const double topH = top ? (area.height() - gap) *
                                          (height(*views[1]) * *scale + 2 * margin) /
                                          (rawH * *scale + 4 * margin)
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
                    cell = {main.left(), topAbove ? area.top() : main.bottom() + gap, main.width(),
                            topH};
                if (choice.key.index == 2 && lateral)
                    cell = {sideLeft ? area.left() : main.right() + gap, main.top(), sideW,
                            main.height()};
                placed.push_back({choice.key, page, cell, cell.center(), *scale});
            }
            QImage scratch(420, 297, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            try {
                for (const auto &p : placed)
                    drawing::paintPlaced(painter, d, p, side, occupied);
                return placed;
            } catch (const std::runtime_error &e) {
                lastError = e.what();
                if (fixed)
                    throw;
            }
            scale = standardDrawingScale(*scale * .8);
        }
    } else {
        const auto &choice = item.choices.front();
        const auto v = drawing::preparedView(d, choice.key);
        const double fit =
            std::min((area.width() - 70) / width(v), (area.height() - 48) / height(v));
        auto scale = choice.scale ? choice.scale
                                  : standardDrawingScale(std::min(
                                        fit, choice.key.kind == DrawingViewKind::Flat ? 1. : 100.));
        const double minimum = choice.scale ? .00001 : minimumScale(d, choice.key, v);
        for (int attempt = 0; attempt < 8 && scale && *scale + 1.e-9 >= minimum; ++attempt) {
            if (*scale > fit + 1.e-9)
                throw std::runtime_error("Вид не помещается в выбранном масштабе");
            DrawingPlacedView at{choice.key, page, area, area.center() + QPointF(0, 4), *scale};
            QImage scratch(420, 297, QImage::Format_RGB32);
            QPainter painter(&scratch);
            std::vector<QRectF> occupied;
            try {
                drawing::paintPlaced(painter, d, at, side, occupied);
                return {at};
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
std::vector<std::vector<QRectF>> cells(std::size_t count) {
    const auto area = drawing::contentArea;
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
} // namespace
std::shared_ptr<const DrawingSheetPlan>
DrawingSheet::compose(BendingDrawingPtr source, const std::vector<DrawingViewChoice> &choices,
                      DrawingFlatSide side) {
    if (!source)
        throw std::runtime_error("Нет данных чертежа");
    const auto available = defaultViews(*source);
    auto plan = std::make_shared<DrawingSheetPlan>();
    plan->source = source;
    plan->choices = choices;
    plan->flatSide = side;
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
    std::size_t begin = 0;
    while (begin < items.size()) {
        bool packed = false;
        for (auto count = std::min(std::size_t{6}, items.size() - begin); count && !packed; --count)
            for (const auto &areas : cells(count)) {
                std::vector<std::vector<std::optional<std::vector<DrawingPlacedView>>>> fits(
                    count, std::vector<std::optional<std::vector<DrawingPlacedView>>>(count));
                for (std::size_t i = 0; i < count; ++i)
                    for (std::size_t j = 0; j < count; ++j)
                        try {
                            fits[i][j] =
                                fitItem(*source, items[begin + i], areas[j], side, plan->pages);
                        } catch (const std::runtime_error &) {
                        }
                std::vector<int> owner(count, -1);
                const auto match = [&](auto &&self, std::size_t i,
                                       std::vector<bool> &visited) -> bool {
                    for (std::size_t j = 0; j < count; ++j)
                        if (!visited[j] && fits[i][j]) {
                            visited[j] = true;
                            if (owner[j] < 0 ||
                                self(self, static_cast<std::size_t>(owner[j]), visited)) {
                                owner[j] = static_cast<int>(i);
                                return true;
                            }
                        }
                    return false;
                };
                bool complete = true;
                for (std::size_t i = 0; i < count && complete; ++i) {
                    std::vector<bool> visited(count);
                    complete = match(match, i, visited);
                }
                if (!complete)
                    continue;
                for (std::size_t j = 0; j < count; ++j) {
                    const auto &at = *fits[static_cast<std::size_t>(owner[j])][j];
                    plan->views.insert(plan->views.end(), at.begin(), at.end());
                }
                begin += count;
                ++plan->pages;
                packed = true;
                break;
            }
        if (!packed) {
            std::string reason;
            try {
                (void)fitItem(*source, items[begin], drawing::contentArea, side, plan->pages);
            } catch (const std::runtime_error &e) {
                reason = e.what();
            }
            throw std::runtime_error((viewName(*source, items[begin].choices.front().key) +
                                      QStringLiteral(": размеры или выноски не помещаются на A3. "
                                                     "Выберите «Авто» или меньший масштаб. ") +
                                      QString::fromStdString(reason))
                                         .toStdString());
        }
    }
    if (iso)
        for (int page = 0; page < plan->pages; ++page) {
            const auto v = drawing::preparedView(*source, iso->key);
            const QRectF area(25, 251, 195, 38);
            const double fit = std::min(175. / width(v), 24. / height(v));
            const auto scale = iso->scale ? iso->scale : standardDrawingScale(fit);
            if (!scale || *scale > fit + 1.e-9)
                throw std::runtime_error("Изометрия не помещается в выбранном масштабе");
            plan->views.push_back({iso->key, page, area, area.center() + QPointF(0, 4), *scale});
        }
    std::set<std::size_t> covered;
    for (const auto &at : plan->views)
        if (at.key.kind != DrawingViewKind::Flat && at.key.kind != DrawingViewKind::Isometric) {
            const auto v = drawing::preparedView(*source, at.key);
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
    return plan;
}
namespace drawing {
void paintComposition(QPainter &p, const BendingDrawing &d, const DrawingTitle &title, int page) {
    const auto &plan = *title.plan;
    if (plan.source.get() != &d || page < 0 || page >= plan.pages)
        throw std::runtime_error("Устаревшая компоновка чертежа");
    paintCompositionFrame(p, d, title, plan, page);
    std::vector<QRectF> occupied;
    for (const auto &at : plan.views)
        if (at.page == page)
            paintPlaced(p, d, at, plan.flatSide, occupied);
}
} // namespace drawing
} // namespace bending::ui
