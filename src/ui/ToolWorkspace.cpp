#include "ToolWorkspace.h"
#include "ToolContourButton.h"
#include "ToolSelectionWidget.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
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
    QStringList pieces;
    for (auto row : assembly->pieces)
        pieces << number((*tool.segments)[row].lengthMm);
    return QStringLiteral("%1 мм · всего %2 мм · зазор %3 мм")
        .arg(pieces.join(QStringLiteral(" + ")), number(*length), number(assembly->gapMm));
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
    auto *row = new QHBoxLayout(profiles_);
    row->setContentsMargins(0, 0, 0, 0);
    punchProfile_ = new ToolContourButton(core::ToolKind::Punch, profiles_);
    dieProfile_ = new ToolContourButton(core::ToolKind::Die, profiles_);
    punchProfile_->setObjectName("punchProfile");
    dieProfile_->setObjectName("dieProfile");
    row->addWidget(punchProfile_, 1);
    row->addWidget(dieProfile_, 1);
    layout->addWidget(profiles_, 1);
    grooveRow_ = new QWidget(this);
    grooves_ = new QHBoxLayout(grooveRow_);
    grooves_->setContentsMargins(0, 0, 0, 0);
    grooves_->addStretch();
    grooveRow_->hide();
    layout->addWidget(grooveRow_);
    punch_ = new QLabel(this);
    die_ = new QLabel(this);
    punch_->setObjectName("toolWorkspacePunchSegments");
    die_->setObjectName("toolWorkspaceDieSegments");
    for (auto *label : {punch_, die_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(label);
    }
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
    punchProfile_->setTool(punch, 0, selection_->punchReversed());
    dieProfile_->setTool(die, groove);
    punchProfile_->setIssue(punch ? selection_->optionIssue(*punch, 0) : QString{});
    dieProfile_->setIssue(die ? selection_->optionIssue(*die, groove) : QString{});
    punch_->setText(punch
                        ? QString::fromStdString(punch->code) + QStringLiteral(" · ") +
                              sections(*punch, selection_->selectedAssembly(core::ToolKind::Punch))
                        : QString{});
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
                    if (const auto groove = core::dieGroove(*die, index))
                        label += QStringLiteral(" · %1°").arg(
                            number(groove->angleRadians * 180 / std::numbers::pi));
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
