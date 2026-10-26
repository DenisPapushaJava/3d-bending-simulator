#include "DrawingDialog.h"
#include "bending/ui/DrawingSheet.h"
#include "bending/ui/ImportController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace bending::ui {
class DrawingDialog::Impl {
  public:
    DrawingDialog *dialog;
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
        return {name->text(), designation->text(), material->text(), execution->text(),
                quantity->value() ? std::optional<int>{quantity->value()} : std::nullopt};
    }
    void invalidate() {
        settingsChanged = true;
        exportPdf->setEnabled(false);
        status->setText(QStringLiteral("Ориентация изменена. Нажмите «Сформировать виды»."));
        image->clear();
    }
    void render() {
        const auto drawing = controller.drawing();
        if (!drawing || settingsChanged || controller.session().revision() != revision)
            return;
        page->setMaximum(DrawingSheet::pageCount(*drawing));
        try {
            image->setPixmap(QPixmap::fromImage(DrawingSheet::preview(
                *drawing, title(), page->value() - 1, std::max(600, dialog->width() - 370))));
            image->adjustSize();
        } catch (const std::exception &e) {
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
            image->clear();
            status->setText(
                QStringLiteral("Документ изменился. Откройте лист заново для новой детали."));
        } else if (controller.isPreparingDrawing()) {
            status->setText(QStringLiteral("Подготовка видов и размеров…"));
        } else if (const auto error = controller.drawingError()) {
            image->clear();
            status->setText(QStringLiteral("Лист не сформирован: %1")
                                .arg(QString::fromStdString(error->detail)));
        } else if (drawing && !settingsChanged) {
            const bool cut = drawing->views[0].omittedX.has_value();
            status->setText(
                QStringLiteral("Готово · A3 · %1 лист(а)%2. Все размеры справочные; подписи и "
                               "допуски не назначены.")
                    .arg(DrawingSheet::pageCount(*drawing))
                    .arg(cut ? QStringLiteral(
                                   " · подтверждённый однородный участок показан с разрывом")
                             : QString{}));
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
    impl_->page = new QSpinBox(panel);
    impl_->page->setObjectName(QStringLiteral("drawingPage"));
    impl_->page->setRange(1, 1);
    form->addRow(QStringLiteral("Лист предпросмотра"), impl_->page);
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
                       "формы. Развёртка, припуски, усилие и задний упор не рассчитываются."),
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
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("drawingPreviewScroll"));
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
    connect(impl_->page, &QSpinBox::valueChanged, this, [this] { impl_->render(); });
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
