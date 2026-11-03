#include "DrawingDialog.h"
#include "bending/ui/DrawingSheet.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/Preferences.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace bending::ui {
namespace {
QString scaleLabel(double scale) {
    return scale >= 1 ? QStringLiteral("%1:1").arg(scale, 0, 'g', 4)
                      : QStringLiteral("1:%1").arg(1 / scale, 0, 'g', 4);
}
class DrawingPreviewArea final : public QScrollArea {
  public:
    explicit DrawingPreviewArea(QWidget *parent) : QScrollArea(parent) {
        setObjectName(QStringLiteral("drawingPreviewScroll"));
        setAlignment(Qt::AlignCenter);
        footer_ = new QFrame(this);
        footer_->setObjectName(QStringLiteral("drawingPreviewControls"));
        navigation_ = new QWidget(footer_);
        navigation_->setObjectName(QStringLiteral("drawingPageNavigation"));
        footer_->setStyleSheet(QStringLiteral(
            "QFrame#drawingPreviewControls{background:#FFFFFF;border:1px solid #D4DCE2;"
            "border-radius:10px;}"
            "QToolButton{background:transparent;color:#334155;border:0;border-radius:6px;}"
            "QToolButton:hover{background:#EEF4F5;}"
            "QToolButton:checked{background:#E5F3F3;}"
            "QToolButton:focus{border:2px solid #087F83;}"
            "QToolButton:disabled{color:#B6BEC7;}"
            "QSpinBox{background:transparent;color:#263542;border:0;padding:2px;}"
            "QSpinBox:focus{background:#E5F3F3;border-radius:4px;}"));
        auto *bar = new QHBoxLayout(footer_);
        bar->setContentsMargins(6, 4, 6, 4);
        bar->setSpacing(8);
        bar->addWidget(navigation_);
        auto *row = new QHBoxLayout(navigation_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        const auto action = [&](const QString &text, const char *name,
                                const QKeySequence &shortcut) {
            auto *a = new QAction(text, this);
            a->setObjectName(QLatin1String(name));
            a->setShortcut(shortcut);
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            a->setToolTip(text + QStringLiteral(" · ") +
                          shortcut.toString(QKeySequence::NativeText));
            parent->addAction(a);
            return a;
        };
        previous_ = action(QStringLiteral("Предыдущий лист"), "previousDrawingPage",
                           QKeySequence(Qt::ALT | Qt::Key_Left));
        next_ = action(QStringLiteral("Следующий лист"), "nextDrawingPage",
                       QKeySequence(Qt::ALT | Qt::Key_Right));
        const auto button = [&](QAction *a, Qt::ArrowType direction, const char *name) {
            auto *b = new QToolButton(navigation_);
            b->setObjectName(QLatin1String(name));
            a->setIconText(direction == Qt::LeftArrow ? QStringLiteral("‹") : QStringLiteral("›"));
            b->setDefaultAction(a);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            auto glyphFont = b->font();
            glyphFont.setPixelSize(24);
            b->setFont(glyphFont);
            b->setAccessibleName(a->text());
            b->setFocusPolicy(Qt::StrongFocus);
            b->setFixedSize(32, 32);
            return b;
        };
        auto *previousButton = button(previous_, Qt::LeftArrow, "previousDrawingPageButton");
        row->addWidget(previousButton);
        page_ = new QSpinBox(navigation_);
        page_->setObjectName(QStringLiteral("drawingPage"));
        page_->setRange(1, 1);
        page_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        page_->setKeyboardTracking(false);
        page_->setAlignment(Qt::AlignCenter);
        page_->setAccessibleName(QStringLiteral("Номер листа предпросмотра"));
        page_->setToolTip(QStringLiteral("Введите номер листа и нажмите Enter"));
        row->addWidget(page_);
        auto *nextButton = button(next_, Qt::RightArrow, "nextDrawingPageButton");
        row->addWidget(nextButton);
        setTabOrder(previousButton, page_);
        setTabOrder(page_, nextButton);
        connect(previous_, &QAction::triggered, page_, &QSpinBox::stepDown);
        connect(next_, &QAction::triggered, page_, &QSpinBox::stepUp);
        zoomLabel_ = new QLabel(footer_);
        zoomLabel_->setObjectName(QStringLiteral("drawingPreviewZoom"));
        zoomLabel_->setAlignment(Qt::AlignCenter);
        zoomLabel_->setMinimumWidth(48);
        zoomLabel_->setToolTip(QStringLiteral("Масштаб предпросмотра. Изменяется колёсиком мыши."));
        bar->addWidget(zoomLabel_);
        fit_ =
            action(QStringLiteral("Вписать"), "fitDrawingPage", QKeySequence(Qt::CTRL | Qt::Key_0));
        fit_->setCheckable(true);
        auto *fitButton = new QToolButton(footer_);
        fitButton->setObjectName(QStringLiteral("fitDrawingPageButton"));
        fitButton->setDefaultAction(fit_);
        fitButton->setMinimumHeight(32);
        bar->addWidget(fitButton);
        connect(fit_, &QAction::triggered, this, [this] {
            fitToWindow_ = true;
            fitPaper();
        });
        auto *zoomIn = action(QStringLiteral("Увеличить"), "zoomInDrawingPage",
                              QKeySequence(Qt::CTRL | Qt::Key_Plus));
        zoomIn->setShortcuts(
            {QKeySequence(Qt::CTRL | Qt::Key_Plus), QKeySequence(Qt::CTRL | Qt::Key_Equal)});
        auto *zoomOut = action(QStringLiteral("Уменьшить"), "zoomOutDrawingPage",
                               QKeySequence(Qt::CTRL | Qt::Key_Minus));
        connect(zoomIn, &QAction::triggered, this,
                [this] { zoomAt(1.2, viewport()->rect().center()); });
        connect(zoomOut, &QAction::triggered, this,
                [this] { zoomAt(1 / 1.2, viewport()->rect().center()); });
        viewport()->setToolTip(QStringLiteral("Колесо — масштаб · перетаскивание — перемещение · "
                                              "Ctrl+0 — вписать лист"));
        viewport()->installEventFilter(this);
        setPageCount(0);
        setMinimumWidth(footer_->sizeHint().width() + 32);
    }
    std::function<void()> viewChanged;
    std::function<void(QPointF)> paperClicked;
    void setPaper(QWidget *paper) {
        setWidget(paper);
        paper->installEventFilter(this);
    }
    QSpinBox *pageControl() const {
        return page_;
    }
    QSize paperSize() const {
        if (!fitToWindow_)
            return {paperWidth_,
                    static_cast<int>(paperWidth_ * DrawingSheet::heightMm / DrawingSheet::widthMm)};
        const auto available = viewport()->size() - QSize(24, 24);
        if (available.width() < 2 || available.height() < 2)
            return {};
        const auto width = std::min(
            available.width(),
            static_cast<int>(available.height() * DrawingSheet::widthMm / DrawingSheet::heightMm));
        return {width, static_cast<int>(width * DrawingSheet::heightMm / DrawingSheet::widthMm)};
    }
    void fitPaper() {
        if (widget() && !paperSize().isEmpty())
            widget()->resize(paperSize());
        fit_->setChecked(fitToWindow_);
        zoomLabel_->setText(
            QStringLiteral("%1%").arg(qRound(paperSize().width() / referenceWidth * 100)));
        viewport()->setCursor(fitToWindow_ ? Qt::ArrowCursor : Qt::OpenHandCursor);
        if (viewChanged)
            viewChanged();
    }
    void setPageCount(int count) {
        const QSignalBlocker blocker(page_);
        page_->setRange(1, std::max(1, count));
        page_->setSuffix(QStringLiteral(" из %1").arg(std::max(1, count)));
        page_->setFixedWidth(std::max(72, page_->fontMetrics().horizontalAdvance(
                                              QStringLiteral("%1 из %1").arg(std::max(1, count))) +
                                              16));
        previous_->setEnabled(count > 1 && page_->value() > 1);
        next_->setEnabled(count > 1 && page_->value() < count);
        page_->setEnabled(count > 1);
        navigation_->setVisible(count > 1);
        footer_->setVisible(count > 0);
        fit_->setEnabled(count > 0);
        footer_->adjustSize();
        // Reserve a footer inside the preview frame, outside the paper viewport.
        setViewportMargins(0, 0, 0, count > 0 ? footer_->height() + 16 : 0);
        positionNavigation();
        fitPaper();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QScrollArea::resizeEvent(event);
        positionNavigation();
        fitPaper();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == viewport() && event->type() == QEvent::Resize) {
            positionNavigation();
            fitPaper();
        }
        if ((object == viewport() || object == widget()) && footer_->isVisible()) {
            if (event->type() == QEvent::Wheel) {
                auto *wheel = static_cast<QWheelEvent *>(event);
                const auto delta = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y()
                                                                : wheel->pixelDelta().y();
                if (delta != 0) {
                    const auto anchor =
                        viewport()->mapFromGlobal(wheel->globalPosition().toPoint());
                    zoomAt(std::pow(1.2, std::clamp(delta / 120.0, -4.0, 4.0)), anchor);
                }
                wheel->accept();
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton || mouse->button() == Qt::MiddleButton) {
                    dragging_ = true;
                    dragPosition_ = mouse->globalPosition().toPoint();
                    pressPosition_ = dragPosition_;
                    viewport()->setCursor(Qt::ClosedHandCursor);
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && dragging_) {
                const auto position = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
                const auto delta = position - dragPosition_;
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
                verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
                dragPosition_ = position;
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease && dragging_) {
                dragging_ = false;
                viewport()->setCursor(fitToWindow_ ? Qt::ArrowCursor : Qt::OpenHandCursor);
                const auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton && paperClicked && widget() &&
                    (mouse->globalPosition().toPoint() - pressPosition_).manhattanLength() < 5) {
                    const auto point = widget()->mapFromGlobal(mouse->globalPosition().toPoint());
                    if (widget()->rect().contains(point))
                        paperClicked({point.x() * DrawingSheet::widthMm / widget()->width(),
                                      point.y() * DrawingSheet::heightMm / widget()->height()});
                }
                return true;
            }
        }
        return QScrollArea::eventFilter(object, event);
    }

