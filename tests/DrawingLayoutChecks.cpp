#include "DrawingLayoutChecks.h"
#include "DrawingLayoutController.h"
#include "DrawingPaginationChecks.h"
#include "DrawingSheetInternal.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
using namespace bending;
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
void wait(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(condition(), "Layout worker timed out");
}
class Engine final : public QPaintEngine {
  public:
    Engine() : QPaintEngine(AllFeatures) {}
    bool begin(QPaintDevice *) override {
        return true;
    }
    bool end() override {
        return true;
    }
    Type type() const override {
        return User;
    }
    void updateState(const QPaintEngineState &s) override {
        transform = s.transform();
    }
    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {
        ++rasters;
    }
    void drawImage(const QRectF &, const QImage &, const QRectF &,
                   Qt::ImageConversionFlags) override {
        ++rasters;
    }
    void drawPath(const QPainterPath &path) override {
        QPointF previous;
        for (int i = 0; i < path.elementCount(); ++i) {
            const auto e = path.elementAt(i);
            const QPointF point{e.x, e.y};
            if (e.isLineTo())
                lines.emplace_back(transform.map(previous), transform.map(point));
            previous = point;
        }
    }
    void drawPolygon(const QPointF *points, int count, PolygonDrawMode mode) override {
        for (int i = 1; i < count; ++i)
            lines.emplace_back(transform.map(points[i - 1]), transform.map(points[i]));
        if (count > 2 && mode != PolylineMode)
            lines.emplace_back(transform.map(points[count - 1]), transform.map(points[0]));
    }
    void drawTextItem(const QPointF &, const QTextItem &item) override {
        ++texts[item.text()];
    }
    void drawLines(const QLineF *value, int count) override {
        for (int i = 0; i < count; ++i)
            lines.emplace_back(transform.map(value[i].p1()), transform.map(value[i].p2()));
    }
    QTransform transform;
    std::map<QString, int> texts;
    std::vector<QLineF> lines;
    int rasters = 0;
};
class Device final : public QPaintDevice {
  public:
    QPaintEngine *paintEngine() const override {
        return &engine;
    }
    mutable Engine engine;
    explicit Device(int dpi) : dpi_(dpi) {}

  protected:
    int metric(PaintDeviceMetric m) const override {
        switch (m) {
        case PdmWidth:
            return 420;
        case PdmHeight:
            return 297;
        case PdmWidthMM:
            return 111;
        case PdmHeightMM:
            return 79;
        case PdmDepth:
            return 32;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return dpi_;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(devicePixelRatioFScale());
        default:
            return 0;
        }
    }

