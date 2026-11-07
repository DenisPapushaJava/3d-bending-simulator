#include "DrawingPaginationChecks.h"
#include "DrawingLayoutChecks.h"
#include <QLineF>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace {
using namespace bending;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void rectangle(application::DrawingView &v, double w, double h) {
    v.minimum = {0, 0};
    v.maximum = {w, h};
    v.paths = {{{{0, 0}, {w, 0}, {w, h}, {0, h}, {0, 0}}, false}};
    v.dimensions = {{{0, 0}, {w, 0}, w, ""}, {{0, 0}, {0, h}, h, ""}};
}
std::shared_ptr<application::BendingDrawing> disjointDimensions() {
    auto d = std::make_shared<application::BendingDrawing>();
    rectangle(d->views[0], 1000, 80);
    rectangle(d->views[1], 1000, 80);
    rectangle(d->views[2], 80, 80);
    for (std::size_t view = 0; view < 2; ++view)
        for (int i = 0; i < 12; ++i) {
            const double x = 10 + 80 * i;
            d->views[view].holeDimensions.push_back({{x, 40}, {x + 40, 40}, 40, ""});
        }
    return d;
}
std::shared_ptr<application::BendingDrawing> nestedDimensions() {
    auto d = std::make_shared<application::BendingDrawing>();
    rectangle(d->views[0], 1900, 300);
    rectangle(d->views[1], 1900, 150);
    rectangle(d->views[2], 150, 300);
    for (int i = 0; i < 8; ++i) {
        const double x = 120 + 200 * i;
        d->views[0].holeDimensions.push_back({{0, 100}, {x, 100}, x, ""});
    }
    for (int i = 0; i < 3; ++i) {
        const double y = 30 + 120 * i, x = 500 + 100 * i;
        d->views[0].holeDimensions.push_back({{x, 0}, {x, y}, y, ""});
    }
    for (int i = 0; i < 15; ++i) {
        const double x = 100 + 115 * i, y = i % 2 ? 70 : 100;
        d->views[1].holeDimensions.push_back({{0, y}, {x, y}, x, ""});
    }
    for (int i = 0; i < 5; ++i) {
        const double y = 20 + 20 * i, x = 140 + 200 * i;
        d->views[1].holeDimensions.push_back({{x, 0}, {x, y}, y, ""});
    }
    return d;
}
std::vector<ui::DrawingViewChoice> projections() {
    return {{{ui::DrawingViewKind::Projection, 0}},
            {{ui::DrawingViewKind::Projection, 1}},
            {{ui::DrawingViewKind::Projection, 2}}};
}
void checkProjections(const application::BendingDrawing &drawing, const ui::DrawingSheetPlan &plan,
                      const std::vector<ui::DrawingViewChoice> &choices) {
    require(plan.choices.size() == choices.size(), "Resolved layout changed user choices");
    std::optional<double> scale;
    std::set<std::size_t> found;
    for (const auto &at : plan.views) {
        require(at.key.kind == ui::DrawingViewKind::Projection && found.insert(at.key.index).second,
                "Pagination lost a view or emitted it twice");
        require(!scale || *scale == at.scale, "Paginated projections have different scales");
        scale = at.scale;
        const auto choice = std::find_if(choices.begin(), choices.end(),
                                         [&](const auto &c) { return c.key == at.key; });
        require(choice != choices.end() && choice->visible &&
                    (!choice->scale || *choice->scale == at.scale),
                "Pagination changed explicit scale or showed a hidden view");
    }
    require(found.size() ==
                static_cast<std::size_t>(std::count_if(choices.begin(), choices.end(),
                                                       [](const auto &c) { return c.visible; })),
            "Selected projections are missing");
    verifyRecordedDimensions(drawing, plan);
}
void checkRowSharing(const ui::DrawingSheetPlan &plan) {
    bool shared = false;
    for (const auto &at : plan.views) {
        if (!at.compactDimensions)
            continue;
        for (std::size_t i = 0; i < at.dimensions.size(); ++i)
            for (std::size_t j = i + 1; j < at.dimensions.size(); ++j) {
                const auto &a = at.dimensions[i].lines[0], &b = at.dimensions[j].lines[0];
                if (std::abs(a.dy()) > 1.e-7 || std::abs(b.dy()) > 1.e-7 ||
                    std::abs(a.p1().y() - b.p1().y()) > 1.e-7)
                    continue;
                shared = true;
                require(std::max(std::min(a.x1(), a.x2()), std::min(b.x1(), b.x2())) >
                            std::min(std::max(a.x1(), a.x2()), std::max(b.x1(), b.x2())) + .9,
                        "Unrelated dimensions share overlapping line intervals");
            }
    }
    require(shared, "Compact layout did not reuse a free dimension row");
}
void interrupted(const std::shared_ptr<application::BendingDrawing> &drawing,
                 const std::vector<ui::DrawingViewChoice> &choices,
                 const ui::DrawingSheetPlan &complete) {
    for (auto probes :
         {std::size_t{0}, complete.dimensionProbes / 2, complete.dimensionProbes - 1}) {
        ui::DrawingLayoutLimits limits;
        limits.maximumDimensionProbes = probes;
        bool limited = false;
        try {
            (void)ui::DrawingSheet::compose(drawing, choices, {}, {}, limits);
        } catch (const ui::DrawingLayoutInterrupted &error) {
            limited = error.reason == ui::DrawingLayoutInterruption::LimitExceeded;
        }
        require(limited, "Compaction/pagination turned a spent budget into success or NoFit");
    }
}
} // namespace

