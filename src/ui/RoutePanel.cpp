#include "RoutePanel.h"
#include "bending/application/SingleBendPreview.h"
#include "bending/core/BendSequence.h"
#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <numbers>
namespace bending::ui {
namespace {
const char *stepMime = "application/x-bending-route-step";
class StepButton final : public QPushButton {
  public:
    using QPushButton::QPushButton;
    std::function<void(bool)> hovered;

  protected:
    void enterEvent(QEnterEvent *e) override {
        QPushButton::enterEvent(e);
        if (hovered)
            hovered(true);
    }
    void leaveEvent(QEvent *e) override {
        QPushButton::leaveEvent(e);
        if (hovered)
            hovered(false);
    }
    void mousePressEvent(QMouseEvent *e) override {
        start_ = e->position().toPoint();
        QPushButton::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if ((e->buttons() & Qt::LeftButton) && property("routePosition").toInt() > 0 &&
            (e->position().toPoint() - start_).manhattanLength() >=
                QApplication::startDragDistance()) {
            QPointer<QDrag> drag = new QDrag(this);
            auto *mime = new QMimeData;
            mime->setData(stepMime, property("routeId").toString().toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(grab());
            setDown(false);
            drag->exec(Qt::MoveAction);
            if (drag)
                drag->deleteLater();
            return;
        }
        QPushButton::mouseMoveEvent(e);
    }

  private:
    QPoint start_;
};
class StepCanvas final : public QWidget {
  public:
    explicit StepCanvas(QWidget *parent) : QWidget(parent) {
        setAcceptDrops(true);
    }
    std::function<void(int, int)> moved;
    std::vector<QPushButton *> buttons;
    void clearMarker() {
        marker_ = -1;
        update();
    }

  protected:
    bool accepts(const QDropEvent *e) const {
        const auto *source = qobject_cast<QPushButton *>(e->source());
        return source && source->parentWidget() == this && source->isEnabled() &&
               source->property("routePosition").toInt() > 0 &&
               e->mimeData()->hasFormat(stepMime) &&
               e->mimeData()->data(stepMime) == source->property("routeId").toString().toUtf8();
    }
    void dragEnterEvent(QDragEnterEvent *e) override {
        if (accepts(e)) {
            e->setDropAction(Qt::MoveAction);
            e->accept();
        } else
            e->ignore();
    }
    int slot(QPoint p) const {
        int i = 1;
        for (; i < static_cast<int>(buttons.size()); ++i)
            if (p.x() < buttons[static_cast<std::size_t>(i)]->geometry().center().x())
                break;
        return i;
    }
    void dragMoveEvent(QDragMoveEvent *e) override {
        if (!accepts(e)) {
            e->ignore();
            return;
        }
        const auto at = slot(e->position().toPoint());
        marker_ = at < static_cast<int>(buttons.size())
                      ? buttons[static_cast<std::size_t>(at)]->x() - 2
                      : buttons.back()->geometry().right() + 2;
        if (auto *scroll = qobject_cast<QScrollArea *>(parentWidget()->parentWidget())) {
            const auto x = mapTo(scroll->viewport(), e->position().toPoint()).x();
            if (x < 28)
                scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->value() -
                                                        20);
            else if (x > scroll->viewport()->width() - 28)
                scroll->horizontalScrollBar()->setValue(scroll->horizontalScrollBar()->value() +
                                                        20);
        }
        update();
        e->setDropAction(Qt::MoveAction);
        e->accept();
    }
    void dragLeaveEvent(QDragLeaveEvent *e) override {
        clearMarker();
        e->accept();
    }
    void dropEvent(QDropEvent *e) override {
        if (!accepts(e)) {
            clearMarker();
            e->ignore();
            return;
        }
        const int from = e->source()->property("routePosition").toInt();
        int to = slot(e->position().toPoint());
        if (to > from)
            --to;
        clearMarker();
        e->setDropAction(Qt::MoveAction);
        e->accept();
        if (moved)
            moved(from - 1, to - 1);
    }
    void paintEvent(QPaintEvent *e) override {
        QWidget::paintEvent(e);
        if (marker_ >= 0) {
            QPainter p(this);
            p.setPen(QPen(QColor("#087F83"), 3));
            p.drawLine(marker_, 3, marker_, 39);
        }
    }