  private:
    int dpi_;
};
std::shared_ptr<ui::DrawingSheetPlan> result(application::BendingDrawingPtr source) {
    auto plan = std::make_shared<ui::DrawingSheetPlan>();
    plan->source = std::move(source);
    plan->pages = 1;
    return plan;
}
void limits() {
    auto drawing = std::make_shared<application::BendingDrawing>();
    auto &v = drawing->views[0];
    v.minimum = {0, 0};
    v.maximum = {100, 50};
    v.paths = {{{{0, 0}, {100, 0}, {100, 50}, {0, 50}, {0, 0}}, false}};
    v.dimensions = {{{0, 0}, {100, 0}, 100, ""}};
    const std::vector<ui::DrawingViewChoice> choices{{{ui::DrawingViewKind::Projection, 0}}};
    const auto rejected = [&](std::stop_token stop, ui::DrawingLayoutLimits bounds,
                              ui::DrawingLayoutInterruption expected) {
        bool found = false;
        try {
            (void)ui::DrawingSheet::compose(drawing, choices, ui::DrawingFlatSide::Automatic, stop,
                                            bounds);
        } catch (const ui::DrawingLayoutInterrupted &e) {
            found = e.reason == expected;
        }
        require(found, "Layout interruption was swallowed or converted to a fit failure");
    };
    std::stop_source cancelled;
    cancelled.request_stop();
    rejected(cancelled.get_token(), {}, ui::DrawingLayoutInterruption::Cancelled);
    ui::DrawingLayoutLimits bounds;
    bounds.maximumDimensionProbes = 0;
    rejected({}, bounds, ui::DrawingLayoutInterruption::LimitExceeded);
    bounds = {};
    bounds.maximumTime = std::chrono::milliseconds{0};
    rejected({}, bounds, ui::DrawingLayoutInterruption::LimitExceeded);
    bounds = {};
    bounds.maximumRecordingBytes = 0;
    rejected({}, bounds, ui::DrawingLayoutInterruption::LimitExceeded);
}
void unevenProjectionDimensions() {
    auto d = std::make_shared<application::BendingDrawing>();
    const auto rectangle = [](application::DrawingView &v, double width, double height) {
        v.minimum = {0, 0};
        v.maximum = {width, height};
        v.paths = {{{{0, 0}, {width, 0}, {width, height}, {0, height}, {0, 0}}, false}};
        v.dimensions = {{{0, 0}, {width, 0}, width, ""}, {{0, 0}, {0, height}, height, ""}};
    };
    rectangle(d->views[0], 1230, 350);
    rectangle(d->views[1], 1230, 170);
    rectangle(d->views[2], 170, 350);
    auto &front = d->views[0];
    front.holeDimensions = {{{0, 18}, {750, 18}, 750, ""},
                            {{0, 175}, {850, 175}, 850, ""},
                            {{750, 0}, {750, 18}, 18, ""},
                            {{850, 0}, {850, 175}, 175, ""},
                            {{750, 0}, {750, 332}, 332, ""}};
    front.axialDimensions = {{{0, 0}, {1000, 0}, 1000, ""},
                             {{1000, 0}, {1230, 0}, 230, ""},
                             {{0, 0}, {0, 45}, 45, ""},
                             {{0, 45}, {0, 305}, 260, ""},
                             {{0, 305}, {0, 350}, 45, ""}};
    auto &top = d->views[1];
    for (int i = 0; i < 11; ++i) {
        const double x = 120 + 95 * i, y = i % 3 ? 40 : 100;
        top.holeDimensions.push_back({{0, y}, {x, y}, x, ""});
        top.circleCenters.push_back({{x, y}, 3});
    }
    for (int i = 0; i < 8; ++i) {
        const double x = 250 + 100 * i, y = 12 + 14 * i;
        top.holeDimensions.push_back({{x, 0}, {x, y}, y, ""});
        top.circleCenters.push_back({{x, y}, 3});
    }
    std::vector<ui::DrawingViewChoice> choices;
    for (std::size_t i = 0; i < 3; ++i)
        choices.push_back({{ui::DrawingViewKind::Projection, i}});
    const auto check = [&](const auto &plan) {
        require(plan->pages == 1 && plan->views.size() == 3,
                "Uneven dimension density split a projection family");
        std::size_t count = 0;
        for (const auto &v : plan->views) {
            require(v.scale >= .1 && v.scale == plan->views.front().scale,
                    "Dense dimensions reduced or unlinked the projection scale");
            count += v.dimensions.size();
        }
        require(count == 35, "Dense projection family lost measurements");
        require(std::abs(plan->views[0].centre.x() - plan->views[1].centre.x()) < 1e-8 &&
                    std::abs(plan->views[0].centre.y() - plan->views[2].centre.y()) < 1e-8,
                "Annotation insets unlinked the projection positions");
        verifyRecordedDimensions(*d, *plan); // Actual vector output at both PDF device DPIs.
    };
    check(ui::DrawingSheet::compose(d, choices));
    for (auto &choice : choices)
        choice.scale = .1;
    check(ui::DrawingSheet::compose(d, choices));
    for (auto &choice : choices)
        choice.scale = .2;
    std::shared_ptr<const ui::DrawingSheetPlan> enlarged;
    try {
        enlarged = ui::DrawingSheet::compose(d, choices);
    } catch (const std::runtime_error &e) {
        const auto error = QString::fromUtf8(e.what());
        require(error.contains(QStringLiteral("Связанные виды")),
                "Wrong oversized-view diagnostic");
    }
    if (enlarged) {
        require(enlarged->views.size() == 3,
                "Oversized group dropped a projection while splitting");
        for (const auto &at : enlarged->views)
            require(at.scale == .2, "Oversized group silently reduced its explicit scale");
        verifyRecordedDimensions(*d, *enlarged);
    }
}
void sameLayout(const ui::DrawingSheetPlan &a, const ui::DrawingSheetPlan &b) {
    require(a.source == b.source && a.pages == b.pages && a.views.size() == b.views.size() &&
                a.recordingBytes == b.recordingBytes && a.coverage.total == b.coverage.total &&
                a.coverage.dimensioned == b.coverage.dimensioned &&
                a.coverage.missing == b.coverage.missing,
            "Fit reuse changed the drawing sheets, coverage or vector output size");
    for (std::size_t i = 0; i < a.views.size(); ++i) {
        const auto &x = a.views[i];
        const auto &y = b.views[i];
        require(x.key == y.key && x.page == y.page && x.area == y.area && x.centre == y.centre &&
                    x.scale == y.scale && x.selectionBounds == y.selectionBounds &&
                    x.dimensions.size() == y.dimensions.size() &&
                    x.compactDimensions == y.compactDimensions && x.sheetLinks == y.sheetLinks,
                "Fit reuse changed a view or reused another item's placement");
        for (std::size_t j = 0; j < x.dimensions.size(); ++j) {
            const auto &m = x.dimensions[j];
            const auto &n = y.dimensions[j];
            require(m.source == n.source && m.index == n.index && m.text == n.text &&
                        m.textBounds == n.textBounds && m.lines == n.lines &&
                        m.measurement.valueMm == n.measurement.valueMm,
                    "Fit reuse changed a recorded dimension or its witnesses");
        }
    }
    ui::DrawingTitle first, second;
    first.plan = std::make_shared<const ui::DrawingSheetPlan>(a);
    second.plan = std::make_shared<const ui::DrawingSheetPlan>(b);
    for (int page = 0; page < a.pages; ++page)
        require(ui::DrawingSheet::preview(*a.source, first, page) ==
                    ui::DrawingSheet::preview(*b.source, second, page),
                "Fit reuse changed the rendered drawing");
    for (const int dpi : {96, 254}) {
        Device x(dpi), y(dpi);
        QPainter firstPainter(&x), secondPainter(&y);
        for (int page = 0; page < a.pages; ++page) {
            ui::DrawingSheet::paint(firstPainter, *a.source, first, page);
            ui::DrawingSheet::paint(secondPainter, *b.source, second, page);
        }
        firstPainter.end();
        secondPainter.end();
        require(x.engine.lines == y.engine.lines && x.engine.texts == y.engine.texts &&
                    x.engine.rasters == 0 && y.engine.rasters == 0,
                "Fit reuse changed vector replay at the output device DPI");
    }
}
void fitCache() {
    auto d = std::make_shared<application::BendingDrawing>();
    auto &v = d->views[0];
    v.minimum = {0, 0};
    v.maximum = {1800, 500};
    v.paths = {{{{0, 0}, {1800, 0}, {1800, 500}, {0, 500}, {0, 0}}, false}};
    v.dimensions = {{{0, 0}, {1800, 0}, 1800, ""}, {{0, 0}, {0, 500}, 500, ""}};
    std::vector<ui::DrawingViewChoice> choices{{{ui::DrawingViewKind::Projection, 0}, true, .1}};
    for (std::size_t i = 0; i < 5; ++i) {
        application::DrawingSection section;
        section.view = v;
        section.designation = std::string(1, static_cast<char>('A' + i));
        d->sections.push_back(std::move(section));
        // Leave room for competing cell assignments while still needing multiple sheets.
        choices.push_back({{ui::DrawingViewKind::Section, i}, true, i % 2 ? .1 : .05});
    }
    ui::DrawingLayoutLimits disabled;
    disabled.maximumCachedFits = 0;
    const auto reference = ui::DrawingSheet::compose(d, choices, {}, {}, disabled);
    const auto cached = ui::DrawingSheet::compose(d, choices);
    require(cached->pages > 1 && cached->fitCacheHits > 0 && cached->fitCacheEntries > 1 &&
                cached->fitCacheEntries <= 4096 && reference->fitCacheHits == 0 &&
                reference->fitCacheEntries == 0 &&
                cached->fitCalls + cached->fitCacheHits == reference->fitCalls &&
                cached->dimensionProbes <= reference->dimensionProbes,
            "Fit cache did not reduce actual work or reported work from cached attempts");
    sameLayout(*reference, *cached);
    ui::DrawingLayoutLimits bounded;
    bounded.maximumCachedFits = 1;
    const auto spilled = ui::DrawingSheet::compose(d, choices, {}, {}, bounded);
    require(spilled->fitCacheEntries == 1 && spilled->fitCalls > cached->fitCalls,
            "Full fit cache exceeded its bound or stopped normal search");
    sameLayout(*reference, *spilled);

    // A new input/selection/scale owns a new cache even if all item indices coincide.
    auto changed = std::make_shared<application::BendingDrawing>(*d);
    changed->sections[0].view.dimensions.push_back({{0, 250}, {700, 250}, 700, ""});
    choices[1].scale = .04;
    choices.back().visible = false;
    const auto updated = ui::DrawingSheet::compose(changed, choices);
    sameLayout(*ui::DrawingSheet::compose(changed, choices, {}, {}, disabled), *updated);

    for (auto bounds : {ui::DrawingLayoutLimits{}, disabled}) {
        bounds.maximumDimensionProbes = std::max(std::size_t{1}, updated->dimensionProbes / 2);
        bool limited = false;
        try {
            (void)ui::DrawingSheet::compose(changed, choices, {}, {}, bounds);
        } catch (const ui::DrawingLayoutInterrupted &e) {
            limited = e.reason == ui::DrawingLayoutInterruption::LimitExceeded;
        }
        require(limited, "Fit reuse turned an interrupted layout into a partial success");
    }
    choices.front().scale = .2;
    const auto failure = [&](ui::DrawingLayoutLimits bounds) {
        try {
            (void)ui::DrawingSheet::compose(changed, choices, {}, {}, bounds);
        } catch (const std::runtime_error &e) {
            return std::string(e.what());
        }
        return std::string{};
    };
    const auto error = failure({});
    require(!error.empty() && error == failure(disabled),
            "Fit reuse hid an impossible explicit scale or changed its diagnostic");
}
void references() {
    using namespace application;
    using namespace ui;
    auto d = std::make_shared<BendingDrawing>();
    for (auto &v : d->views) {
        v.minimum = {0, 0};
        v.maximum = {120, 80};
        v.paths = {{{{0, 0}, {120, 0}, {120, 80}, {0, 80}, {0, 0}}, false}};
    }
    d->bends.resize(2);
    for (std::size_t i = 0; i < 2; ++i) {
        DrawingSection section;
        section.designation = i ? "Б" : "А";
        section.view = d->views[0];
        d->sections.push_back(section);
        d->details.push_back({d->views[0], i, i ? "А2" : "А1"});
    }
    DrawingProfile profile;
    profile.designation = "П1";
    profile.view = d->views[0];
    d->profiles.push_back(profile);
    d->auxiliaryViews.push_back({d->views[0], "Ц", 1});
    auto &source = d->views[0];
    source.sectionMarkers = {{{40, 0}, {40, 80}, {-1, 0}, "А"},
                             {{80, 0}, {80, 80}, {-1, 0}, "Б"},
                             {{90, 0}, {90, 80}, {-1, 0}, "Б"}};
    source.labels = {{{60, 10}, "П1"}, {{60, 70}, "Примечание"}};
    source.detailLocators = {{{20, 40}, 8, "А1"}, {{20, 40}, 16, "А2"}};
    source.circleCenters = {{{30, 30}, 3}};
    source.viewArrows = {{{120, 40}, {-1, 0}, "Ц"}};
    std::vector<DrawingViewChoice> choices{
        {{DrawingViewKind::Projection, 0}},      {{DrawingViewKind::Section, 0}},
        {{DrawingViewKind::Section, 1}, false},  {{DrawingViewKind::Detail, 0}},
        {{DrawingViewKind::Detail, 1}, false},   {{DrawingViewKind::Profile, 0}, false},
        {{DrawingViewKind::Auxiliary, 0}, false}};
    const auto key = DrawingViewKey{DrawingViewKind::Projection, 0};
    const auto selected = drawing::selectedView(*d, key, &choices);
    require(selected.sectionMarkers.size() == 1 && selected.sectionMarkers[0].designation == "А" &&
                selected.labels.size() == 1 && selected.labels[0].second == "Примечание" &&
                selected.detailLocators.size() == 1 &&
                selected.detailLocators[0].designation == "А1" &&
                selected.detailLocators[0].radius == 8 && selected.viewArrows.empty() &&
                selected.circleCenters.size() == 1 && selected.paths.size() == source.paths.size(),
            "Disabled destination retained a marker or erased geometry/unrelated text");
    auto reversed = choices;
    for (auto &choice : reversed)
        choice.visible = !choice.visible;
    const auto alternate = drawing::selectedView(*d, key, &reversed);
    require(alternate.sectionMarkers.size() == 2 && alternate.labels.size() == 2 &&
                alternate.labels[0].second == "П1" && alternate.detailLocators.size() == 1 &&
                alternate.detailLocators[0].designation == "А2" &&
                alternate.detailLocators[0].radius == 16 && alternate.viewArrows.size() == 1,
            "Related markers did not follow their individual destinations");
    const std::vector<DrawingViewChoice> onlySource{{key}};
    const auto none = drawing::selectedView(*d, key, &onlySource);
    require(none.sectionMarkers.empty() && none.detailLocators.empty() && none.viewArrows.empty() &&
                none.labels.size() == 1 && none.labels[0].second == "Примечание",
            "Omitted destinations differ from unchecked destinations");
    // Classical preparation can restore locators from the original projection.
    // Selection is applied afterwards, so restoration cannot resurrect a hidden one.
    d->classicalViews = std::array{d->views[0], d->views[1], d->views[2]};
    (*d->classicalViews)[0].labels.clear();
    (*d->classicalViews)[0].detailLocators.clear();
    const auto classical = drawing::selectedView(*d, key, &choices);
    require(classical.labels.empty() && classical.detailLocators.size() == 1 &&
                classical.detailLocators[0].designation == "А1" &&
                classical.detailLocators[0].radius == 8 && classical.sectionMarkers.size() == 1,
            "Classical locator restoration borrowed a circle from the coincident detail");
    const auto classicalAlternate = drawing::selectedView(*d, key, &reversed);
    require(classicalAlternate.detailLocators.size() == 1 &&
                classicalAlternate.detailLocators[0].designation == "А2" &&
                classicalAlternate.detailLocators[0].radius == 16,
            "Classical locator restoration lost the second coincident detail");
    auto all = DrawingSheet::defaultViews(*d);
    const auto defaultVisible = [](const auto &choice) {
        return choice.key.kind == DrawingViewKind::Projection ||
               choice.key.kind == DrawingViewKind::Section ||
               choice.key.kind == DrawingViewKind::Isometric;
    };
    require(all.size() == 10 && std::all_of(all.begin(), all.end(),
                                            [&](const auto &choice) {
                                                return choice.visible == defaultVisible(choice);
                                            }),
            "Default choices omitted a destination or enabled an optional view");
    const auto standard = drawing::selectedView(*d, key, &all);
    require(standard.sectionMarkers.size() == 3 && standard.detailLocators.empty() &&
                standard.viewArrows.empty(),
            "Default choices retained references to optional views");
    for (auto &choice : all)
        choice.visible = true;
    const auto complete = drawing::selectedView(*d, key, &all);
    const auto original = drawing::preparedView(*d, key);
    require(complete.sectionMarkers.size() == original.sectionMarkers.size() &&
                complete.detailLocators.size() == 2 && original.detailLocators.size() == 2 &&
                complete.labels.size() == original.labels.size() &&
                complete.viewArrows.size() == original.viewArrows.size(),
            "Visible references changed the original annotation set");
    require(source.sectionMarkers.size() == 3 && source.labels.size() == 2 &&
                source.detailLocators.size() == 2 && source.viewArrows.size() == 1,
            "Reference filtering mutated the immutable CAD snapshot");

    auto pair = choices;
    pair[1].visible = false;
    pair[4].visible = true;
    DrawingTitle pairedTitle;
    pairedTitle.plan = DrawingSheet::compose(d, pair);
    const auto parentView =
        std::find_if(pairedTitle.plan->views.begin(), pairedTitle.plan->views.end(),
                     [&](const auto &at) { return at.key == key; });
    require(parentView != pairedTitle.plan->views.end(), "Missing detail parent projection");
    Device pairedDevice(96);
    {
        QPainter painter(&pairedDevice);
        DrawingSheet::paint(painter, *d, pairedTitle, parentView->page);
    }
    const auto centre = parentView->centre + QPointF(-40 * parentView->scale, 0);
    for (const auto &locator : source.detailLocators) {
        const double radius = std::max(2.5, locator.radius * parentView->scale);
        require(pairedDevice.engine.texts[QString::fromStdString(locator.designation)] == 1 &&
                    std::any_of(pairedDevice.engine.lines.begin(), pairedDevice.engine.lines.end(),
                                [&](const auto &line) {
                                    return std::abs(QLineF(centre, line.p1()).length() - radius) <
                                               1e-4 &&
                                           QLineF(centre, line.p2()).length() > radius + 1 &&
                                           std::abs(line.dx()) > 1e-3 && std::abs(line.dy()) > 1e-3;
                                }),
                "Concentric detail leader did not connect its own circle to a slanted line");
    }

    // The same policy must reach fit probes, recordings, preview and PDF. Keep
    // one true section marker; all local detail locators are disabled here.
    choices[3].visible = false;
    const auto enabled = DrawingSheet::compose(d, choices);
    DrawingTitle title;
    title.plan = enabled;
    Device with(96);
    {
        QPainter painter(&with);
        for (int page = 0; page < enabled->pages; ++page)
            DrawingSheet::paint(painter, *d, title, page);
    }
    require(with.engine.texts[QStringLiteral("А")] == 2 &&
                with.engine.texts[QStringLiteral("Б")] == 0 &&
                with.engine.texts[QStringLiteral("А1")] == 0 &&
                with.engine.texts[QStringLiteral("А2")] == 0,
            "Vector recording retained disabled callouts");
    choices[1].visible = false;
    const auto disabled = DrawingSheet::compose(d, choices);
    title.plan = disabled;
    Device without(96);
    {
        QPainter painter(&without);
        for (int page = 0; page < disabled->pages; ++page)
            DrawingSheet::paint(painter, *d, title, page);
    }
    require(without.engine.texts[QStringLiteral("А")] == 0 &&
                without.engine.texts[QStringLiteral("Б")] == 0,
            "Hidden section left arrows/letters in the recorded parent");
    DrawingLayoutLimits reference;
    reference.maximumCachedFits = 0;
    sameLayout(*disabled, *DrawingSheet::compose(d, choices, {}, {}, reference));
    choices[1].visible = true;
    sameLayout(*enabled, *DrawingSheet::compose(d, choices));
    auto paginated = std::make_shared<BendingDrawing>(*d);
    auto &large = paginated->sections[0].view;
    large.maximum = {300, 150};
    large.paths = {{{{0, 0}, {300, 0}, {300, 150}, {0, 150}, {0, 0}}, false}};
    auto fixed = choices;
    for (auto &choice : fixed)
        choice.scale = 1;
    const auto multiPage = DrawingSheet::compose(paginated, fixed);
    require(multiPage->pages >= 2, "Reference fixture did not separate source and target sheets");
    const auto parent = std::find_if(multiPage->views.begin(), multiPage->views.end(),
                                     [&](const auto &at) { return at.key == key; });
    require(parent != multiPage->views.end(), "Cross-sheet source missing");
    title.plan = multiPage;
    Device crossSheet(96);
    {
        QPainter painter(&crossSheet);
        DrawingSheet::paint(painter, *paginated, title, parent->page);
    }
    require(crossSheet.engine.texts[QStringLiteral("А")] == 2,
            "A section on another sheet lost its parent marker");
    if (const auto prefix = qEnvironmentVariable("BENDING_DRAWING_REFERENCES_OUTPUT");
        !prefix.isEmpty()) {
        title.plan = enabled;
        require(DrawingSheet::writePdf(prefix + "-enabled.pdf", *d, title).isEmpty(),
                "Cannot write selected-reference PDF");
        title.plan = disabled;
        require(DrawingSheet::writePdf(prefix + "-disabled.pdf", *d, title).isEmpty(),
                "Cannot write hidden-reference PDF");
        require(DrawingSheet::preview(*d, title, 0).save(prefix + "-preview.png"),
                "Cannot write hidden-reference preview");
    }
}
void worker() {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool released = false;
        void release() {
            std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }
    } gate;
    std::atomic<int> calls = 0;
    std::atomic<bool> entered = false, outsideGui = false;
    const auto gui = QThread::currentThread();
    ui::DrawingLayoutController controller(
        nullptr,
        [&](auto source, const auto &, auto, std::stop_token,
            auto, auto) -> std::shared_ptr<const ui::DrawingSheetPlan> {
            outsideGui = QThread::currentThread() != gui;
            if (++calls == 1) {
                entered = true;
                std::unique_lock lock(gate.mutex);
                gate.changed.wait(lock, [&] { return gate.released; });
                throw std::runtime_error("Obsolete failure must be discarded");
            }
            return result(std::move(source));
        });
    struct Release {
        Gate &gate;
        ~Release() {
            gate.release();
        }
    } release{gate}; // Unblocks the worker even if an assertion throws.
    application::BendingDrawingPtr accepted;
    int failures = 0, ticks = 0;
    QObject::connect(&controller, &ui::DrawingLayoutController::ready, &controller,
                     [&](auto plan) { accepted = plan->source; });
    QObject::connect(&controller, &ui::DrawingLayoutController::failed, &controller,
                     [&](const auto &) { ++failures; });
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &controller, [&] { ++ticks; });
    heartbeat.start();
    auto first = std::make_shared<application::BendingDrawing>();
    auto superseded = std::make_shared<application::BendingDrawing>();
    auto last = std::make_shared<application::BendingDrawing>();
    controller.request(first, {}, ui::DrawingFlatSide::Automatic);
    wait([&] { return entered.load() && ticks >= 3; });
    controller.request(superseded, {}, ui::DrawingFlatSide::Automatic);
    controller.request(last, {}, ui::DrawingFlatSide::Opposite);
    gate.release();
    wait([&] { return !controller.busy(); });
    require(outsideGui && accepted == last && calls == 2 && failures == 0,
            "Layout blocked GUI, ran a superseded request or published an obsolete failure");

    entered = false;
    auto stopping = std::make_unique<ui::DrawingLayoutController>(
        nullptr, [&](auto source, const auto &, auto, std::stop_token stop, auto, auto) {
            entered = true;
            std::mutex mutex;
            std::unique_lock lock(mutex);
            std::condition_variable_any ready;
            ready.wait(lock, stop, [] { return false; });
            return result(std::move(source));
        });
    int published = 0;
    QObject::connect(stopping.get(), &ui::DrawingLayoutController::ready, &controller,
                     [&](const auto &) { ++published; });
    stopping->request(first, {}, ui::DrawingFlatSide::Automatic);
    wait([&] { return entered.load(); });
    stopping->cancel();
    wait([&] { return !stopping->busy(); });
    require(published == 0, "Cancelled layout published a plan");
    entered = false;
    stopping->request(first, {}, ui::DrawingFlatSide::Automatic);
    wait([&] { return entered.load(); });
    stopping.reset(); // Destruction requests cancellation and joins the worker.
    QApplication::processEvents();
    require(published == 0, "Destroyed layout controller delivered a late result");
    QString error;
    ui::DrawingLayoutController failing(
        nullptr,
        [](auto, const auto &, auto, auto, auto, auto) -> std::shared_ptr<const ui::DrawingSheetPlan> {
            throw ui::DrawingLayoutInterrupted(ui::DrawingLayoutInterruption::LimitExceeded,
                                               "Test budget exhausted");
        });
    QObject::connect(&failing, &ui::DrawingLayoutController::failed, &controller,
                     [&](const QString &value) { error = value; });
    failing.request(first, {}, ui::DrawingFlatSide::Automatic);
    wait([&] { return !failing.busy(); });
    require(error == "Test budget exhausted", "Current layout failure was not delivered");
}
} // namespace

