#include "DrawingLayoutChecks.h"
#include "DrawingLayoutController.h"
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
            auto) -> std::shared_ptr<const ui::DrawingSheetPlan> {
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
        nullptr, [&](auto source, const auto &, auto, std::stop_token stop, auto) {
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
        [](auto, const auto &, auto, auto, auto) -> std::shared_ptr<const ui::DrawingSheetPlan> {
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

void drawingLayoutChecks() {
    limits();
    worker();
}
