#include "ToolWorkspace.h"
#include "ToolAssemblyDiagram.h"
#include "ToolContourButton.h"
#include "ToolEndButtons.h"
#include "ToolSelectionWidget.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <numbers>
#include <numeric>
namespace bending::ui {
namespace {
QString number(double n) {
    return QLocale().toString(n, 'g', 6);
}
QString sections(const core::ToolPreset &tool, const std::optional<core::ToolAssembly> &assembly) {
    if (!tool.segments)
        return QStringLiteral("Цельный · %1 мм").arg(number(tool.lengthMm));
    const auto length = assembly ? core::assemblyLength(*tool.segments, *assembly) : std::nullopt;
    if (!length)
        return QStringLiteral("Укажите секции и наличие");
    auto text =
        QStringLiteral("Сборка %1 мм · %2 секц.").arg(number(*length)).arg(assembly->pieces.size());
    const auto gap = core::maximumAssemblyGap(*assembly);
    if (gap > 0)
        text += QStringLiteral(" · зазор до %1 мм").arg(number(gap));
    return text;
}
} // namespace
ToolWorkspace::ToolWorkspace(QWidget *parent) : QWidget(parent) {
    setObjectName("toolWorkspace");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    context_ = new QLabel(this);
    context_->setObjectName("toolWorkspaceContext");
    context_->setTextFormat(Qt::PlainText);
    layout->addWidget(context_);
    profiles_ = new QWidget(this);
    profiles_->setObjectName("toolWorkspaceProfiles");
    auto *row = new QGridLayout(profiles_);
    row->setColumnStretch(0, 1);
    row->setColumnStretch(1, 1);
    row->setRowStretch(0, 1);
    row->setContentsMargins(0, 0, 0, 0);
    punchProfile_ = new ToolContourButton(core::ToolKind::Punch, profiles_);
    dieProfile_ = new ToolContourButton(core::ToolKind::Die, profiles_);
    punchProfile_->setObjectName("punchProfile");
    dieProfile_->setObjectName("dieProfile");
    punchAssembly_ = new ToolAssemblyDiagram({}, profiles_);
    dieAssembly_ = new ToolAssemblyDiagram({}, profiles_);
    punchAssembly_->setObjectName("punchAssemblyDiagram");
    dieAssembly_->setObjectName("dieAssemblyDiagram");
    ends_ = new ToolEndButtons(profiles_);
    ends_->setObjectName("workspacePunchEnds");
    grooveRow_ = new QWidget(profiles_);
    grooves_ = new QHBoxLayout(grooveRow_);
    grooves_->setContentsMargins(0, 0, 0, 0);
    grooves_->addStretch();
    auto *dieSlot = new QWidget(profiles_);
    dieSlot->setMinimumHeight(44);
    auto *dieSlotLayout = new QVBoxLayout(dieSlot);
    dieSlotLayout->setContentsMargins(0, 0, 0, 0);
    dieSlotLayout->addWidget(grooveRow_);
    punch_ = new QLabel(this);
    die_ = new QLabel(this);
    punch_->setObjectName("toolWorkspacePunchSegments");
    die_->setObjectName("toolWorkspaceDieSegments");
    editPunch_ = new QPushButton(QStringLiteral("Секции…"), this);
    editDie_ = new QPushButton(QStringLiteral("Секции…"), this);
    editPunch_->setObjectName("editPunchAssembly");
    editDie_->setObjectName("editDieAssembly");
    editPunch_->setAccessibleName(QStringLiteral("Расположение секций пуансона"));
    editDie_->setAccessibleName(QStringLiteral("Расположение секций матрицы"));
    std::array<ToolContourButton *, 2> buttons{punchProfile_, dieProfile_};
    std::array<ToolAssemblyDiagram *, 2> diagrams{punchAssembly_, dieAssembly_};
    for (std::size_t side = 0; side < 2; ++side) {
        row->addWidget(buttons[side], 0, static_cast<int>(side));
        row->addWidget(diagrams[side], 1, static_cast<int>(side));
        row->addWidget(side ? dieSlot : static_cast<QWidget *>(ends_), 2, static_cast<int>(side));
        auto *label = side ? die_ : punch_;
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *line = new QHBoxLayout;
        line->addWidget(label, 1);
        auto *edit = side ? editDie_ : editPunch_;
        edit->setToolTip(edit->accessibleName());
        line->addWidget(edit);
        row->addLayout(line, 3, static_cast<int>(side));
        const auto kind = side ? core::ToolKind::Die : core::ToolKind::Punch;
        diagrams[side]->intervals = [this, kind] {
            const auto tool = selection_ ? selection_->selectedTool(kind) : std::nullopt;
            const auto assembly = selection_ ? selection_->selectedAssembly(kind) : std::nullopt;
            return tool && tool->segments && assembly
                       ? core::assemblyIntervals(*tool->segments, *assembly)
                       : std::nullopt;
        };
        diagrams[side]->required = [this] {
            return selection_ ? selection_->requiredAssemblyLength().value_or(0) : 0;
        };
        diagrams[side]->onSelected = [this, kind](int piece) {
            if (selection_) {
                selection_->editAssembly(kind, static_cast<std::size_t>(piece));
                refresh();
            }
        };
        connect(edit, &QPushButton::clicked, this, [this, kind] {
            if (selection_) {
                selection_->editAssembly(kind);
                refresh();
            }
        });
    }
    layout->addWidget(profiles_, 1);
    fit_ = new QPushButton(QStringLiteral("Подобрать секции"), this);
    fit_->setObjectName("workspaceFitSections");
    layout->addWidget(fit_);
    notice_ = new QLabel(this);
    notice_->setObjectName("workspaceAssemblyNotice");
    notice_->setWordWrap(true);
    notice_->hide();
    layout->addWidget(notice_);
    connect(fit_, &QPushButton::clicked, this, [this] {
        if (!selection_)
            return;
        const auto issue = selection_->fitSelectedAssemblies();
        refresh();
        notice_->setText(issue);
        notice_->setVisible(!issue.isEmpty());
    });
    ends_->onSelected = [this](const core::ToolPreset &snapshot, bool right,
                               std::optional<std::size_t> piece) {
        if (!selection_)
            return;
        const auto issue = selection_->setPunchEnd(snapshot, right, piece);
        refresh();
        notice_->setText(issue);
        notice_->setVisible(!issue.isEmpty());
    };
    connect(punchProfile_, &QToolButton::clicked, this, [this] {
        if (selection_)
            selection_->showPicker(core::ToolKind::Punch, punchProfile_);
    });
    connect(dieProfile_, &QToolButton::clicked, this, [this] {
        if (selection_)
            selection_->showPicker(core::ToolKind::Die, dieProfile_);
    });
}
void ToolWorkspace::bindSelection(ToolSelectionWidget *selection) {
    selection_ = selection;
    if (selection_)
        selection_->useInlineAssemblyControls();
}
void ToolWorkspace::setContext(const QString &text) {
    context_->setText(text);
    context_->setVisible(!text.isEmpty());
}
void ToolWorkspace::refresh() {
    if (!selection_)
        return;
    const auto punch = selection_->selectedTool(core::ToolKind::Punch);
    const auto die = selection_->selectedTool(core::ToolKind::Die);
    const auto groove = selection_->selectedGroove();
    notice_->hide();
    punchAssembly_->setTool(punch);
    dieAssembly_->setTool(die);
    punchAssembly_->setVisible(punch && punch->segments.has_value());
    dieAssembly_->setVisible(die && die->segments.has_value());
    ends_->setVisible(punch && punch->segments.has_value());
    if (punch)
        ends_->setSource(*punch, selection_->selectedAssembly(core::ToolKind::Punch));
    ends_->setEnabled(selection_->canEditAssemblies());
    fit_->setEnabled(selection_->isEnabled() && selection_->canFitAssemblies());
    const auto maximum = selection_->compactPunchMaximumMm();
    if (maximum)
        fit_->setToolTip(QStringLiteral("Пуансон до %1 мм с зазором у полок. "
                                        "Свободный ход проверяется в симуляции.")
                             .arg(*maximum, 0, 'f', 2));
    else if (const auto minimum = selection_->compactSectionMinimumMm())
        fit_->setToolTip(QStringLiteral("Подобрать непрерывные сборки от %1 мм до длины гиба. "
                                        "Проём проверяется в симуляции.")
                             .arg(*minimum, 0, 'f', 2));
    else
        fit_->setToolTip(QStringLiteral(
            "Подобрать длину обоих инструментов, сохранив выбранные крайние сапожки."));

    editPunch_->setEnabled(selection_->isEnabled() && punch && punch->segments.has_value());
    editDie_->setEnabled(selection_->isEnabled() && die && die->segments.has_value());
    punchProfile_->setTool(punch, 0, selection_->punchReversed());
    dieProfile_->setTool(die, groove);
    punchProfile_->setIssue(punch ? selection_->optionIssue(*punch, 0) : QString{});
    dieProfile_->setIssue(die ? selection_->optionIssue(*die, groove) : QString{});
    punch_->setText(punch
                        ? QString::fromStdString(punch->code) + QStringLiteral(" · ") +
                              sections(*punch, selection_->selectedAssembly(core::ToolKind::Punch))
                        : QString{});
    if (punch && selection_->punchReversed())
        punch_->setText(punch_->text() + QStringLiteral(" · схема в обычной ориентации"));
    if (maximum)
        punch_->setText(punch_->text() +
                        QStringLiteral(" · по проёму до %1 мм").arg(*maximum, 0, 'f', 2));
    die_->setText(die ? QString::fromStdString(die->code) + QStringLiteral(" · ") +
                            sections(*die, selection_->selectedAssembly(core::ToolKind::Die))
                      : QString{});
    profiles_->setAccessibleDescription(punchProfile_->accessibleDescription() + QLatin1Char('\n') +
                                        dieProfile_->accessibleDescription());
    if (shownDie_ != die) {
        shownDie_ = die;
        while (auto *item = grooves_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        grooves_->addStretch();
        if (die && die->openingsMm.size() > 1) {
            std::vector<std::size_t> indices(die->openingsMm.size());
            std::iota(indices.begin(), indices.end(), 0);
            std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) {
                return die->openingsMm[a] < die->openingsMm[b];
            });
            for (auto index : indices) {
                auto *button = new QToolButton(grooveRow_);
                auto label = QStringLiteral("V %1").arg(number(die->openingsMm[index]));
                if (std::count(die->openingsMm.begin(), die->openingsMm.end(),
                               die->openingsMm[index]) > 1) {
                    if (const auto workingGroove = core::dieGroove(*die, index))
                        label += QStringLiteral(" · %1°").arg(
                            number(workingGroove->angleRadians * 180 / std::numbers::pi));
                    label += QStringLiteral(" · #%1").arg(index + 1);
                }
                button->setText(label);
                button->setObjectName(QStringLiteral("activeGroove%1").arg(index));
                button->setProperty("grooveIndex", static_cast<int>(index));
                button->setCheckable(true);
                button->setAutoExclusive(true);
                button->setMinimumHeight(32);
                grooves_->addWidget(button);
                connect(button, &QToolButton::clicked, this, [this, index] {
                    if (selection_)
                        if (const auto tool = selection_->selectedTool(core::ToolKind::Die))
                            selection_->chooseTool(*tool, index);
                });
            }
        }
    }
    grooveRow_->setVisible(die && die->openingsMm.size() > 1);
    if (die)
        for (auto *button : grooveRow_->findChildren<QToolButton *>()) {
            const auto index = static_cast<std::size_t>(button->property("grooveIndex").toInt());
            const auto issue = selection_->optionIssue(*die, index);
            button->setEnabled(issue.isEmpty());
            button->setChecked(index == groove);
            button->setToolTip(issue.isEmpty() ? button->text() : issue);
        }
}
} // namespace bending::ui
