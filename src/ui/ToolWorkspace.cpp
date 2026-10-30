#include "ToolWorkspace.h"
#include "ToolProfilesWidget.h"
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>
namespace bending::ui {
namespace {
QString number(double n) {
    return QLocale().toString(n, 'g', 6);
}
QString sections(const core::ToolPreset &tool, const std::optional<core::ToolAssembly> &assembly) {
    if (!tool.segments)
        return QStringLiteral("Цельный инструмент · %1 мм").arg(number(tool.lengthMm));
    if (!assembly || !core::assemblyLength(*tool.segments, *assembly))
        return QStringLiteral("Секции не подобраны — укажите наличие в библиотеке");
    QStringList pieces;
    for (auto row : assembly->pieces)
        pieces << number((*tool.segments)[row].lengthMm);
    return QStringLiteral("%1 мм · всего %2 мм · зазор %3 мм")
        .arg(pieces.join(QStringLiteral(" + ")),
             number(*core::assemblyLength(*tool.segments, *assembly)), number(assembly->gapMm));
}
} // namespace
ToolWorkspace::ToolWorkspace(QWidget *parent) : QWidget(parent) {
    setObjectName("toolWorkspace");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *title = new QLabel(QStringLiteral("Гибочный инструмент"), this);
    auto font = title->font();
    font.setPointSize(font.pointSize() + 4);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    profiles_ = new ToolProfilesWidget(this, true);
    profiles_->setObjectName("toolWorkspaceProfiles");
    layout->addWidget(profiles_, 1);
    punch_ = new QLabel(this);
    die_ = new QLabel(this);
    punch_->setObjectName("toolWorkspacePunchSegments");
    die_->setObjectName("toolWorkspaceDieSegments");
    for (auto *label : {punch_, die_}) {
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
    setChoice({}, {});
}
void ToolWorkspace::setChoice(std::optional<core::PressTools> tools,
                              std::optional<core::ToolAssignment> assignment) {
    if (tools_ == tools && assignment_ == assignment && !punch_->text().isEmpty())
        return;
    tools_ = std::move(tools);
    assignment_ = std::move(assignment);
    profiles_->setTools(tools_);
    profiles_->setDrawingAssignment(assignment_);
    punch_->setText(
        assignment_ ? QStringLiteral("Пуансон %1: %2")
                          .arg(QString::fromStdString(assignment_->punch.code),
                               sections(assignment_->punch, assignment_->punchAssembly))
        : tools_ ? QStringLiteral("Пуансон · длина %1 мм").arg(number(core::punchLengthMm(*tools_)))
                 : QStringLiteral("Выберите пуансон в библиотеке справа"));
    die_->setText(assignment_ ? QStringLiteral("Матрица %1: %2")
                                    .arg(QString::fromStdString(assignment_->die.code),
                                         sections(assignment_->die, assignment_->dieAssembly))
                  : tools_ ? QStringLiteral("Матрица · V %1 мм · длина %2 мм")
                                 .arg(number(tools_->openingMm), number(tools_->lengthMm))
                           : QStringLiteral("Выберите матрицу и ручей"));
}
} // namespace bending::ui
