#pragma once
#include "bending/ui/DrawingSheet.h"
#include <QObject>
#include <QThread>
#include <functional>

namespace bending::ui {
// One active layout and one replaceable pending request. All public calls and
// signals belong to the GUI thread; the worker owns only immutable snapshots.
class DrawingLayoutController final : public QObject {
    Q_OBJECT
  public:
    using Composer = std::function<std::shared_ptr<const DrawingSheetPlan>(
        application::BendingDrawingPtr, const std::vector<DrawingViewChoice> &, DrawingFlatSide,
        std::stop_token, DrawingLayoutLimits)>;
    explicit DrawingLayoutController(QObject *parent = nullptr, Composer composer = {});
    ~DrawingLayoutController() override;
    void request(application::BendingDrawingPtr, std::vector<DrawingViewChoice>, DrawingFlatSide,
                 DrawingLayoutLimits = {});
    void cancel();
    [[nodiscard]] bool busy() const {
        return running_ || pending_.has_value();
    }

  signals:
    void ready(std::shared_ptr<const DrawingSheetPlan> plan);
    void failed(QString detail);

  private:
    struct Request {
        application::BendingDrawingPtr source;
        std::vector<DrawingViewChoice> choices;
        DrawingFlatSide side;
        DrawingLayoutLimits limits;
        std::stop_source stop;
        std::uint64_t generation;
    };
    void dispatch();
    Composer composer_;
    QThread thread_;
    QObject *worker_;
    std::optional<Request> pending_;
    std::stop_source activeStop_;
    std::uint64_t generation_ = 0;
    bool running_ = false, stopping_ = false;
};
} // namespace bending::ui
