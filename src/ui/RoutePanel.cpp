#include "RoutePanel.h"
#include "bending/application/SingleBendPreview.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <numbers>
namespace bending::ui {
namespace {
class StepStrip final : public QWidget {
  public:
    using QWidget::QWidget;
    int count = 1;
    QSize sizeHint() const override {
        return {count * 44 + 12, 60};
    }
};
} // namespace
RoutePanel::RoutePanel(QWidget *parent) : QWidget(parent) {
    setObjectName("bendRoutePanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    auto *actions = new QHBoxLayout;
    automatic_ = new QPushButton(QStringLiteral("Подобрать порядок"), this);
    automatic_->setObjectName("prepareBendRoute");
    manual_ = new QPushButton(QStringLiteral("Проверить мой порядок"), this);
    manual_->setObjectName("checkBendRoute");
    mode_ = new QComboBox(this);
    mode_->setObjectName("bendRouteMode");
    mode_->setAccessibleName(QStringLiteral("Охват проверки порядка гибов"));
    mode_->addItems(
        {QStringLiteral("Без инструмента"), QStringLiteral("С выбранным инструментом")});
    mode_->setCurrentIndex(1);
    actions->addWidget(automatic_);
    actions->addWidget(manual_);
    actions->addWidget(mode_);
    actions->addStretch();
    layout->addLayout(actions);
    stepStrip_ = new StepStrip(this);
    stepStrip_->setObjectName("routeStepStrip");
    auto *stripLayout = new QHBoxLayout(stepStrip_);
    stripLayout->setContentsMargins(4, 4, 4, 4);
    auto *scroll = new QScrollArea(stepStrip_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(52);
    auto *strip = new QWidget(scroll);
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
    details->addWidget(status_, 1);
    details->addWidget(earlier_);
    details->addWidget(later_);
    layout->addLayout(details);
    stepStrip_->setStyleSheet(QStringLiteral(
        "QWidget#routeStepStrip{background:#F4F7F9;border:1px solid #526171;border-radius:7px;}"
        "QScrollArea{background:transparent;border:0;}"
        "QPushButton{background:#FFFFFF;color:#1C2938;border:1px solid #DCE3E9;border-radius:4px;}"
        "QPushButton:hover{background:#E5F3F3;}"
        "QPushButton:checked{background:#087F83;color:white;border:1px solid #087F83;}"
        "QPushButton:focus{border:2px solid #4DADFF;}"));
    connect(automatic_, &QPushButton::clicked, this, [this] {
        if (onPrepare)
            onPrepare(true, mode_->currentIndex() == 1);
    });
    connect(manual_, &QPushButton::clicked, this, [this] {
        if (onPrepare)
            onPrepare(false, mode_->currentIndex() == 1);
    });
    connect(earlier_, &QPushButton::clicked, this, [this] { move(-1); });
    connect(later_, &QPushButton::clicked, this, [this] { move(1); });
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
void RoutePanel::setWithTools(bool withTools) {
    const QSignalBlocker block(mode_);
    mode_->setCurrentIndex(withTools ? 1 : 0);
}
void RoutePanel::move(int delta) {
    const int from = selected_ - 1, to = from + delta;
    if (from < 0 || to < 0 || to >= static_cast<int>(ids_.size()) || !onReorder)
        return;
    auto ids = ids_;
    std::swap(ids[static_cast<std::size_t>(from)], ids[static_cast<std::size_t>(to)]);
    selected_ += delta;
    onReorder(std::move(ids));
}
void RoutePanel::refresh(const application::DocumentSession &session,
                         application::BendRoutePtr route,
                         std::optional<application::BendRouteError> error, bool busy,
                         bool preparing, bool toolsAvailable) {
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
    if (changed) {
        while (auto *item = strip_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        steps_.clear();
        for (std::size_t i = 0; i <= ids_.size(); ++i) {
            const auto title = QString::number(i);
            auto *button = new QPushButton(title, this);
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
    bool included = !ids_.empty() && session.document() &&
                    ids_.size() == session.document()->metadata().bendAnalysis.bends.size();
    for (auto index : indices_)
        included &= application::included(session.bend(index)->review);
    const bool ready =
        included && ids_.size() <= 12 && !busy && (mode_->currentIndex() == 0 || toolsAvailable);
    automatic_->setEnabled(ready);
    manual_->setEnabled(ready);
    mode_->setEnabled(!busy);
    earlier_->setEnabled(!busy && selected_ > 1);
    later_->setEnabled(!busy && selected_ > 0 && selected_ < static_cast<int>(ids_.size()));
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        steps_[i]->setEnabled(!busy && (i > 0 || bool(route_)));
        steps_[i]->setChecked(static_cast<int>(i) == selected_ && (route_ || selected_ > 0));
    }
    QString message;
    if (preparing)
        message = QStringLiteral("Строим 3D-состояния и проверяем порядок… Можно отменить расчёт.");
    else if (error) {
        message =
            error->code == application::BendRouteErrorCode::SearchLimit
                ? QStringLiteral("Лимит поиска достигнут. Полный порядок пока не найден.")
            : error->code == application::BendRouteErrorCode::NoRoute
                ? QStringLiteral(
                      "В проверенных вариантах порядок не найден. Измените порядок или инструмент.")
                : QStringLiteral(
                      "Не удалось подготовить полный порядок для этой геометрии/настройки.");
        if (error->bendIndex)
            message += QStringLiteral(" Последняя попытка: B%1.")
                           .arg(*error->bendIndex + 1, 2, 10, QLatin1Char('0'));
        if (error->operationError) {
            using Code = application::BendPreviewErrorCode;
            switch (*error->operationError) {
            case Code::ToolTooShort:
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
        message += route_->withTools ? QStringLiteral("Циклы с инструментом проверены. ")
                                     : QStringLiteral("Движение без инструмента. ");
        if (selected_ > 0 && route_->withTools) {
            const auto *entry = session.bend(route_->steps[operation()].bendIndex);
            if (entry && entry->catalogTools)
                message += QStringLiteral("%1 / %2 · V%3. ")
                               .arg(QString::fromStdString(entry->catalogTools->punch.code),
                                    QString::fromStdString(entry->catalogTools->die.code))
                               .arg(entry->tools ? entry->tools->openingMm : 0, 0, 'f', 1);
        }
        message += QStringLiteral("Извлечение, перенос и станок не проверены.");
    } else if (ids_.size() > 12)
        message = QStringLiteral("Первый режим порядка поддерживает до 12 полных прямых гибов.");
    else if (!included)
        message = QStringLiteral("Для полного порядка нужны все распознанные зоны детали.");
    else if (mode_->currentIndex() == 1 && !toolsAvailable)
        message = QStringLiteral("Выберите инструмент либо режим «Без инструмента».");
    else
        message = QStringLiteral("Черновик порядка. «Проверить все гибы» или «Подобрать порядок» "
                                 "подготовит 3D-шаги из заготовки. После перестановки нажмите "
                                 "«Проверить мой порядок».");
    status_->setText(message);
    status_->setToolTip(
        error ? QString::fromStdString(error->detail)
              : QStringLiteral("Геометрическая заготовка сохраняет среднюю поверхность. "
                               "Материальная развёртка для раскроя рассчитывается отдельно."));
}
} // namespace bending::ui
