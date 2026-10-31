#include "ToolLibraryDialog.h"
#include "ToolDrawingWidget.h"
#include "ToolImportDialog.h"
#include "ToolStockDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <numbers>

namespace bending::ui {
namespace {
QString text(const std::string &s) {
    return QString::fromStdString(s);
}
QIcon icon(const core::ToolPreset &tool) {
    QPixmap image(42, 56);
    image.fill(Qt::transparent);
    if (!tool.drawing)
        return {};
    const auto path = toolDrawingPath(*tool.drawing);
    const auto bounds = path.boundingRect();
    if (bounds.isEmpty())
        return {};
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(21, 28);
    const double scale = std::min(34 / bounds.width(), 48 / bounds.height());
    p.scale(scale, scale);
    p.translate(-bounds.center());
    QPen pen(QColor("#087F83"), 1);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(QColor("#D8E8EA"));
    p.drawPath(path);
    return QIcon(image);
}
} // namespace
ToolLibraryDialog::ToolLibraryDialog(
    application::ToolLibrary &library, ImportController &controller,
    std::shared_ptr<const application::ToolProfileImporter> importer,
    std::function<QString(const core::ToolPreset &, std::size_t)> issue, QWidget *parent,
    std::optional<core::ToolKind> kind, std::string selectedId, std::size_t initialGroove)
    : QDialog(parent), library_(library), controller_(controller), importer_(std::move(importer)),
      issue_(std::move(issue)), selectedId_(std::move(selectedId)) {
    grooveChoices_[selectedId_] = static_cast<int>(initialGroove);
    setObjectName("toolLibraryDialog");
    setWindowTitle(QStringLiteral("Библиотека инструмента"));
    resize(1000, 780);
    setMinimumSize(760, 560);
    setAcceptDrops(true);
    auto *root = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    auto *heading = new QLabel(QStringLiteral("Библиотека инструмента"), this);
    auto font = heading->font();
    font.setPointSize(font.pointSize() + 2);
    font.setBold(true);
    heading->setFont(font);
    top->addWidget(heading);
    top->addStretch();
    auto *add = new QPushButton(QStringLiteral("+ Добавить DXF"), this);
    add->setObjectName("libraryAddDxf");
    add->setEnabled(static_cast<bool>(importer_));
    top->addWidget(add);
    auto *more = new QToolButton(this);
    more->setText(QStringLiteral("Ещё"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    auto *importCatalog = menu->addAction(QStringLiteral("Добавить каталог мастерской…"));
    more->setMenu(menu);
    top->addWidget(more);
    root->addLayout(top);
    auto *split = new QSplitter(this);
    root->addWidget(split, 1);
    auto *side = new QWidget(split);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 8, 0);
    search_ = new QLineEdit(side);
    search_->setPlaceholderText(QStringLiteral("Найти инструмент"));
    search_->setObjectName("librarySearch");
    sideLayout->addWidget(search_);
    filter_ = new QComboBox(side);
    filter_->setObjectName("libraryFilter");
    filter_->addItems(
        {QStringLiteral("Все инструменты"), QStringLiteral("Пуансоны"), QStringLiteral("Матрицы")});
    if (kind)
        filter_->setCurrentIndex(*kind == core::ToolKind::Punch ? 1 : 2);
    sideLayout->addWidget(filter_);
    archive_ = new QCheckBox(QStringLiteral("Удалённые"), side);
    archive_->setObjectName("libraryArchive");
    sideLayout->addWidget(archive_);
    list_ = new QListWidget(side);
    list_->setObjectName("libraryTools");
    list_->setIconSize(QSize(42, 56));
    list_->setTextElideMode(Qt::ElideRight);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumWidth(170);
    sideLayout->addWidget(list_, 1);
    pages_ = new QStackedWidget(split);
    auto *scroll = new QScrollArea(pages_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    card_ = new QWidget(scroll);
    auto *body = new QVBoxLayout(card_);
    name_ = new QLineEdit(card_);
    name_->setMaxLength(60);
    name_->setObjectName("libraryToolName");
    name_->setAccessibleName(QStringLiteral("Название инструмента"));
    body->addWidget(name_);
    auto *profile = new QHBoxLayout;
    drawing_ = new ToolDrawingWidget({}, card_);
    drawing_->setMinimumSize(160, 160);
    drawing_->setFixedHeight(180);
    profile->addWidget(drawing_, 1);
    auto *profileActions = new QVBoxLayout;
    groove_ = new QComboBox(card_);
    groove_->setObjectName("libraryGroove");
    groove_->setAccessibleName(QStringLiteral("Рабочий ручей"));
    profileActions->addWidget(groove_);
    parameters_ = new QLabel(card_);
    parameters_->setObjectName("libraryWorkingParameters");
    parameters_->setWordWrap(true);
    parameters_->setTextFormat(Qt::PlainText);
    profileActions->addWidget(parameters_);
    replace_ = new QPushButton(QStringLiteral("Заменить DXF…"), card_);
    replace_->setObjectName("libraryReplaceDxf");
    profileActions->addWidget(replace_);
    remove_ = new QPushButton(QStringLiteral("Удалить"), card_);
    remove_->setObjectName("libraryDelete");
    profileActions->addWidget(remove_);
    profileActions->addStretch();
    profile->addLayout(profileActions);
    body->addLayout(profile);
    auto *details = new QWidget(card_);
    auto *form = new QFormLayout(details);
    manufacturer_ = new QLineEdit(details);
    manufacturer_->setMaxLength(60);
    manufacturer_->setObjectName("libraryManufacturer");
    form->addRow(QStringLiteral("Производитель:"), manufacturer_);
    length_ = new QDoubleSpinBox(details);
    length_->setRange(0, 100000);
    length_->setDecimals(3);
    length_->setKeyboardTracking(false);
    length_->setSuffix(QStringLiteral(" мм"));
    length_->setObjectName("libraryNominalLength");
    form->addRow(QStringLiteral("Цельный инструмент:"), length_);
    source_ = new QLabel(details);
    source_->setWordWrap(true);
    source_->setTextFormat(Qt::PlainText);
    form->addRow(source_);
    details->hide();
    auto *detailToggle = new QToolButton(card_);
    detailToggle->setText(QStringLiteral("Данные профиля"));
    detailToggle->setCheckable(true);
    detailToggle->setArrowType(Qt::RightArrow);
    detailToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    body->addWidget(detailToggle);
    body->addWidget(details);
    connect(detailToggle, &QToolButton::toggled, this, [details, detailToggle](bool on) {
        details->setVisible(on);
        detailToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    body->addWidget(new QLabel(QStringLiteral("Длины и количество секций"), card_));
    stockHost_ = new QWidget(card_);
    stockLayout_ = new QVBoxLayout(stockHost_);
    stockLayout_->setContentsMargins(0, 0, 0, 0);
    body->addWidget(stockHost_, 1);
    scroll->setWidget(card_);
    pages_->addWidget(scroll);
    split->setSizes({220, 740});
    split->setStretchFactor(1, 1);
    status_ = new QLabel(this);
    status_->setObjectName("libraryStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    root->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("Закрыть"));
    use_ = buttons->addButton(QStringLiteral("Использовать"), QDialogButtonBox::ActionRole);
    use_->setObjectName("libraryUse");
    root->addWidget(buttons);
    connect(add, &QPushButton::clicked, this, [this] { startImport(); });
    connect(importCatalog, &QAction::triggered, this, [this] {
        if (saveCurrent() && onImportCatalog) {
            onImportCatalog();
            refreshList();
            loadCard();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &ToolLibraryDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        if (saveCurrent()) {
            refreshList();
            loadCard();
        }
    });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this] {
        if (saveCurrent()) {
            refreshList();
            loadCard();
        }
    });
    connect(archive_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!saveCurrent()) {
            QSignalBlocker b(archive_);
            archive_->setChecked(showingArchive_);
            return;
        }
        showingArchive_ = checked;
        selectedId_.clear();
        refreshList();
        loadCard();
    });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { select(); });
    connect(groove_, &QComboBox::currentIndexChanged, this, [this] { updateGroove(); });
    connect(replace_, &QPushButton::clicked, this, [this] { startImport(true); });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (!saveCurrent() || !selected())
            return;
        auto error =
            showingArchive_ ? library_.restore(selectedId_) : library_.archive(selectedId_);
        if (!error.empty()) {
            status_->setText(text(error));
            return;
        }
        selectedId_.clear();
        changed();
        refreshList();
        loadCard();
        status_->setText(
            showingArchive_
                ? QStringLiteral("Инструмент восстановлен.")
                : QStringLiteral(
                      "Инструмент перемещён в «Удалённые». Проекты сохраняют свои снимки."));
    });
    auto *purge = menu->addAction(QStringLiteral("Удалить выбранный из архива навсегда…"));
    purge->setEnabled(false);
    connect(menu, &QMenu::aboutToShow, this,
            [this, purge] { purge->setEnabled(showingArchive_ && selected().has_value()); });
    connect(purge, &QAction::triggered, this, [this] {
        if (!showingArchive_ || !selected())
            return;
        if (QMessageBox::warning(
                this, QStringLiteral("Удалить навсегда?"),
                QStringLiteral(
                    "Удалить сохранённый профиль и наличие из архива? Снимки проектов сохранятся."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        const auto error = library_.purge(selectedId_);
        if (!error.empty()) {
            status_->setText(text(error));
            return;
        }
        selectedId_.clear();
        changed();
        refreshList();
        loadCard();
    });
    connect(use_, &QPushButton::clicked, this, [this] {
        if (!saveCurrent())
            return;
        const auto t = selected();
        if (!t || showingArchive_)
            return;
        const auto g = static_cast<std::size_t>(std::max(0, groove_->currentIndex()));
        const auto error = issue_(*t, g);
        if (!error.isEmpty()) {
            status_->setText(error);
            return;
        }
        choice_ = std::pair{*t, g};
        QDialog::accept();
    });
    for (auto *edit : {name_, manufacturer_})
        connect(edit, &QLineEdit::textChanged, this, [this] {
            if (!loading_ && stock_)
                stock_->markDirty();
        });
    connect(length_, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!loading_ && stock_)
            stock_->markDirty();
    });
    refreshList();
    loadCard();
    if (!library_.error().empty())
        status_->setText(text(library_.error()));
}
std::optional<core::ToolPreset> ToolLibraryDialog::selected() const {
    const auto &tools = showingArchive_ ? library_.data().archived : library_.data().tools;
    const auto i = std::find_if(tools.begin(), tools.end(),
                                [&](const auto &t) { return t.id == selectedId_; });
    return i == tools.end() ? std::nullopt : std::optional{*i};
}
void ToolLibraryDialog::refreshList() {
    QSignalBlocker block(list_);
    list_->clear();
    const auto &tools = showingArchive_ ? library_.data().archived : library_.data().tools;
    for (const auto &t : tools) {
        if (filter_->currentIndex() &&
            (filter_->currentIndex() == 1) != (t.kind == core::ToolKind::Punch))
            continue;
        if (!text(t.code + " " + t.manufacturer).contains(search_->text(), Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(icon(t), text(t.code), list_);
        item->setData(Qt::UserRole, text(t.id));
        item->setToolTip(text(t.code) + QStringLiteral("\n") + text(t.manufacturer) +
                         QStringLiteral(" · ") +
                         (t.kind == core::ToolKind::Punch ? QStringLiteral("Пуансон")
                                                          : QStringLiteral("Матрица")));
        if (t.id == selectedId_)
            list_->setCurrentItem(item);
    }
    if (!list_->currentItem() && list_->count())
        list_->setCurrentRow(0);
    selectedId_ = list_->currentItem()
                      ? list_->currentItem()->data(Qt::UserRole).toString().toStdString()
                      : std::string{};
}
bool ToolLibraryDialog::saveCurrent() {
    return !stock_ || showingArchive_ || stock_->apply();
}
void ToolLibraryDialog::select() {
    if (loading_)
        return;
    const auto requested = list_->currentItem()
                               ? list_->currentItem()->data(Qt::UserRole).toString().toStdString()
                               : std::string{};
    if (requested == selectedId_)
        return;
    if (!saveCurrent()) {
        refreshList();
        return;
    }
    selectedId_ = requested;
    refreshList();
    loadCard();
}
void ToolLibraryDialog::loadCard() {
    loading_ = true;
    if (import_) {
        disconnect(import_, nullptr, this, nullptr);
        import_->reject();
        pages_->removeWidget(import_);
        delete import_;
        import_ = nullptr;
    }
    pages_->setCurrentIndex(0);
    delete stock_;
    stock_ = nullptr;
    const auto t = selected();
    card_->setEnabled(t.has_value());
    use_->setEnabled(false);
    if (!t) {
        name_->clear();
        drawing_->setDrawing({});
        status_->setText(QStringLiteral("Выберите профиль или добавьте DXF."));
        loading_ = false;
        return;
    }
    name_->setText(text(t->code));
    manufacturer_->setText(text(t->manufacturer));
    length_->setValue(t->lengthMm);
    name_->setReadOnly(showingArchive_);
    manufacturer_->setReadOnly(showingArchive_);
    length_->setEnabled(!showingArchive_);
    source_->setText(t->drawing ? text(t->drawing->sourceFile) + QStringLiteral(" · мм")
                                : QStringLiteral("Нет сечения DXF"));
    remove_->setText(showingArchive_ ? QStringLiteral("Восстановить") : QStringLiteral("Удалить"));
    replace_->setEnabled(!showingArchive_ && static_cast<bool>(importer_));
    groove_->clear();
    for (std::size_t i = 0; i < t->openingsMm.size(); ++i) {
        const auto g = core::dieGroove(*t, i);
        groove_->addItem(QStringLiteral("V %1 мм · %2°")
                             .arg(t->openingsMm[i], 0, 'g', 5)
                             .arg(g ? g->angleRadians * 180 / std::numbers::pi : 0, 0, 'g', 5));
    }
    if (groove_->count())
        groove_->setCurrentIndex(std::clamp(grooveChoices_[t->id], 0, groove_->count() - 1));
    groove_->setVisible(t->kind == core::ToolKind::Die);
    stock_ = new ToolStockDialog(
        {*t},
        [this](core::ToolPreset tool) {
            tool.code = name_->text().trimmed().toStdString();
            tool.manufacturer = manufacturer_->text().trimmed().toStdString();
            tool.lengthMm = length_->value();
            const auto previous = selected();
            const auto error = library_.upsert(std::move(tool));
            if (error.empty() && selected() != previous) {
                changed();
                QTimer::singleShot(0, this, [this] {
                    if (!import_) {
                        refreshList();
                        loadCard();
                    }
                });
            }
            return text(error);
        },
        stockHost_, true);
    stock_->setEnabled(!showingArchive_);
    stock_->onColorsChanged = [this] {
        if (onColorsChanged)
            onColorsChanged();
    };
    stockLayout_->addWidget(stock_);
    connect(stock_, &QDialog::rejected, this, &ToolLibraryDialog::reject);
    loading_ = false;
    updateGroove();
}
void ToolLibraryDialog::updateGroove() {
    if (loading_)
        return;
    const auto t = selected();
    if (!t)
        return;
    const auto g = static_cast<std::size_t>(std::max(0, groove_->currentIndex()));
    grooveChoices_[t->id] = static_cast<int>(g);
    const auto d = t->kind == core::ToolKind::Die ? core::activeDieDrawing(*t, g) : t->drawing;
    drawing_->setDrawing(d.value_or(core::ToolDrawing{}));
    parameters_->setVisible(t->kind == core::ToolKind::Punch && t->noseRadiusMm.has_value());
    parameters_->setText(t->noseRadiusMm
                             ? QStringLiteral("α %1° · R %2 мм")
                                   .arg(t->angleRadians * 180 / std::numbers::pi, 0, 'g', 5)
                                   .arg(*t->noseRadiusMm, 0, 'g', 5)
                             : QString{});
    const auto error = issue_(*t, g);
    status_->setText(error);
    use_->setEnabled(!showingArchive_ && error.isEmpty());
}
void ToolLibraryDialog::changed() {
    if (onLibraryChanged)
        onLibraryChanged();
}
void ToolLibraryDialog::startImport(bool replace) {
    if (!importer_ || !saveCurrent())
        return;
    if (import_)
        return;
    const auto t = selected();
    if (replace && !t)
        return;
    import_ = new ToolImportDialog(
        controller_, importer_, library_.data().tools,
        [this](core::ToolPreset tool) {
            const auto id = tool.id;
            const auto error = library_.upsert(std::move(tool));
            if (error.empty()) {
                selectedId_ = id;
                showingArchive_ = false;
                QSignalBlocker b(archive_);
                archive_->setChecked(false);
                QSignalBlocker f(filter_);
                filter_->setCurrentIndex(0);
                QSignalBlocker s(search_);
                search_->clear();
                changed();
            }
            return text(error);
        },
        pages_, t ? std::optional{t->kind} : std::nullopt);
    import_->embed(replace ? std::optional{t->id} : std::nullopt);
    pages_->addWidget(import_);
    pages_->setCurrentWidget(import_);
    use_->setEnabled(false);
    connect(import_, &QDialog::finished, this, [this] {
        QTimer::singleShot(0, this, [this] {
            refreshList();
            loadCard();
        });
    });
    import_->show();
}
void ToolLibraryDialog::accept() {
    if (import_) {
        import_->accept();
        return;
    }
    if (saveCurrent())
        QDialog::accept();
}
void ToolLibraryDialog::reject() {
    if (import_) {
        disconnect(import_, nullptr, this, nullptr);
        import_->reject();
        pages_->removeWidget(import_);
        delete import_;
        import_ = nullptr;
    }
    if (saveCurrent() ||
        QMessageBox::warning(
            this, QStringLiteral("Изменения не сохранены"),
            QStringLiteral("Не удалось сохранить изменения библиотеки. Закрыть без сохранения?"),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel) == QMessageBox::Discard)
        QDialog::reject();
}
void ToolLibraryDialog::loadFile(const QString &path) {
    if (!import_)
        startImport();
    if (import_)
        import_->loadFile(path);
}
namespace {
QString dxf(const QMimeData *data) {
    if (!data || !data->hasUrls() || data->urls().size() != 1 ||
        !data->urls().front().isLocalFile())
        return {};
    const auto path = data->urls().front().toLocalFile();
    return QFileInfo(path).suffix().compare(QStringLiteral("dxf"), Qt::CaseInsensitive) == 0
               ? path
               : QString{};
}
} // namespace
void ToolLibraryDialog::dragEnterEvent(QDragEnterEvent *e) {
    if (!dxf(e->mimeData()).isEmpty()) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else
        e->ignore();
}
void ToolLibraryDialog::dragMoveEvent(QDragMoveEvent *e) {
    if (!dxf(e->mimeData()).isEmpty()) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else
        e->ignore();
}
void ToolLibraryDialog::dropEvent(QDropEvent *e) {
    const auto path = dxf(e->mimeData());
    if (path.isEmpty()) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    loadFile(path);
}
} // namespace bending::ui