  private:
    static constexpr double referenceWidth = DrawingSheet::widthMm * 96 / 25.4;
    QFrame *footer_ = nullptr;
    QWidget *navigation_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QAction *fit_ = nullptr;
    bool fitToWindow_ = true;
    bool dragging_ = false;
    int paperWidth_ = 0;
    QPoint dragPosition_;
    QPoint pressPosition_;
    QSpinBox *page_ = nullptr;
    QAction *previous_ = nullptr;
    QAction *next_ = nullptr;
    void zoomAt(double factor, const QPoint &anchor) {
        if (!widget() || !footer_->isVisible() || widget()->width() < 1)
            return;
        const auto point = widget()->mapFrom(viewport(), anchor);
        const QPointF fraction(static_cast<double>(point.x()) / widget()->width(),
                               static_cast<double>(point.y()) / widget()->height());
        paperWidth_ = std::clamp(qRound(widget()->width() * factor), qRound(referenceWidth * .1),
                                 qRound(referenceWidth * 4));
        fitToWindow_ = false;
        fitPaper();
        const auto offset = widget()->mapTo(viewport(), QPoint{}) +
                            QPoint(qRound(fraction.x() * widget()->width()),
                                   qRound(fraction.y() * widget()->height())) -
                            anchor;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + offset.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + offset.y());
    }
    void positionNavigation() {
        if (!footer_)
            return;
        const auto area = viewport()->geometry();
        footer_->move(area.left() + (area.width() - footer_->width()) / 2, area.bottom() + 9);
        footer_->raise();
    }
};
} // namespace
class DrawingDialog::Impl {
  public:
    ImportController &controller;
    std::shared_ptr<const application::BendingDrawingBuilder> builder;
    std::uint64_t revision;
    QComboBox *base;
    QComboBox *turn;
    QCheckBox *reverse;
    QCheckBox *breaks;
    QLineEdit *name;
    QLineEdit *designation;
    QLineEdit *material;
    QLineEdit *execution;
    QSpinBox *quantity;
    QSpinBox *page;
    DrawingPreviewArea *preview;
    QTimer *refit;
    QLabel *image;
    QLabel *status;
    QPushButton *prepare;
    QPushButton *exportPdf;
    QListWidget *views;
    QComboBox *viewScale;
    QComboBox *flatSide;
    QLabel *sideInfo;
    std::vector<DrawingViewChoice> choices;
    application::BendingDrawingPtr compositionSource;
    std::shared_ptr<const DrawingSheetPlan> plan;
    std::optional<std::size_t> selected;
    bool settingsChanged = false;
    bool started = false;
    Impl(ImportController &c, std::shared_ptr<const application::BendingDrawingBuilder> b,
         std::optional<std::size_t> face)
        : controller(c), builder(std::move(b)), revision(c.session().revision()), selected(face) {}
    DrawingTitle title() const {
        return {name->text(),
                designation->text(),
                material->text(),
                execution->text(),
                quantity->value() ? std::optional<int>{quantity->value()} : std::nullopt,
                DrawingLayout::Classical,
                plan};
    }
    void ensurePlan(const application::BendingDrawingPtr &drawing) {
        if (compositionSource != drawing) {
            compositionSource = drawing;
            choices = DrawingSheet::defaultViews(*drawing);
            plan.reset();
            const QSignalBlocker block(views), blockSide(flatSide);
            views->clear();
            for (const auto &choice : choices) {
                auto *item =
                    new QListWidgetItem(DrawingSheet::viewName(*drawing, choice.key), views);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(choice.visible ? Qt::Checked : Qt::Unchecked);
            }
            flatSide->setCurrentIndex(0);
            const bool known =
                drawing->flatPattern && drawing->flatPattern->info.positiveZIsSideA.has_value();
            flatSide->setItemText(1,
                                  known ? QStringLiteral("Сторона A") : QStringLiteral("Исходная"));
            flatSide->setItemText(2,
                                  known ? QStringLiteral("Сторона B") : QStringLiteral("Обратная"));
            views->setCurrentRow(0);
        }
        if (!plan)
            plan = DrawingSheet::compose(drawing, choices,
                                         static_cast<DrawingFlatSide>(flatSide->currentIndex()));
        selectionControls();
    }
    void selectionControls() {
        const QSignalBlocker block(viewScale), blockViews(views);
        const int row = views->currentRow();
        const bool valid = row >= 0 && static_cast<std::size_t>(row) < choices.size();
        viewScale->setEnabled(valid);
        viewScale->setCurrentIndex(0);
        if (valid && choices[row].scale)
            for (int i = 1; i < viewScale->count(); ++i)
                if (viewScale->itemData(i).toDouble() == *choices[row].scale)
                    viewScale->setCurrentIndex(i);
        const bool flat = valid && choices[row].key.kind == DrawingViewKind::Flat;
        flatSide->setVisible(flat);
        sideInfo->setVisible(flat);
        if (flat && compositionSource->flatPattern) {
            const auto orientation = DrawingSheet::flatOrientation(
                compositionSource->flatPattern->info,
                static_cast<DrawingFlatSide>(flatSide->currentIndex()));
            sideInfo->setText(
                QStringLiteral("ВВЕРХ: %1 · ВНИЗ: %2").arg(orientation.up).arg(orientation.down));
        }
        if (plan && compositionSource)
            for (std::size_t i = 0; i < choices.size(); ++i) {
                const auto at =
                    std::find_if(plan->views.begin(), plan->views.end(),
                                 [&](const auto &v) { return v.key == choices[i].key; });
                auto label = DrawingSheet::viewName(*compositionSource, choices[i].key);
                if (at != plan->views.end())
                    label += QStringLiteral(" · %1").arg(scaleLabel(at->scale));
                views->item(static_cast<int>(i))->setText(label);
            }
    }
    void selectView(int row, bool followPage = true) {
        selectionControls();
        if (followPage && plan && row >= 0 && static_cast<std::size_t>(row) < choices.size()) {
            const auto at = std::find_if(plan->views.begin(), plan->views.end(),
                                         [&](const auto &v) { return v.key == choices[row].key; });
            if (at != plan->views.end()) {
                const QSignalBlocker block(page);
                page->setValue(at->page + 1);
            }
        }
        render();
    }
    void invalidate() {
        settingsChanged = true;
        exportPdf->setEnabled(false);
        status->setText(QStringLiteral("Ориентация изменена. Нажмите «Сформировать виды»."));
        image->clear();
        preview->setPageCount(0);
    }
    void render(bool contentChanged = true) {
        const auto drawing = controller.drawing();
        if (!drawing || settingsChanged || controller.session().revision() != revision)
            return;
        try {
            ensurePlan(drawing);
            if (contentChanged) {
                const auto previousPage = page->value();
                preview->setPageCount(DrawingSheet::pageCount(*drawing, title()));
                if (page->value() != previousPage)
                    preview->verticalScrollBar()->setValue(0);
            }
            const auto fit = preview->paperSize();
            if (fit.isEmpty())
                return;
            const auto ratio = preview->devicePixelRatioF();
            // Bound the full-page raster while allowing a larger logical zoom.
            const int pixels = std::clamp(qRound(fit.width() * ratio), 2, 4096);
            const auto current = image->pixmap();
            if (!contentChanged && !current.isNull() && current.width() == pixels &&
                qFuzzyCompare(current.devicePixelRatioF(), ratio))
                return;
            auto raster = DrawingSheet::preview(*drawing, title(), page->value() - 1, pixels);
            const int selectedRow = views->currentRow();
            if (plan && selectedRow >= 0 &&
                static_cast<std::size_t>(selectedRow) < choices.size()) {
                QPainter marker(&raster);
                marker.scale(pixels / DrawingSheet::widthMm, pixels / DrawingSheet::widthMm);
                marker.setPen(QPen(QColor("#38898B"), .3, Qt::DashLine));
                marker.setBrush(Qt::NoBrush);
                for (const auto &at : plan->views)
                    if (at.page == page->value() - 1 && at.key == choices[selectedRow].key)
                        marker.drawRect(at.area.adjusted(1, 1, -1, -1));
            }
            raster.setDevicePixelRatio(ratio);
            image->setPixmap(QPixmap::fromImage(raster));
            image->resize(fit);
            exportPdf->setEnabled(!controller.hasWork());
        } catch (const std::exception &e) {
            image->clear();
            image->adjustSize();
            exportPdf->setEnabled(false);
            status->setText(QString::fromUtf8(e.what()));
        }
    }
    void refresh() {
        const bool same =
            revision == controller.session().revision() && controller.session().document();
        const bool busy = controller.hasWork();
        for (auto *w : {static_cast<QWidget *>(base), static_cast<QWidget *>(turn),
                        static_cast<QWidget *>(reverse), static_cast<QWidget *>(breaks)})
            w->setEnabled(same && !busy);
        prepare->setEnabled(same && !busy);
        const auto drawing = controller.drawing();
        exportPdf->setEnabled(same && !busy && drawing && !settingsChanged);
        if (!same) {
            preview->setPageCount(0);
            image->clear();
            status->setText(
                QStringLiteral("Документ изменился. Откройте лист заново для новой детали."));
        } else if (controller.isPreparingDrawing()) {
            preview->setPageCount(0);
            image->clear();
            status->setText(QStringLiteral("Подготовка видов и размеров…"));
        } else if (const auto error = controller.drawingError()) {
            preview->setPageCount(0);
            image->clear();
            status->setText(QStringLiteral("Лист не сформирован: %1")
                                .arg(QString::fromStdString(error->detail)));
        } else if (drawing && !settingsChanged) {
            try {
                ensurePlan(drawing);
            } catch (const std::exception &e) {
                exportPdf->setEnabled(false);
                image->clear();
                preview->setPageCount(0);
                status->setText(QString::fromUtf8(e.what()));
                return;
            }
            const bool cut = drawing->views[0].omittedX.has_value();
            const auto coverage = DrawingSheet::coverage(*drawing, title());
            status->setText(
                QStringLiteral("%1 · A3 · %2 лист(а)%3. Размеры полок и углы: %4 из %5 "
                               "распознанных гибов. Все размеры справочные.")
                    .arg(coverage.missing.empty() ? QStringLiteral("Лист сформирован")
                                                  : QStringLiteral("Неполные размеры"))
                    .arg(DrawingSheet::pageCount(*drawing, title()))
                    .arg(cut ? QStringLiteral(
                                   " · подтверждённый однородный участок показан с разрывом")
                             : QString{})
                    .arg(coverage.dimensioned)
                    .arg(coverage.total));
            if (drawing->drawingOnlyRecognition)
                status->setText(
                    status->text() +
                    QStringLiteral(
                        " Зон на чертеже: %1; это измерения для листа, не проверка гибки.")
                        .arg(drawing->bends.size()));
            if (drawing->bendRecognitionStatus != application::BendRecognitionStatus::Completed)
                status->setText(
                    status->text() +
                    QStringLiteral(" Размеры гибов не построены: распознавание не выполнено."));
            if (drawing->flatPattern)
                status->setText(
                    status->text() +
                    (drawing->flatPattern->info.cutoutsIgnored
                         ? QStringLiteral(" Развёртка включена; влияние отверстий не учитывается.")
                         : QStringLiteral(" Теоретическая развёртка включена.")));
            else if (drawing->flatPatternError) {
                status->setText(
                    status->text() +
                    (drawing->flatPatternError->code ==
                             application::FlatPatternErrorCode::MissingMaterial
                         ? QStringLiteral(" Для развёртки выберите материал на этапе «Деталь».")
                         : QStringLiteral(" Развёртка этой формы не подтверждена и не включена.")));
                status->setToolTip(QString::fromStdString(drawing->flatPatternError->detail));
            }
            const auto detail = status->text();
            status->setToolTip(detail +
                               QStringLiteral("\nРазмеры развёртки — от края заготовки, не X "
                                              "упора. Калибровка станка не выполнена."));
            status->setText(QStringLiteral("A3 · %1 лист(а)\nРазмеры гибов: %2 из %3%4")
                                .arg(plan->pages)
                                .arg(coverage.dimensioned)
                                .arg(coverage.total)
                                .arg(drawing->flatPatternError
                                         ? QStringLiteral("\nРазвёртка не включена")
                                         : QString{}));
            render();
        } else {
            preview->setPageCount(0);
            image->clear();
            status->setText(
                busy ? QStringLiteral("Ожидание завершения текущей операции…")
                     : QStringLiteral("Данные детали изменены. Нажмите «Сформировать виды»."));
        }
    }
    void build() {
        if (!controller.session().document() || controller.session().revision() != revision)
            return;
        application::DrawingRequest request;
        if (base->currentIndex() == 1)
            request.baseFace = selected;
        request.quarterTurns = turn->currentIndex() - 1;
        request.reverseView = reverse->isChecked();
        request.allowBreaks = breaks->isChecked();
        settingsChanged = false;
        image->clear();
        started = controller.prepareDrawing(builder, request);
        refresh();
    }
};
DrawingDialog::DrawingDialog(ImportController &c,
                             std::shared_ptr<const application::BendingDrawingBuilder> builder,
                             std::optional<std::size_t> selected, QWidget *parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(c, std::move(builder), selected)) {
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                   Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::NonModal);
    setObjectName(QStringLiteral("bendingDrawingDialog"));
    setWindowTitle(QStringLiteral("Чертёж"));
    resize(1200, 720);
    auto *layout = new QHBoxLayout(this);
    auto *panel = new QWidget(this);
    panel->setMaximumWidth(310);
    auto *left = new QVBoxLayout(panel);
    auto *options = new QWidget(panel);
    auto *optionLayout = new QVBoxLayout(options);
    optionLayout->setContentsMargins(0, 0, 0, 0);
    optionLayout->setSizeConstraint(QLayout::SetMinimumSize);
    auto *optionScroll = new QScrollArea(panel);
    optionScroll->setObjectName(QStringLiteral("drawingOptionsScroll"));
    optionScroll->setFrameShape(QFrame::NoFrame);
    optionScroll->setWidgetResizable(true);
    optionScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    optionScroll->setWidget(options);
    left->addWidget(optionScroll, 1);
    auto *viewHeading = new QLabel(QStringLiteral("Виды на листе"), panel);
    optionLayout->addWidget(viewHeading);
    impl_->views = new QListWidget(panel);
    impl_->views->setObjectName(QStringLiteral("drawingViews"));
    impl_->views->setAccessibleName(QStringLiteral("Виды на листе"));
    impl_->views->setToolTip(
        QStringLiteral("Отметьте нужные виды. Щелчок по виду на листе выбирает его здесь."));
    impl_->views->setMinimumHeight(150);
    impl_->views->setMaximumHeight(240);
    optionLayout->addWidget(impl_->views);
    auto *viewForm = new QFormLayout;
    viewForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    impl_->viewScale = new QComboBox(panel);
    impl_->viewScale->setObjectName(QStringLiteral("drawingViewScale"));
    impl_->viewScale->setAccessibleName(QStringLiteral("Масштаб выбранного вида"));
    impl_->viewScale->addItem(QStringLiteral("Авто"));
    for (double scale : {10., 5., 4., 2.5, 2., 1., .5, .4, .25, .2, .1, 1. / 15, .05, .04, .025,
                         .02, 1. / 75, .01})
        impl_->viewScale->addItem(scaleLabel(scale), scale);
    impl_->viewScale->setToolTip(
        QStringLiteral("Масштаб на бумаге. Главный вид и связанные проекции имеют общий масштаб."));
    viewForm->addRow(QStringLiteral("Масштаб вида"), impl_->viewScale);
    optionLayout->addLayout(viewForm);
    impl_->flatSide = new QComboBox(panel);
    impl_->flatSide->setObjectName(QStringLiteral("drawingFlatSide"));
    impl_->flatSide->setAccessibleName(QStringLiteral("Показанная сторона развёртки"));
    impl_->flatSide->addItems({QStringLiteral("Авто: больше гибов вверх"),
                               QStringLiteral("Сторона A"), QStringLiteral("Сторона B")});
    impl_->flatSide->setToolTip(
        QStringLiteral("ВВЕРХ/ВНИЗ — от показанной стороны развёртки. Смена стороны отражает "
                       "контур и направления гибов."));
    optionLayout->addWidget(impl_->flatSide);
    impl_->sideInfo = new QLabel(panel);
    impl_->sideInfo->setObjectName(QStringLiteral("drawingFlatDirections"));
    optionLayout->addWidget(impl_->sideInfo);
    impl_->flatSide->hide();
    impl_->sideInfo->hide();
    auto *resetViews = new QPushButton(QStringLiteral("Автокомпоновка"), panel);
    resetViews->setObjectName(QStringLiteral("resetDrawingComposition"));
    resetViews->setToolTip(
        QStringLiteral("Вернуть рекомендуемые виды, масштабы и сторону развёртки."));
    optionLayout->addWidget(resetViews);
    const auto section = [&](const QString &label, const char *objectName) {
        auto *toggle = new QToolButton(panel);
        toggle->setObjectName(QLatin1String(objectName));
        toggle->setText(label);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        toggle->setCheckable(true);
        optionLayout->addWidget(toggle);
        auto *body = new QWidget(panel);
        optionLayout->addWidget(body);
        body->hide();
        connect(toggle, &QToolButton::toggled, this, [toggle, body](bool open) {
            body->setVisible(open);
            toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        });
        return body;
    };
    auto *requisites = section(QStringLiteral("Основная надпись"), "drawingTitleOptions");
    auto *form = new QFormLayout;
    requisites->setLayout(form);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto field = [&](const QString &label, const char *name, int length) {
        auto *line = new QLineEdit(panel);
        line->setObjectName(QLatin1String(name));
        line->setMaxLength(length);
        line->setAccessibleName(label);
        form->addRow(label, line);
        connect(line, &QLineEdit::textChanged, this, [this] { impl_->render(); });
        return line;
    };
    impl_->name = field(QStringLiteral("Наименование"), "drawingName", 60);
    impl_->designation =
        field(QStringLiteral("Обозначение (если задано)"), "drawingDesignation", 48);
    impl_->material = field(QStringLiteral("Материал (если известен)"), "drawingMaterial", 60);
    impl_->execution = field(QStringLiteral("Исполнение / ориентир"), "drawingExecution", 60);
    impl_->quantity = new QSpinBox(panel);
    impl_->quantity->setObjectName(QStringLiteral("drawingQuantity"));
    impl_->quantity->setRange(0, 1000000);
    impl_->quantity->setSpecialValueText(QStringLiteral("Не указано"));
    impl_->quantity->setAccessibleName(QStringLiteral("Количество в партии, штук"));
    form->addRow(QStringLiteral("Количество, шт."), impl_->quantity);
    auto *geometryOptions =
        section(QStringLiteral("Ориентация и разрывы"), "drawingGeometryOptions");
    form = new QFormLayout(geometryOptions);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    impl_->base = new QComboBox(panel);
    impl_->base->setObjectName(QStringLiteral("drawingBaseFace"));
    impl_->base->addItem(QStringLiteral("Авто: основная плоская грань"));
    if (selected)
        impl_->base->addItem(QStringLiteral("Выбранная грань №%1").arg(*selected));
    form->addRow(QStringLiteral("База главного вида"), impl_->base);
    impl_->turn = new QComboBox(panel);
    impl_->turn->setObjectName(QStringLiteral("drawingOrientation"));
    impl_->turn->addItems({QStringLiteral("Авто по геометрии"), QStringLiteral("0°"),
                           QStringLiteral("90°"), QStringLiteral("180°"), QStringLiteral("270°")});
    form->addRow(QStringLiteral("Ориентация видов"), impl_->turn);
    impl_->reverse = new QCheckBox(QStringLiteral("Обратная сторона"), panel);
    impl_->reverse->setObjectName(QStringLiteral("drawingReverseView"));
    form->addRow(impl_->reverse);
    impl_->breaks = new QCheckBox(QStringLiteral("Разрывы однородных участков"), panel);
    impl_->breaks->setObjectName(QStringLiteral("drawingAllowBreaks"));
    impl_->breaks->setChecked(true);
    form->addRow(impl_->breaks);
    impl_->prepare = new QPushButton(QStringLiteral("Сформировать виды"), panel);
    impl_->prepare->setObjectName(QStringLiteral("prepareDrawing"));
    form->addRow(impl_->prepare);
    impl_->status = new QLabel(panel);
    impl_->status->setWordWrap(true);
    impl_->status->setObjectName(QStringLiteral("drawingStatus"));
    optionLayout->addWidget(impl_->status);
    impl_->status->setToolTip(QStringLiteral(
        "Размеры справочные. Развёртка теоретическая, по выбранным материалу и K; калибровка "
        "станка не выполнена. Размеры от края развёртки не являются X упора."));
    optionLayout->addStretch();
    impl_->exportPdf = new QPushButton(QStringLiteral("Сохранить PDF…"), panel);
    impl_->exportPdf->setEnabled(false);
    impl_->exportPdf->setObjectName(QStringLiteral("exportDrawingPdf"));
    impl_->exportPdf->setMinimumHeight(40);
    left->addWidget(impl_->exportPdf);
    auto *close = new QPushButton(QStringLiteral("Закрыть"), panel);
    left->addWidget(close);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    auto *scroll = new DrawingPreviewArea(this);
    impl_->preview = scroll;
    impl_->page = scroll->pageControl();
    impl_->image = new QLabel(scroll);
    impl_->image->setObjectName(QStringLiteral("drawingPreviewImage"));
    impl_->image->setAlignment(Qt::AlignCenter);
    impl_->image->setScaledContents(true);
    scroll->setPaper(impl_->image);
    impl_->refit = new QTimer(this);
    impl_->refit->setSingleShot(true);
    impl_->refit->setInterval(80);
    connect(impl_->refit, &QTimer::timeout, this, [this] { impl_->render(false); });
    scroll->viewChanged = [this] { impl_->refit->start(); };
    scroll->paperClicked = [this](QPointF point) {
        if (!impl_->plan)
            return;
        for (const auto &at : impl_->plan->views)
            if (at.page == impl_->page->value() - 1 && at.area.contains(point)) {
                for (std::size_t i = 0; i < impl_->choices.size(); ++i)
                    if (impl_->choices[i].key == at.key) {
                        impl_->views->setCurrentRow(static_cast<int>(i));
                        return;
                    }
            }
    };
    layout->addWidget(panel);
    layout->addWidget(scroll, 1);
    // No edit signal runs before all controls and the preview exist.
    const auto utf8 = c.session().document()->metadata().sourcePath.stem().u8string();
    impl_->name->setText(QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()),
                                           static_cast<qsizetype>(utf8.size())));
    setWindowTitle(QStringLiteral("Чертёж — %1").arg(impl_->name->text()));
    connect(impl_->quantity, &QSpinBox::valueChanged, this, [this] { impl_->render(); });
    connect(impl_->views, &QListWidget::currentRowChanged, this,
            [this](int row) { impl_->selectView(row); });
    connect(impl_->views, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const auto row = impl_->views->row(item);
        if (row < 0 || static_cast<std::size_t>(row) >= impl_->choices.size())
            return;
        const bool visible = item->checkState() == Qt::Checked;
        if (impl_->choices[row].visible == visible)
            return;
        impl_->choices[row].visible = visible;
        impl_->plan.reset();
        impl_->refresh();
    });
    connect(impl_->viewScale, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto row = impl_->views->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= impl_->choices.size())
            return;
        const auto scale = index
                               ? std::optional<double>{impl_->viewScale->itemData(index).toDouble()}
                               : std::nullopt;
        const auto key = impl_->choices[row].key;
        for (auto &choice : impl_->choices)
            if (choice.key == key ||
                (key.kind == DrawingViewKind::Projection && choice.key.kind == key.kind))
                choice.scale = scale;
        impl_->plan.reset();
        impl_->refresh();
    });
    connect(impl_->flatSide, &QComboBox::currentIndexChanged, this, [this] {
        impl_->plan.reset();
        impl_->refresh();
    });
    connect(resetViews, &QPushButton::clicked, this, [this] {
        impl_->compositionSource.reset();
        impl_->plan.reset();
        impl_->refresh();
    });
    connect(impl_->page, &QSpinBox::valueChanged, this, [this] {
        impl_->render();
        impl_->preview->verticalScrollBar()->setValue(0);
    });
    for (auto *combo : {impl_->base, impl_->turn})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { impl_->invalidate(); });
    for (auto *check : {impl_->reverse, impl_->breaks})
        connect(check, &QCheckBox::toggled, this, [this] { impl_->invalidate(); });
    connect(impl_->prepare, &QPushButton::clicked, this, [this] { impl_->build(); });
    connect(&c, &ImportController::stateChanged, this, [this] {
        if (!impl_->controller.session().document() ||
            impl_->revision != impl_->controller.session().revision()) {
            reject();
            return;
        }
        impl_->refresh();
    });
    connect(impl_->exportPdf, &QPushButton::clicked, this, [this] {
        if (!impl_->exportPdf->isEnabled())
            return;
        const auto drawing = impl_->controller.drawing();
        const auto title = impl_->title();
        const auto utf8 = impl_->controller.session().document()->metadata().sourcePath.u8string();
        const auto suggested = suggestedDrawingPdfPath(QString::fromUtf8(
            reinterpret_cast<const char *>(utf8.data()), static_cast<qsizetype>(utf8.size())));
        auto *file =
            new QFileDialog(this, QStringLiteral("Сохранить чертёж"),
                            QFileInfo(suggested).absolutePath(), QStringLiteral("PDF (*.pdf)"));
        file->setObjectName(QStringLiteral("drawingPdfFileDialog"));
        file->setAttribute(Qt::WA_DeleteOnClose);
        file->setAcceptMode(QFileDialog::AcceptSave);
        file->setFileMode(QFileDialog::AnyFile);
        file->setDefaultSuffix(QStringLiteral("pdf"));
        file->selectFile(QFileInfo(suggested).fileName());
        file->setWindowModality(Qt::WindowModal);
        connect(
            file, &QFileDialog::fileSelected, this, [this, drawing, title](const QString &path) {
                if (path.isEmpty() || !impl_->exportPdf->isEnabled() ||
                    drawing != impl_->controller.drawing())
                    return;
                const auto destination = path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
                                             ? path
                                             : path + QStringLiteral(".pdf");
                // A user-supplied different suffix is normalized after QFileDialog's own check.
                if (destination != path && QFileInfo::exists(destination) &&
                    QMessageBox::question(this, QStringLiteral("Заменить файл?"),
                                          QStringLiteral("Файл «%1» уже существует. Заменить его?")
                                              .arg(QFileInfo(destination).fileName()),
                                          QMessageBox::Yes | QMessageBox::No,
                                          QMessageBox::No) != QMessageBox::Yes)
                    return;
                if (!impl_->exportPdf->isEnabled() || drawing != impl_->controller.drawing())
                    return;
                const auto error = DrawingSheet::writePdf(destination, *drawing, title);
                if (!error.isEmpty())
                    QMessageBox::warning(this, QStringLiteral("PDF не сохранён"), error);
                else {
                    rememberDrawingPdfDirectory(destination);
                    impl_->status->setText(QStringLiteral("PDF сохранён: %1").arg(destination));
                }
            });
        file->open();
    });
    QTimer::singleShot(0, this, [this] { impl_->build(); });
}
void DrawingDialog::done(int result) {
    impl_->refit->stop();
    const bool cancel = impl_->started && impl_->controller.isPreparingDrawing() &&
                        impl_->controller.session().revision() == impl_->revision;
    impl_->started = false;
    if (cancel)
        impl_->controller.cancelDrawing();
    QDialog::done(result);
}
DrawingDialog::~DrawingDialog() = default;
} // namespace bending::ui