void verifyRecordedDimensions(const bending::application::BendingDrawing &drawing,
                              const bending::ui::DrawingSheetPlan &plan) {
    using namespace bending;
    require(plan.recordingBytes > 0 && plan.dimensionProbes > 0,
            "Plan has no prepared vector views");
    for (const int dpi : {96, 254}) {
        Device device(dpi);
        QPainter painter(&device);
        ui::DrawingTitle title;
        title.plan = std::make_shared<const ui::DrawingSheetPlan>(plan);
        for (int page = 0; page < plan.pages; ++page)
            ui::DrawingSheet::paint(painter, drawing, title, page);
        painter.end();
        std::map<QString, int> requiredText;
        for (const auto &at : plan.views) {
            require(bool(at.recording), "Placed view has no immutable drawing");
            const auto &v =
                drawing.views.at(at.key.index); // This fixture contains only projections.
            const std::array<const std::vector<application::DrawingDimension> *, 4> lists{
                &v.axialDimensions, &v.holeDimensions, &v.sectionDimensions, &v.dimensions};
            std::size_t expected = 0;
            for (std::size_t kind = 0; kind < lists.size(); ++kind)
                for (std::size_t index = 0; index < lists[kind]->size(); ++index) {
                    const auto &measurement = lists[kind]->at(index);
                    if (std::hypot(measurement.end.x - measurement.start.x,
                                   measurement.end.y - measurement.start.y) *
                            at.scale <
                        1e-7)
                        continue;
                    ++expected;
                    const auto found = std::find_if(
                        at.dimensions.begin(), at.dimensions.end(), [&](const auto &p) {
                            return static_cast<std::size_t>(p.source) == kind && p.index == index;
                        });
                    require(found != at.dimensions.end() &&
                                found->measurement.valueMm == measurement.valueMm,
                            "An input dimension is missing or changed in the output plan");
                    bool numeric = false;
                    const auto value = QString(found->text).replace(',', '.').toDouble(&numeric);
                    require(numeric && std::abs(value - measurement.valueMm) < .005,
                            "Printed dimension text differs from its measured value");
                    require(at.area.contains(found->textBounds), "Dimension text is clipped");
                    ++requiredText[found->text];
                    for (const auto &line : found->lines) {
                        const auto samePoint = [](auto a, auto b) {
                            return QLineF(a, b).length() < 1e-5;
                        };
                        require(std::any_of(device.engine.lines.begin(), device.engine.lines.end(),
                                            [&](const auto &drawn) {
                                                return (samePoint(line.p1(), drawn.p1()) &&
                                                        samePoint(line.p2(), drawn.p2())) ||
                                                       (samePoint(line.p1(), drawn.p2()) &&
                                                        samePoint(line.p2(), drawn.p1()));
                                            }),
                                "Prepared dimension line did not reach the output painter");
                    }
                }
            require(at.dimensions.size() == expected,
                    "Output plan duplicated or dropped dimensions");
        }
        for (const auto &[text, count] : requiredText)
            require(device.engine.texts[text] >= count,
                    "A prepared dimension was omitted during replay");
        require(device.engine.rasters == 0, "Vector drawing was rasterized during replay");
    }
}

void verifySameDrawingLayout(const bending::ui::DrawingSheetPlan &a,
                             const bending::ui::DrawingSheetPlan &b) {
    sameLayout(a, b);
}
void drawingReferenceChecks() {
    references();
}
void drawingLayoutChecks() {
    references();
    limits();
    unevenProjectionDimensions();
    drawingPaginationChecks();
    fitCache();
    worker();
}
