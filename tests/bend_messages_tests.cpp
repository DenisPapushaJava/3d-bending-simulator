#include "BendMessagePanel.h"
#include "ToolFixtures.h"
#include "ToolSelectionWidget.h"
#include "bending/ui/Preferences.h"
#include <QApplication>
#include <QLabel>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QToolButton>
#include <iostream>
#include <stdexcept>
using namespace bending::ui;
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir folder;
    bending::ui::setPreferencesFile(folder.path() + "/settings.ini");
    try {
        BendMessagePanel panel;
        const std::vector<BendMessage> messages{
            {BendMessageKind::ToolLength, BendMessageSeverity::Warning, "Пуансон короче на 100 мм",
             "Пуансон: 80 из 180 мм. Только просмотр."},
            {BendMessageKind::Collision, BendMessageSeverity::Error, "Столкновение",
             "Деталь пересекает пуансон."}};
        panel.setMessages(messages);
        require(!panel.isVisible(), "Messages escaped the bending stage");
        panel.setActive(true);
        auto *toggle = panel.findChild<QToolButton *>("bendMessagesToggle");
        auto *summary = panel.findChild<QLabel *>("bendMessagesSummary");
        auto *details = panel.findChild<QLabel *>("bendMessagesText");
        auto *scroll = panel.findChild<QScrollArea *>("bendMessagesDetails");
        require(panel.isVisible() && toggle && toggle->text().contains("Столкновение") &&
                    summary->text().contains("Пуансон") && !scroll->isVisible(),
                "Collision hid the length warning or did not receive priority");
        toggle->click();
        require(scroll->isVisible() && details->text().contains("80 из 180") &&
                    details->text().contains("пересекает"),
                "Details lost a simultaneous issue");
        panel.setMessages(messages);
        require(toggle->isChecked(), "An unchanged frame reset the user's expanded panel");
        panel.setMessages({messages.front()});
        require(toggle->isChecked() && !toggle->text().contains("Столкновение"),
                "The previous collision survived a fresh result");
        panel.setActive(false);
        require(!panel.isVisible(), "Message panel remained visible outside bending");
        panel.setMessages({});
        panel.setActive(true);
        require(!panel.isVisible(), "An empty panel obscures the viewport");
        auto pair = tool_fixture::pair(16);
        ToolSelectionWidget selection({pair.punch, pair.die}, nullptr);
        selection.show();
        selection.setChoice(bending::core::toolParameters(pair), pair);
        selection.setSelectionRequest(bending::core::ToolSelectionRequest{2, 1.57, 1, 180});
        auto *length = selection.findChild<QLabel *>("toolLengthWarning");
        require(length && length->isVisible() && length->text().contains("не хватает 100"),
                "A short unsegmented local tool hides its length warning");
        pair.punch.lengthMm = pair.die.lengthMm = 200;
        selection.setChoice(bending::core::toolParameters(pair), pair);
        require(!length->isVisible(), "A longer tool is incorrectly rejected by length");
        pair.punch.lengthMm = 80;
        selection.setChoice(bending::core::toolParameters(pair), pair);
        require(length->isVisible(), "A short punch was hidden by a sufficiently long die");
        pair.punch.lengthMm = 200;
        pair.die.lengthMm = 80;
        selection.setChoice(bending::core::toolParameters(pair), pair);
        require(length->isVisible(), "A short die was hidden by a sufficiently long punch");
        pair.punch.segments = pair.die.segments = std::vector<bending::core::ToolSegment>{{40, 2}};
        pair.punchAssembly = pair.dieAssembly = bending::core::ToolAssembly{{0, 0}, 3};
        selection.setSelectionRequest(bending::core::ToolSelectionRequest{2, 1.57, 1, 82});
        selection.setChoice(bending::core::toolParameters(pair), pair);
        require(!length->isVisible(), "A permitted segment gap was lost from the actual span");
        pair.punchAssembly->gapMm = pair.dieAssembly->gapMm = 0;
        selection.setChoice(bending::core::toolParameters(pair), pair);
        require(length->isVisible(), "Insufficient assembled length appears sufficient");
        std::cout << "Message priority, simultaneous length warning, expansion and stage "
                     "visibility passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
