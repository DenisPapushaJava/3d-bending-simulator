#include "DrawingDialog.h"
#include "bending/ui/DrawingSheet.h"
#include "bending/ui/ImportController.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
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
    QComboBox *layoutMode;
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
                layoutMode->currentIndex() == 0 ? DrawingLayout::Classical
                                                : DrawingLayout::Profiles};
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
    auto *form = new QFormLayout;
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
    impl_->layoutMode = new QComboBox(panel);
    impl_->layoutMode->setObjectName(QStringLiteral("drawingLayout"));
    impl_->layoutMode->addItems({QStringLiteral("Виды и сечения"), QStringLiteral("Профили")});
    form->addRow(QStringLiteral("Представление"), impl_->layoutMode);
    impl_->base = new QComboBox(panel);
    impl_->base->setObjectName(QStringLiteral("drawingBaseFace"));
    impl_->base->addItem(QStringLiteral("Авто: основная плоская грань"));
    if (selected)
        impl_->base->addItem(QStringLiteral("Выбранная грань №%1").arg(*selected));
    form->addRow(QStringLiteral("Базовая сторона A"), impl_->base);
    impl_->turn = new QComboBox(panel);
    impl_->turn->setObjectName(QStringLiteral("drawingOrientation"));
    impl_->turn->addItems({QStringLiteral("Авто по геометрии"), QStringLiteral("0°"),
                           QStringLiteral("90°"), QStringLiteral("180°"), QStringLiteral("270°")});
    form->addRow(QStringLiteral("Ориентация видов"), impl_->turn);
    impl_->reverse = new QCheckBox(QStringLiteral("Смотреть с обратной стороны A"), panel);
    impl_->reverse->setObjectName(QStringLiteral("drawingReverseView"));
    form->addRow(impl_->reverse);
    impl_->breaks = new QCheckBox(QStringLiteral("Разрывы однородных участков"), panel);
    impl_->breaks->setObjectName(QStringLiteral("drawingAllowBreaks"));
    impl_->breaks->setChecked(true);
    form->addRow(impl_->breaks);
    optionLayout->addLayout(form);
    impl_->prepare = new QPushButton(QStringLiteral("Сформировать виды"), panel);
    impl_->prepare->setObjectName(QStringLiteral("prepareDrawing"));
    optionLayout->addWidget(impl_->prepare);
    impl_->status = new QLabel(panel);
    impl_->status->setWordWrap(true);
    impl_->status->setObjectName(QStringLiteral("drawingStatus"));
    optionLayout->addWidget(impl_->status);
    auto *scope = new QLabel(
        QStringLiteral("База и виды выбираются по форме детали. Для другой базы можно выбрать "
                       "плоскую грань в 3D перед открытием листа.\n\nЭто справочный лист готовой "
                       "формы. При выбранном материале добавляется теоретическая развёртка. Её "
                       "размеры не являются X упора; калибровка станка не выполнена."),
        panel);
    scope->setWordWrap(true);
    optionLayout->addWidget(scope);
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
    layout->addWidget(panel);
    layout->addWidget(scroll, 1);
    // No edit signal runs before all controls and the preview exist.
    const auto utf8 = c.session().document()->metadata().sourcePath.stem().u8string();
    impl_->name->setText(QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()),
                                           static_cast<qsizetype>(utf8.size())));
    setWindowTitle(QStringLiteral("Чертёж — %1").arg(impl_->name->text()));
    connect(impl_->quantity, &QSpinBox::valueChanged, this, [this] { impl_->render(); });
    connect(impl_->layoutMode, &QComboBox::currentIndexChanged, this, [this] { impl_->refresh(); });
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
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить лист гибки"),
                                                       QStringLiteral("bending-sheet.pdf"),
                                                       QStringLiteral("PDF (*.pdf)"));
        if (path.isEmpty())
            return;
        if (!impl_->exportPdf->isEnabled() || drawing != impl_->controller.drawing())
            return;
        const auto destination = path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
                                     ? path
                                     : path + QStringLiteral(".pdf");
        const auto error = DrawingSheet::writePdf(destination, *drawing, title);
        if (!error.isEmpty())
            QMessageBox::warning(this, QStringLiteral("PDF не сохранён"), error);
        else
            impl_->status->setText(QStringLiteral("PDF сохранён: %1").arg(destination));
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