  private:
    int marker_ = -1;
};
class StepStrip final : public QWidget {
  public:
    using QWidget::QWidget;
    int count = 1;
    QSize sizeHint() const override {
        return {count * 44 + 12, 44};
    }
};
} // namespace
RoutePanel::RoutePanel(QWidget *parent) : QWidget(parent) {
    setObjectName("bendRoutePanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *advanced = new QWidget(this);
    advanced->setObjectName("routeAdvancedActions");
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto *actions = new QVBoxLayout;
    automatic_ = new QPushButton(QStringLiteral("Автопорядок"), this);
    automatic_->setObjectName("prepareBendRoute");
    manual_ = new QPushButton(QStringLiteral("Проверить мой порядок"), this);
    manual_->setObjectName("checkBendRoute");
    mode_ = new QComboBox(this);
    mode_->setObjectName("bendRouteMode");
    mode_->setAccessibleName(QStringLiteral("Охват проверки порядка гибов"));
    mode_->setToolTip(QStringLiteral("Охват полной проверки порядка. Ручной просмотр всегда "
                                     "проверяет столкновения с назначенным инструментом."));
    mode_->addItems({QStringLiteral("Без инструмента"), QStringLiteral("Одна пара для всех гибов"),
                     QStringLiteral("По назначениям гибов")});
    mode_->setCurrentIndex(1);
    actions->addWidget(manual_);
    actions->addWidget(mode_);
    expandSearch_ = new QPushButton(QStringLiteral("Расширить поиск"), this);
    expandSearch_->setObjectName("expandBendRouteSearch");
    expandSearch_->setToolTip(QStringLiteral("Повторить расчёт с начала: до 120 секунд поиска "
                                             "и 512 попыток. Это не обещание найти маршрут."));
    expandSearch_->hide();
    layout->addWidget(automatic_);
    layout->addWidget(expandSearch_);
    advancedLayout->addLayout(actions);
    stepStrip_ = new StepStrip(this);
    stepStrip_->setObjectName("routeStepStrip");
    auto *stripLayout = new QHBoxLayout(stepStrip_);
    stripLayout->setContentsMargins(2, 2, 2, 2);
    auto *scroll = new QScrollArea(stepStrip_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(40);
    auto *strip = new StepCanvas(scroll);
    strip->moved = [this](int from, int to) { moveTo(from, to); };
    strip_ = new QHBoxLayout(strip);
    strip_->setContentsMargins(0, 0, 0, 0);
    strip_->setSpacing(4);
    scroll->setWidget(strip);
    stripLayout->addWidget(scroll);
    earlier_ = new QPushButton(QStringLiteral("← Раньше"), this);
    later_ = new QPushButton(QStringLiteral("Позже →"), this);
    earlier_->setObjectName("routeMoveEarlier");
    later_->setObjectName("routeMoveLater");
    earlier_->setToolTip(QStringLiteral("Изменить порядок; потребуется новая проверка"));
    later_->setToolTip(earlier_->toolTip());
    auto *details = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setObjectName("bendRouteStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    advancedLayout->addWidget(status_);
    editTool_ = new QPushButton(QStringLiteral("Инструмент выбранного гиба…"), this);
    editTool_->setObjectName("routeEditTool");
    editTool_->setToolTip(QStringLiteral("Открыть инструмент выбранного гиба"));
    editTool_->setAccessibleName(QStringLiteral("Изменить инструмент выбранного гиба"));
    layout->addWidget(editTool_);
    details->addWidget(earlier_);
    details->addWidget(later_);
    undoButton_ = new QPushButton(QStringLiteral("↶"), this);
    redoButton_ = new QPushButton(QStringLiteral("↷"), this);
    undoButton_->setObjectName("routeUndo");
    redoButton_->setObjectName("routeRedo");
    undoButton_->setToolTip(QStringLiteral("Отменить перестановку (Ctrl+Z)"));
    redoButton_->setToolTip(QStringLiteral("Повторить перестановку (Ctrl+Shift+Z)"));
    undoButton_->setAccessibleName(QStringLiteral("Отменить перестановку гибов"));
    redoButton_->setAccessibleName(QStringLiteral("Повторить перестановку гибов"));
    for (auto *button : {undoButton_, redoButton_}) {
        button->setFixedWidth(32);
        details->addWidget(button);
    }
    connect(undoButton_, &QPushButton::clicked, this, [this] { undo(); });
    connect(redoButton_, &QPushButton::clicked, this, [this] { redo(); });
    for (const auto &[key, redo] :
         {std::pair{QKeySequence(Qt::CTRL | Qt::Key_Z), false},
          std::pair{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), true}}) {
        auto *shortcut = new QShortcut(key, stepStrip_);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, redo] {
            if (redo)
                this->redo();
            else
                undo();
        });
    }
    advancedLayout->addLayout(details);
    auto *more = new QToolButton(this);
    more->setObjectName("routeActionsToggle");
    more->setText(QStringLiteral("Действия шага и параметры ▸"));
    more->setCheckable(true);
    more->setMinimumHeight(32);
    layout->addWidget(more);
    layout->addWidget(advanced);
    advanced->hide();
    connect(more, &QToolButton::toggled, advanced, &QWidget::setVisible);
    stepStrip_->setStyleSheet(QStringLiteral(
        "QWidget#routeStepStrip{background:transparent;border:0;}"
        "QScrollArea{background:transparent;border:0;}"
        "QPushButton{background:#FFFFFF;color:#1C2938;border:1px solid #DCE3E9;border-radius:4px;}"
        "QPushButton:hover{background:#E5F3F3;}"
        "QPushButton:checked{background:#087F83;color:white;border:1px solid #087F83;}"
        "QPushButton:focus{border:2px solid #4DADFF;}"));
    connect(automatic_, &QPushButton::clicked, this, [this] {
        if (onPrepare)
            onPrepare(true, mode_->currentIndex() != 0);
    });
    connect(manual_, &QPushButton::clicked, this, [this] {
        if (onPrepare)
            onPrepare(false, mode_->currentIndex() != 0);
    });
    connect(expandSearch_, &QPushButton::clicked, this, [this] {
        if (onExpandSearch)
            onExpandSearch();
    });
    connect(earlier_, &QPushButton::clicked, this, [this] { move(-1); });
    connect(later_, &QPushButton::clicked, this, [this] { move(1); });
    connect(editTool_, &QPushButton::clicked, this, [this] {
        if (const auto index = selectedBend(); index && onEditTool)
            onEditTool(*index);
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (onModeChanged)
            onModeChanged();
    });
}
void RoutePanel::select(int step) {
    selected_ = std::clamp(step, 0, static_cast<int>(ids_.size()));
    for (std::size_t i = 0; i < steps_.size(); ++i)
        steps_[i]->setChecked(static_cast<int>(i) == selected_);
    if (onSelect)
        onSelect(selected_);
}
void RoutePanel::setWithTools(bool withTools, bool perBend) {
    const QSignalBlocker block(mode_);
    mode_->setCurrentIndex(withTools ? (perBend ? 2 : 1) : 0);
}
bool RoutePanel::perBendTools() const {
    return mode_->currentIndex() == 2;
}
bool RoutePanel::withTools() const {
    return mode_->currentIndex() != 0;
}
QString RoutePanel::statusText() const {
    return status_->text();
}
QWidget *RoutePanel::reportWidget() const {
    return status_;
}
void RoutePanel::move(int delta) {
    moveTo(selected_ - 1, selected_ - 1 + delta);
}
void RoutePanel::moveTo(int from, int to) {
    if (from < 0 || to < 0 || to >= static_cast<int>(ids_.size()) || !onReorder)
        return;
    core::BendSequence sequence(ids_);
    if (!sequence.move(static_cast<std::size_t>(from), static_cast<std::size_t>(to)))
        return;
    const auto before = ids_;
    const auto selected = ids_[static_cast<std::size_t>(from)];
    selected_ = to + 1;
    if (onReorder(sequence.bendIds())) {
        undo_.push_back({before, sequence.bendIds(), selected});
        redo_.clear();
        if (undo_.size() > 50)
            undo_.erase(undo_.begin());
    }
    undoButton_->setEnabled(!undo_.empty());
    redoButton_->setEnabled(!redo_.empty());
}
void RoutePanel::undo() {
    if (undo_.empty() || !onReorder || ids_ != undo_.back().after)
        return;
    const auto edit = undo_.back();
    selected_ = static_cast<int>(std::find(edit.before.begin(), edit.before.end(), edit.selected) -
                                 edit.before.begin()) +
                1;
    if (onReorder(edit.before)) {
        undo_.pop_back();
        redo_.push_back(edit);
    }
    undoButton_->setEnabled(!undo_.empty());
    redoButton_->setEnabled(!redo_.empty());
}
void RoutePanel::redo() {
    if (redo_.empty() || !onReorder || ids_ != redo_.back().before)
        return;
    const auto edit = redo_.back();
    selected_ = static_cast<int>(std::find(edit.after.begin(), edit.after.end(), edit.selected) -
                                 edit.after.begin()) +
                1;
    if (onReorder(edit.after)) {
        redo_.pop_back();
        undo_.push_back(edit);
    }
    undoButton_->setEnabled(!undo_.empty());
    redoButton_->setEnabled(!redo_.empty());
}
void RoutePanel::refresh(const application::DocumentSession &session,
                         application::BendRoutePtr route,
                         std::optional<application::BendRouteError> error, bool busy,
                         bool preparing, bool toolsAvailable, application::RouteDraftPtr draft,
                         std::optional<application::BendRouteError> draftError,
                         bool preparingDraft) {
    if (documentRevision_ != session.revision()) {
        documentRevision_ = session.revision();
        selected_ = 0;
        undo_.clear();
        redo_.clear();
    }
    const auto indices = session.orderedBends();
    std::vector<std::string> ids;
    for (auto i : indices)
        if (const auto *b = session.bend(i))
            ids.push_back(b->id);
    const bool changed = ids != ids_ || route != route_;
    const bool newRoute = route && route != route_;
    ids_ = std::move(ids);
    indices_ = indices;
    route_ = std::move(route);
    selected_ = newRoute ? 0 : std::clamp(selected_, 0, static_cast<int>(ids_.size()));
    if (changed && steps_.size() != ids_.size() + 1) {
        while (auto *item = strip_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        steps_.clear();
        for (std::size_t i = 0; i <= ids_.size(); ++i) {
            const auto title = QString::number(i);
            auto *button = new StepButton(title, strip_->parentWidget());
            button->hovered = [this, i](bool entered) {
                if (onHover)
                    onHover(entered && i && i <= indices_.size() ? std::optional{indices_[i - 1]}
                                                                 : std::nullopt);
            };
            button->setObjectName(QStringLiteral("routeStep%1").arg(i));
            button->setCheckable(true);
            button->setFixedSize(40, 36);
            button->setToolTip(
                i == 0 ? QStringLiteral("0 — заготовка до первого гиба")
                       : (route_ ? QStringLiteral("Шаг %1 · B%2 · показать начало операции")
                                 : QStringLiteral("Черновик: шаг %1 · B%2 · выделить гиб детали"))
                             .arg(i)
                             .arg(indices_[i - 1] + 1, 2, 10, QLatin1Char('0')));
            button->setAccessibleName(i == 0 ? QStringLiteral("Показать заготовку")
                                             : (route_ ? QStringLiteral("Показать операцию %1")
                                                       : QStringLiteral("Выделить гиб для шага %1"))
                                                   .arg(i));
            strip_->addWidget(button);
            steps_.push_back(button);
            connect(button, &QPushButton::clicked, this,
                    [this, i] { select(static_cast<int>(i)); });
        }
        strip_->addStretch();
        static_cast<StepStrip *>(stepStrip_)->count = static_cast<int>(ids_.size() + 1);
        stepStrip_->adjustSize();
    }
    auto *canvas = static_cast<StepCanvas *>(strip_->parentWidget());
    canvas->buttons = steps_;
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        steps_[i]->setProperty("routePosition", static_cast<int>(i));
        steps_[i]->setProperty("routeId", i ? QString::fromStdString(ids_[i - 1]) : QString{});
        steps_[i]->setToolTip(
            i ? (route_ ? QString{} : QStringLiteral("Черновик · ")) +
                    QStringLiteral("Шаг %1 · B%2 · форма после гиба. Перетащите между шагами.")
                        .arg(i)
                        .arg(indices_[i - 1] + 1, 2, 10, QLatin1Char('0'))
              : QStringLiteral("0 — заготовка: все гибы разогнуты"));
        steps_[i]->setAccessibleName(i ? QStringLiteral("Показать результат шага %1").arg(i)
                                       : QStringLiteral("Показать заготовку"));
    }
    bool included = !ids_.empty() && session.document() &&
                    ids_.size() == session.document()->metadata().bendAnalysis.bends.size();
    bool assigned = included;
    std::optional<std::size_t> missingTool;
    for (auto index : indices_)
        if (const auto *entry = session.bend(index)) {
            included &= application::included(entry->review);
            if (!entry->tools) {
                assigned = false;
                if (!missingTool)
                    missingTool = index;
            }
        }
    const bool ready = included && ids_.size() <= 12 && !busy && !preparingDraft &&
                       (mode_->currentIndex() == 0 || (perBendTools() ? assigned : toolsAvailable));
    automatic_->setEnabled(ready);
    manual_->setEnabled(ready);
    mode_->setEnabled(!busy);
    const bool canExpand =
        error && error->code == application::BendRouteErrorCode::SearchLimit && !expandedSearch_;
    expandSearch_->setVisible(canExpand);
    expandSearch_->setEnabled(canExpand && !busy);
    earlier_->setEnabled(!busy && selected_ > 1);
    later_->setEnabled(!busy && selected_ > 0 && selected_ < static_cast<int>(ids_.size()));
    editTool_->setVisible(selected_ > 0);
    editTool_->setEnabled(!busy && selected_ > 0);
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        steps_[i]->setEnabled(!busy && included);
        steps_[i]->setChecked(static_cast<int>(i) == selected_);
    }
    undoButton_->setEnabled(!busy && !undo_.empty() && ids_ == undo_.back().after);
    redoButton_->setEnabled(!busy && !redo_.empty() && ids_ == redo_.back().before);
    QString message;
    if (preparing)
        message = QStringLiteral("Строим 3D-состояния и проверяем порядок… Можно отменить расчёт.");
    else if (preparingDraft)
        message = QStringLiteral("Ручной просмотр: готовим выбранный гиб и проверяем столкновения… "
                                 "Порядок можно менять.");
    else if (draftError)
        message =
            QStringLiteral("Не удалось подготовить ручной просмотр; столкновения не проверены. %1")
                .arg(QString::fromStdString(draftError->detail));
    else if (error) {
        message =
            error->toolLengths ? QStringLiteral("Недостаточная длина выбранной сборки инструмента.")
            : error->code == application::BendRouteErrorCode::SearchLimit
                ? QStringLiteral("Лимит поиска достигнут. Полный порядок пока не найден.")
            : error->code == application::BendRouteErrorCode::NoRoute
                ? QStringLiteral(
                      "В проверенных вариантах порядок не найден. Измените порядок или инструмент.")
                : QStringLiteral(
                      "Не удалось подготовить полный порядок для этой геометрии/настройки.");
        if (error->bendIndex)
            message += (error->toolLengths ? QStringLiteral(" Гиб B%1.")
                                           : QStringLiteral(" Последняя попытка: B%1."))
                           .arg(*error->bendIndex + 1, 2, 10, QLatin1Char('0'));
        if (error->toolLengths) {
            const auto &lengths = *error->toolLengths;
            message += QStringLiteral(" Нужно %1 мм; пуансон %2 мм, матрица %3 мм. "
                                      "Подберите секции на этапе «Инструмент».")
                           .arg(lengths.bendLengthMm, 0, 'f', 2)
                           .arg(lengths.punchLengthMm, 0, 'f', 2)
                           .arg(lengths.dieLengthMm, 0, 'f', 2);
        }
        if (error->operationError) {
            using Code = application::BendPreviewErrorCode;
            switch (*error->operationError) {
            case Code::ToolTooShort:
                if (!error->toolLengths)
                    message += QStringLiteral(" Инструмент короче линии гиба.");
                break;
            case Code::MissingLeftSupport:
            case Code::MissingRightSupport:
            case Code::SupportNotProven:
                message += QStringLiteral(" Не подтверждена опора на матрицу.");
                break;
            case Code::ToolIntersection:
                message += QStringLiteral(" Обнаружено столкновение с инструментом.");
                break;
            case Code::MachineIntersection:
                message += QStringLiteral(" Столкновение с узлом демонстрационного станка: %1.")
                               .arg(QString::fromStdString(error->detail));
                break;
            case Code::SheetIntersection:
                message += QStringLiteral(" Стенки детали пересекаются при движении.");
                break;
            case Code::ToolAngleConflict:
                message += QStringLiteral(" Угол инструмента не подходит.");
                break;
            case Code::NoseTooLarge:
                message += QStringLiteral(" Радиус пуансона слишком велик.");
                break;
            case Code::NarrowDieOpening:
                message += QStringLiteral(" Раскрытие V слишком мало для толщины.");
                break;
            case Code::RetractionNotProven:
                message += QStringLiteral(" Не подтверждён свободный отвод пуансона.");
                break;
            case Code::ContactLost:
                message += QStringLiteral(" Не подтверждён контакт с инструментом.");
                break;
            case Code::UnsupportedToolGeometry:
                message += QStringLiteral(" Контакт этого профиля пока не поддержан.");
                break;
            default:
                break;
            }
        }
    } else if (route_) {
        if (selected_ == 0)
            message = QStringLiteral("Плоская геометрическая заготовка · %1 операций. ")
                          .arg(route_->steps.size());
        else {
            const auto &step = route_->steps[operation()];
            const auto &b = session.document()->metadata().bendAnalysis.bends[step.bendIndex];
            message =
                QStringLiteral("Шаг %1/%2 · B%6 · целевой внутренний угол %3° · L %4 мм · подача "
                               "со стороны %5. ")
                    .arg(selected_)
                    .arg(route_->steps.size())
                    .arg((std::numbers::pi - b.angleRadians) * 180 / std::numbers::pi, 0, 'f', 1)
                    .arg(b.lengthMm, 0, 'f', 1)
                    .arg(step.fixedSecond ? "B" : "A")
                    .arg(step.bendIndex + 1, 2, 10, QLatin1Char('0'));
        }
        message += route_->partialEndSupport ? QStringLiteral("Неполная опора у торца. ")
                   : route_->withTools ? QStringLiteral("Циклы с инструментом проверены. ")
                                       : QStringLiteral("Движение без инструмента. ");
        if (selected_ > 0)
            if (const auto *support = route_->steps[operation()].preview->partialEndSupport())
                message += QStringLiteral("Материал под опорой: %1–%2 из %3 мм. ")
                               .arg(support->supportedFromMm, 0, 'f', 2)
                               .arg(support->supportedToMm, 0, 'f', 2)
                               .arg(support->bendLengthMm, 0, 'f', 2);
        if (selected_ > 0 && route_->withTools) {
            const auto *entry = session.bend(route_->steps[operation()].bendIndex);
            if (entry && entry->catalogTools)
                message += QStringLiteral("%1 / %2 · V%3. ")
                               .arg(QString::fromStdString(entry->catalogTools->punch.code),
                                    QString::fromStdString(entry->catalogTools->die.code))
                               .arg(entry->tools ? entry->tools->openingMm : 0, 0, 'f', 1);
            if (entry && entry->tools)
                message += QStringLiteral("Пуансон %1 мм, матрица %2 мм. ")
                               .arg(core::punchLengthMm(*entry->tools), 0, 'f', 1)
                               .arg(entry->tools->lengthMm, 0, 'f', 1);
            if (operation() > 0) {
                const auto *previous = session.bend(route_->steps[operation() - 1].bendIndex);
                if (entry && previous &&
                    (entry->tools != previous->tools ||
                     entry->catalogTools != previous->catalogTools))
                    message += QStringLiteral("Установка отличается от предыдущего шага; "
                                              "переналадка не проверена. ");
            }
        }
        const bool demoMachine =
            std::any_of(route_->steps.begin(), route_->steps.end(), [](const auto &s) {
                return s.preview && s.preview->machineModel() == core::MachineModel::DemoPressV1;
            });
        message += demoMachine
                       ? QStringLiteral("Учебный станок проверен по кадрам. Реальные габариты, "
                                        "ход, извлечение и перенос не подтверждены.")
                       : QStringLiteral("Извлечение, перенос и станок не проверены.");
    } else if (draft) {
        message = selected_ ? QStringLiteral("Ручной порядок · шаг %1/%2 · B%3. ")
                                  .arg(selected_)
                                  .arg(ids_.size())
                                  .arg(indices_[operation()] + 1, 2, 10, QLatin1Char('0'))
                            : QStringLiteral("0 · Плоская заготовка. ");
        message +=
            draft->checkedToolPositions
                ? QStringLiteral("Столкновения проверены по кадрам. Полный порядок не проверен.")
                : QStringLiteral("Выберите инструмент для проверки столкновений.");
    } else if (ids_.size() > 12)
        message = QStringLiteral("Первый режим порядка поддерживает до 12 полных прямых гибов.");
    else if (!included)
        message = QStringLiteral("Для полного порядка нужны все распознанные зоны детали.");
    else if (perBendTools() && missingTool)
        message = QStringLiteral("Не назначен инструмент для B%1. Выберите шаг → «Инструмент…». "
                                 "Назначения сохраняются отдельно для каждого гиба.")
                      .arg(*missingTool + 1, 2, 10, QLatin1Char('0'));
    else if (mode_->currentIndex() == 1 && !toolsAvailable)
        message = QStringLiteral("Выберите инструмент либо режим «Без инструмента».");
    else
        message = QStringLiteral("Выберите 0 или номер для ручного 3D-просмотра. Перетаскивайте "
                                 "шаги; затем «Проверить мой порядок».");
    status_->setText(message);
    updateGeometry();
    status_->setToolTip(
        error ? QString::fromStdString(error->detail)
              : QStringLiteral("Геометрическая заготовка сохраняет среднюю поверхность. "
                               "Материальная развёртка для раскроя рассчитывается отдельно."));
}
} // namespace bending::ui
