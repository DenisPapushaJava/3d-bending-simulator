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
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace bending::ui {
namespace {
class DrawingPreviewArea final : public QScrollArea {
  public:
    explicit DrawingPreviewArea(QWidget *parent) : QScrollArea(parent) {
        setObjectName(QStringLiteral("drawingPreviewScroll"));
        setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        navigation_ = new QFrame(this);
        navigation_->setObjectName(QStringLiteral("drawingPageNavigation"));
        navigation_->setStyleSheet(QStringLiteral(
            "QFrame#drawingPageNavigation{background:#FFFFFF;border:1px solid #D4DCE2;"
            "border-radius:10px;}"
            "QToolButton{background:transparent;color:#334155;border:0;border-radius:6px;}"
            "QToolButton:hover{background:#EEF4F5;}"
            "QToolButton:focus{border:2px solid #087F83;}"
            "QToolButton:disabled{color:#B6BEC7;}"
            "QSpinBox{background:transparent;color:#263542;border:0;padding:2px;}"
            "QSpinBox:focus{background:#E5F3F3;border-radius:4px;}"));
        auto *row = new QHBoxLayout(navigation_);
        row->setContentsMargins(6, 4, 6, 4);
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
        viewport()->installEventFilter(this);
        setPageCount(0);
        setMinimumWidth(navigation_->sizeHint().width() + 32);
    }
    QSpinBox *pageControl() const {
        return page_;
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
        navigation_->adjustSize();
        // Reserve a footer inside the preview frame, outside the paper viewport.
        setViewportMargins(0, 0, 0, count > 1 ? navigation_->height() + 16 : 0);
        positionNavigation();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QScrollArea::resizeEvent(event);
        positionNavigation();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == viewport() && event->type() == QEvent::Resize)
            positionNavigation();
        return QScrollArea::eventFilter(object, event);
    }

  private:
    QFrame *navigation_ = nullptr;
    QSpinBox *page_ = nullptr;
    QAction *previous_ = nullptr;
    QAction *next_ = nullptr;
    void positionNavigation() {
        if (!navigation_)
            return;
        const auto area = viewport()->geometry();
        navigation_->move(area.left() + (area.width() - navigation_->width()) / 2,
                          area.bottom() + 9);
        navigation_->raise();
    }
};
} // namespace
class DrawingDialog::Impl {
  public:
    DrawingDialog *dialog;
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
    QLabel *image;
    QLabel *status;
    QPushButton *prepare;
    QPushButton *exportPdf;
    std::optional<std::size_t> selected;
    bool settingsChanged = false;
    bool started = false;
    Impl(DrawingDialog *d, ImportController &c,
         std::shared_ptr<const application::BendingDrawingBuilder> b,
         std::optional<std::size_t> face)
        : dialog(d), controller(c), builder(std::move(b)), revision(c.session().revision()),
          selected(face) {}
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
    void render() {
        const auto drawing = controller.drawing();
        if (!drawing || settingsChanged || controller.session().revision() != revision)
            return;
        try {
            const auto previousPage = page->value();
            preview->setPageCount(DrawingSheet::pageCount(*drawing, title()));
            if (page->value() != previousPage)
                preview->verticalScrollBar()->setValue(0);
            image->setPixmap(QPixmap::fromImage(DrawingSheet::preview(
                *drawing, title(), page->value() - 1, std::max(600, dialog->width() - 370))));
            image->adjustSize();
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
                status->setText(status->text() +
                                QStringLiteral(" Теоретическая развёртка включена."));
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
        }
    }
    void build() {
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
    : QDialog(parent), impl_(std::make_unique<Impl>(this, c, std::move(builder), selected)) {
    setObjectName(QStringLiteral("bendingDrawingDialog"));
    setWindowTitle(QStringLiteral("Лист гибки из STEP"));
    resize(1200, 720);
    auto *layout = new QHBoxLayout(this);
    auto *panel = new QWidget(this);
    panel->setMaximumWidth(310);
    auto *left = new QVBoxLayout(panel);
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
    impl_->layoutMode->addItems(
        {QStringLiteral("Чертёж: виды и сечения"), QStringLiteral("Профили для ввода")});
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
    left->addLayout(form);
    impl_->prepare = new QPushButton(QStringLiteral("Сформировать виды"), panel);
    impl_->prepare->setObjectName(QStringLiteral("prepareDrawing"));
    left->addWidget(impl_->prepare);
    impl_->status = new QLabel(panel);
    impl_->status->setWordWrap(true);
    impl_->status->setObjectName(QStringLiteral("drawingStatus"));
    left->addWidget(impl_->status);
    auto *scope = new QLabel(
        QStringLiteral("База и виды выбираются по форме детали. Для другой базы можно выбрать "
                       "плоскую грань в 3D перед открытием листа.\n\nЭто справочный лист готовой "
                       "формы. При выбранном материале добавляется теоретическая развёртка. Её "
                       "размеры не являются X упора; калибровка станка не выполнена."),
        panel);
    scope->setWordWrap(true);
    left->addWidget(scope);
    left->addStretch();
    impl_->exportPdf = new QPushButton(QStringLiteral("Сохранить PDF…"), panel);
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
    scroll->setWidget(impl_->image);
    layout->addWidget(panel);
    layout->addWidget(scroll, 1);
    // No edit signal runs before all controls and the preview exist.
    const auto utf8 = c.session().document()->metadata().sourcePath.stem().u8string();
    impl_->name->setText(QString::fromUtf8(reinterpret_cast<const char *>(utf8.data()),
                                           static_cast<qsizetype>(utf8.size())));
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
    connect(&c, &ImportController::stateChanged, this, [this] { impl_->refresh(); });
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
DrawingDialog::~DrawingDialog() {
    if (impl_->controller.isPreparingDrawing())
        impl_->controller.cancelDrawing();
}
} // namespace bending::ui