void drawingPaginationChecks() {
    auto choices = projections();
    const auto compactDrawing = disjointDimensions();
    const auto compact = ui::DrawingSheet::compose(compactDrawing, choices);
    require(compact->pages == 1 && std::any_of(compact->views.begin(), compact->views.end(),
                                               [](const auto &v) { return v.compactDimensions; }),
            "Disjoint dimensions were split instead of compacted");
    checkProjections(*compactDrawing, *compact, choices);
    checkRowSharing(*compact);
    interrupted(compactDrawing, choices, *compact);

    const auto drawing = nestedDimensions();
    const auto paginated = ui::DrawingSheet::compose(drawing, choices);
    require(paginated->pages > 1, "Nested coordinates were collapsed into overlapping rows");
    checkProjections(*drawing, *paginated, choices);
    const auto main = std::find_if(paginated->views.begin(), paginated->views.end(),
                                   [](const auto &v) { return v.key.index == 0; });
    require(main != paginated->views.end() && !main->sheetLinks.isEmpty(),
            "Main view lost its cross-sheet reference");
    for (const auto &at : paginated->views)
        if (at.page != main->page)
            require(at.sheetLinks.contains(QStringLiteral("Главный вид")) &&
                        at.sheetLinks.contains(QString::number(main->page + 1)),
                    "Separate projection has no correct reference to its main view");
    interrupted(drawing, choices, *paginated);

    ui::DrawingLayoutLimits reference;
    reference.maximumCachedFits = 0;
    verifySameDrawingLayout(*compact,
                            *ui::DrawingSheet::compose(compactDrawing, choices, {}, {}, reference));
    verifySameDrawingLayout(*paginated,
                            *ui::DrawingSheet::compose(drawing, choices, {}, {}, reference));
    reference.maximumCachedFits = 1;
    const auto bounded = ui::DrawingSheet::compose(drawing, choices, {}, {}, reference);
    require(bounded->fitCacheEntries == 1, "Fallback cache exceeded its single-entry bound");
    verifySameDrawingLayout(*paginated, *bounded);

    for (auto &choice : choices)
        choice.scale = .1;
    const auto fixed = ui::DrawingSheet::compose(drawing, choices);
    checkProjections(*drawing, *fixed, choices);
    for (auto &choice : choices)
        choice.scale = 1.;
    bool impossible = false;
    try {
        (void)ui::DrawingSheet::compose(drawing, choices);
    } catch (const std::runtime_error &) {
        impossible = true;
    }
    require(impossible, "An individually oversized view was shrunk or dropped");
    choices = projections();
    choices[0].scale = .1;
    choices[1].scale = .05;
    impossible = false;
    try {
        (void)ui::DrawingSheet::compose(drawing, choices);
    } catch (const std::runtime_error &) {
        impossible = true;
    }
    require(impossible, "Incompatible explicit scales were silently reconciled");

    choices = projections();
    choices[2].visible = false;
    checkProjections(*drawing, *ui::DrawingSheet::compose(drawing, choices), choices);
    choices = projections();
    auto reversed = std::make_shared<application::BendingDrawing>(*drawing);
    reversed->views[1].reverseProjection = reversed->views[2].reverseProjection = true;
    const auto reversePlan = ui::DrawingSheet::compose(reversed, choices);
    checkProjections(*reversed, *reversePlan, choices);
    const auto reverseMain = std::find_if(reversePlan->views.begin(), reversePlan->views.end(),
                                          [](const auto &v) { return v.key.index == 0; });
    require(reverseMain != reversePlan->views.end() &&
                !reverseMain->sheetLinks.contains(QStringLiteral("Вид сверху")) &&
                !reverseMain->sheetLinks.contains(QStringLiteral("Вид слева")),
            "Cross-sheet references ignored the actual reverse projection");
    for (bool mode : {false, true}) {
        auto stale = std::make_shared<ui::DrawingSheetPlan>(*paginated);
        if (mode)
            stale->views.front().sheetLinks += QLatin1Char('!');
        else
            stale->views.front().compactDimensions = !stale->views.front().compactDimensions;
        ui::DrawingTitle title;
        title.plan = stale;
        bool rejected = false;
        try {
            (void)ui::DrawingSheet::preview(*drawing, title);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "A vector recording accepted changed layout policy or sheet references");
    }
}
