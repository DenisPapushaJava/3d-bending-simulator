#include "ToolPickerPopup.h"
#include "ToolContourButton.h"
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <numeric>
namespace bending::ui {
ToolPickerPopup::ToolPickerPopup(
    core::ToolCatalog tools, core::ToolKind kind, const std::optional<core::ToolPreset> &current,
    std::size_t groove, std::function<QString(const core::ToolPreset &, std::size_t)> issue,
    bool canImport, QWidget *anchor, bool punchReversed)
    : QDialog(anchor, Qt::Popup), tools_(std::move(tools)) {
    setObjectName("toolPickerPopup");
    setWindowTitle(kind == core::ToolKind::Punch ? QStringLiteral("Выбрать пуансон")
                                                 : QStringLiteral("Выбрать матрицу"));
    auto *layout = new QVBoxLayout(this);
    auto *screen = QGuiApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()));
    const auto available = (screen ? screen : anchor->screen())->availableGeometry();
    const auto popupWidth = std::min(620, available.width() - 24);
    const int dieColumns = std::clamp((popupWidth - 64) / 124, 1, 4);
    const int punchColumns = std::min(3, dieColumns);
    auto *search = new QLineEdit(this);
    search->setObjectName("toolPickerSearch");
    search->setPlaceholderText(QStringLiteral("Артикул или V"));
    search->setAccessibleName(QStringLiteral("Поиск инструмента"));
    layout->addWidget(search);
    if (current && std::find(tools_.begin(), tools_.end(), *current) == tools_.end()) {
        auto *saved = new QLabel(
            QStringLiteral("В детали выбран %1 из сохранённых данных. Ниже — библиотека.")
                .arg(QString::fromStdString(current->code)),
            this);
        saved->setObjectName("toolPickerCapturedChoice");
        saved->setTextFormat(Qt::PlainText);
        saved->setWordWrap(true);
        layout->addWidget(saved);
    }
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    auto *items = new QVBoxLayout(content);
    items->setAlignment(Qt::AlignTop);
    auto *punchGrid = new QGridLayout;
    punchGrid->setAlignment(Qt::AlignTop);
    if (kind == core::ToolKind::Punch)
        items->addLayout(punchGrid);
    else
        delete punchGrid;
    std::vector<std::pair<QWidget *, QString>> searchable;
    QToolButton *initial = nullptr;
    int punches = 0;
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const auto &tool = tools_[i];
        if (!core::hasLibraryProfile(tool) || tool.kind != kind)
            continue;
        QString terms = QString::fromStdString(tool.code + " " + tool.manufacturer);
        for (double v : tool.openingsMm)
            terms += QStringLiteral(" V%1").arg(v);
        if (!tool.drawing || !tool.drawing->millimeters || !core::validToolDrawing(*tool.drawing) ||
            !tool.parametersConfirmed) {
            auto *record = new QLabel(terms + QStringLiteral(" — ") + issue(tool, 0), content);
            record->setObjectName(
                QStringLiteral("toolNeedsData_%1").arg(QString::fromStdString(tool.id)));
            record->setWordWrap(true);
            record->setTextFormat(Qt::PlainText);
            record->setMargin(8);
            items->addWidget(record);
            searchable.push_back({record, terms});
            continue;
        }
        auto *group =
            kind == core::ToolKind::Die
                ? new QGroupBox(QString::fromStdString(tool.code + " · " + tool.manufacturer),
                                content)
                : nullptr;
        auto *variants = group ? new QGridLayout(group) : punchGrid;
        if (group) {
            group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
            variants->setAlignment(Qt::AlignTop);
            const auto title = group->title();
            group->setTitle(
                group->fontMetrics().elidedText(title, Qt::ElideRight, popupWidth - 64));
            group->setAccessibleName(title);
            group->setToolTip(title);
        }
        std::vector<std::size_t> grooves(kind == core::ToolKind::Die ? tool.openingsMm.size() : 1);
        std::iota(grooves.begin(), grooves.end(), 0);
        if (kind == core::ToolKind::Die)
            std::stable_sort(grooves.begin(), grooves.end(), [&](auto a, auto b) {
                return tool.openingsMm[a] < tool.openingsMm[b];
            });
        for (std::size_t position = 0; position < grooves.size(); ++position) {
            const auto g = grooves[position];
            auto *button = new ToolContourButton(kind, group ? group : content, true);
            button->setObjectName(
                QStringLiteral("toolOption_%1_%2").arg(QString::fromStdString(tool.id)).arg(g));
            button->setProperty("toolId", QString::fromStdString(tool.id));
            button->setProperty("grooveIndex", static_cast<int>(g));
            button->setTool(tool, g, kind == core::ToolKind::Punch && punchReversed);
            const auto reason = issue(tool, g);
            button->setIssue(reason);
            button->setEnabled(reason.isEmpty());
            button->setChecked(current == tool && (kind == core::ToolKind::Punch || groove == g));
            if (!initial && button->isEnabled())
                initial = button;
            if (button->isChecked() && button->isEnabled())
                initial = button;
            connect(button, &QToolButton::clicked, this, [this, i, g] {
                choice_ = std::pair{tools_[i], g};
                accept();
            });
            if (group)
                variants->addWidget(button, static_cast<int>(position / dieColumns),
                                    static_cast<int>(position % dieColumns));
            else {
                variants->addWidget(button, punches / punchColumns, punches % punchColumns);
                ++punches;
                searchable.push_back({button, terms});
            }
        }
        if (group) {
            if (grooves.empty()) {
                auto *label =
                    new QLabel(QStringLiteral("Нужны параметры ручьёв и контур DXF"), group);
                label->setWordWrap(true);
                variants->addWidget(label, 0, 0);
            }
            searchable.push_back({group, terms});
            items->addWidget(group);
        }
    }
    auto *empty = new QLabel(searchable.empty() ? QStringLiteral("Добавьте инструмент из DXF")
                                                : QStringLiteral("Инструменты не найдены"),
                             content);
    empty->setVisible(searchable.empty());
    items->addWidget(empty);
    items->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto *add = new QPushButton(QStringLiteral("Добавить DXF…"), this);
    add->setAutoDefault(false);
    add->setObjectName("toolPickerImportDxf");
    add->setEnabled(canImport);
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { done(ImportDxf); });
    connect(search, &QLineEdit::textChanged, this, [searchable, empty](const QString &term) {
        bool any = false;
        for (const auto &[item, keywords] : searchable) {
            const bool visible = keywords.contains(term.trimmed(), Qt::CaseInsensitive);
            item->setVisible(visible);
            any |= visible;
        }
        empty->setVisible(!any);
    });
    resize(popupWidth, std::min(570, available.height() - 40));
    positionAtAnchor();
    QTimer::singleShot(0, this, [initial, search] {
        if (initial)
            initial->setFocus();
        else
            search->setFocus();
    });
}
void ToolPickerPopup::positionAtAnchor() {
    auto *anchor = parentWidget();
    if (!anchor)
        return;
    const auto origin = anchor->mapToGlobal(QPoint());
    auto *screen = QGuiApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()));
    const auto available = (screen ? screen : anchor->screen())->availableGeometry();
    move(std::clamp(origin.x(), available.left(),
                    std::max(available.left(), available.right() - width() + 1)),
         std::clamp(origin.y(), available.top(),
                    std::max(available.top(), available.bottom() - height() + 1)));
}
void ToolPickerPopup::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    positionAtAnchor();
    QTimer::singleShot(0, this, [this] { positionAtAnchor(); });
}
} // namespace bending::ui
