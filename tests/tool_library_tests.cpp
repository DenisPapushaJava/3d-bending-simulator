#include "ToolFixtures.h"
#include "ToolLibraryDialog.h"
#include "ToolSelectionWidget.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}
QByteArray bytes(const QString &name) {
    QFile f(name);
    require(f.open(QIODevice::ReadOnly), "read");
    return f.readAll();
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        require(dir.isValid(), "temporary directory");
        ui::setPreferencesFile(dir.path() + "/settings.ini");
        const auto name = dir.path() + "/tools/tool-catalog.json";
        const auto pair = tool_fixture::pair();
        auto files = std::make_shared<project::ProjectFiles>();
        ui::ImportController controller(files, [](auto, auto) { return QString{}; });
        // Optional native visual audit: read private definitions into an isolated
        // temporary library. Never edit the supplied catalogue or user settings.
        if (const auto input = qEnvironmentVariable("BENDING_LIBRARY_CATALOG"); !input.isEmpty()) {
            const auto loaded = project::readToolCatalog(native(input));
            require(std::holds_alternative<core::ToolCatalog>(loaded), "preview catalogue");
            const auto tools = std::get<core::ToolCatalog>(loaded);
            std::string id;
            for (const auto &tool : tools)
                if (QString::fromStdString(tool.code) ==
                    qEnvironmentVariable("BENDING_LIBRARY_SELECTED"))
                    id = tool.id;
            application::ToolLibrary preview(tools, native(name),
                                             {project::readToolLibrary, project::writeToolLibrary});
            ui::ToolLibraryDialog dialog(
                preview, controller, std::make_shared<project::DxfToolImporter>(),
                [](const auto &, auto) { return QString{}; }, nullptr, {}, id);
            QTimer::singleShot(100, &dialog, [&] {
                const auto output = qEnvironmentVariable("BENDING_LIBRARY_SNAPSHOT");
                if (!output.isEmpty())
                    require(dialog.grab().save(output), "preview screenshot");
                dialog.reject();
            });
            dialog.exec();
            controller.shutdown();
            return 0;
        }
        ui::ToolSelectionWidget selection({pair.punch, pair.die}, nullptr);
        selection.setToolProfileImport(controller, std::make_shared<project::DxfToolImporter>(),
                                       {project::readToolLibrary, project::writeToolLibrary});
        selection.setChoice(core::toolParameters(pair), pair);
        const auto before = selection.assignment();
        int changed = 0;
        int cancelled = 0;
        selection.onChanged = [&] { ++changed; };
        selection.onCancelChecked = [&] { ++cancelled; };
        QString error;
        QTimer::singleShot(0, [&] {
            auto *dialog = selection.findChild<ui::ToolLibraryDialog *>();
            try {
                require(dialog, "unified library did not open");
                auto *nameEdit = dialog->findChild<QLineEdit *>("libraryToolName");
                nameEdit->setText(QStringLiteral("Пуансон с новым именем"));
                dialog->findChild<QPushButton *>("stockSave")->click();
                QApplication::processEvents();
                auto saved = project::readToolLibrary(native(name));
                require(std::holds_alternative<application::ToolLibrarySnapshot>(saved),
                        "saved library not readable");
                require(std::get<application::ToolLibrarySnapshot>(saved).data.tools.front().code ==
                            "Пуансон с новым именем",
                        "rename not saved");
                require(selection.assignment() == before && changed == 0,
                        "rename silently changed assignment");
                require(cancelled == 1, "workshop edit retained stale checked-search candidates");
                auto *enabled = dialog->findChild<QCheckBox *>("stockEnabled");
                enabled->setChecked(true);
                auto *rows = dialog->findChild<QTableWidget *>("stockRows");
                require(rows->rowCount() == 1, "unknown stock invented template rows");
                static_cast<QSpinBox *>(rows->cellWidget(0, 1))->setValue(2);
                dialog->findChild<QPushButton *>("stockSave")->click();
                QApplication::processEvents();
                auto tools = std::get<core::ToolCatalog>(project::readToolCatalog(native(name)));
                require(tools.front().segments && tools.front().segments->front().quantity == 2,
                        "stock not saved");
                require(selection.assignment() == before && changed == 0,
                        "stock silently rebuilt assignment");
                auto *list = dialog->findChild<QListWidget *>("libraryTools");
                list->setCurrentRow(1);
                dialog->findChild<QPushButton *>("libraryDelete")->click();
                QApplication::processEvents();
                auto snapshot = std::get<application::ToolLibrarySnapshot>(
                    project::readToolLibrary(native(name)));
                require(snapshot.data.tools.size() == 1 &&
                            snapshot.data.archived.front() == pair.die,
                        "deletion lost archived geometry");
                require(selection.assignment() == before && changed == 0,
                        "deletion broke captured assignment");
                auto *archive = dialog->findChild<QCheckBox *>("libraryArchive");
                archive->setChecked(true);
                require(!dialog->findChild<QPushButton *>("libraryUse")->isEnabled(),
                        "archived tool assignable");
                dialog->findChild<QPushButton *>("libraryDelete")->click();
                QApplication::processEvents();
                require(
                    std::get<core::ToolCatalog>(project::readToolCatalog(native(name))).size() == 2,
                    "restore failed");
                archive->setChecked(false);
                list->setCurrentRow(0);
                if (const auto output = qEnvironmentVariable("BENDING_LIBRARY_SNAPSHOT");
                    !output.isEmpty()) {
                    QApplication::processEvents();
                    require(dialog->grab().save(output), "library screenshot failed");
                }
                dialog->reject();
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
                if (dialog)
                    dialog->reject();
            }
        });
        selection.findChild<QPushButton *>("toolStock")->click();
        require(error.isEmpty(), qPrintable(error));
        require(selection.assignment() == before && changed == 0,
                "closing library applied new stock");
        const auto data =
            std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(name)));
        require(QJsonDocument::fromJson(bytes(name)).object().value("version").toInt() == 5,
                "archive schema missing");
        application::ToolLibrary owner(data.data.tools, native(name),
                                       {project::readToolLibrary, project::writeToolLibrary});
        require(owner.archive(pair.die.id).empty(), "archive command failed");
        const auto current =
            std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(name)));
        application::ToolLibrary restarted(current.data.tools, native(name),
                                           {project::readToolLibrary, project::writeToolLibrary});
        require(restarted.restore(pair.die.id).empty(), "archive not durable across new owner");
        auto external = restarted.data().tools.front();
        external.notes = "another writer";
        require(restarted.upsert(external).empty(), "concurrent change setup");
        const auto stable = bytes(name);
        require(std::holds_alternative<std::string>(
                    project::writeToolLibrary(native(name), data.data, data.revision)),
                "stale writer accepted");
        require(bytes(name) == stable, "stale writer damaged file");
        QLockFile lock(name + ".lock");
        require(lock.tryLock(0), "lock setup");
        const auto latest =
            std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(name)));
        require(std::holds_alternative<std::string>(
                    project::writeToolLibrary(native(name), latest.data, latest.revision)),
                "concurrent lock bypassed");
        lock.unlock();
        require(bytes(name) == stable, "locked write damaged file");
        const auto legacy = dir.path() + "/legacy.json";
        require(!project::writeToolCatalog(native(legacy), {pair.punch, pair.die}), "legacy setup");
        const auto legacyBytes = bytes(legacy);
        require(std::get<application::ToolLibrarySnapshot>(project::readToolLibrary(native(legacy)))
                        .data.tools.size() == 2,
                "v4 not readable");
        require(bytes(legacy) == legacyBytes, "reading migrated file on disk");
        application::ToolLibrary migration({pair.punch, pair.die}, native(legacy),
                                           {project::readToolLibrary, project::writeToolLibrary});
        auto renamed = pair.punch;
        renamed.code = "upgraded";
        require(migration.upsert(renamed).empty(), "v4 upgrade failed");
        require(bytes(legacy + ".pre-v5.json") == legacyBytes,
                "migration backup is not byte-exact");
        auto multi = tool_fixture::fourWayDie();
        require(restarted.upsert(multi).empty(), "multi-groove setup");
        ui::ToolLibraryDialog matrix(
            restarted, controller, {}, [](const auto &, auto) { return QString{}; }, nullptr,
            core::ToolKind::Die, multi.id);
        matrix.show();
        auto *grooves = matrix.findChild<QComboBox *>("libraryGroove");
        require(grooves->count() == 4, "missing working grooves");
        grooves->setCurrentIndex(3);
        matrix.findChild<QCheckBox *>("stockEnabled")->setChecked(true);
        auto *matrixRows = matrix.findChild<QTableWidget *>("stockRows");
        static_cast<QSpinBox *>(matrixRows->cellWidget(0, 1))->setValue(2);
        matrix.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(grooves->currentIndex() == 3, "saving stock silently changed the working groove");
        grooves->setCurrentIndex(0);
        matrixRows = matrix.findChild<QTableWidget *>("stockRows");
        require(static_cast<QSpinBox *>(matrixRows->cellWidget(0, 1))->value() == 2,
                "working grooves have separate stock");
        matrix.findChild<QLineEdit *>("libraryToolName")
            ->setText(QStringLiteral("Matrix accepted"));
        matrix.accept();
        require(restarted.data().tools.back().code == "Matrix accepted",
                "accept lost pending metadata");
        const auto good = bytes(name);
        auto invalid = QJsonDocument::fromJson(good).object();
        invalid.insert("archived", QJsonArray{invalid.value("tools").toArray()[0]});
        const auto broken = dir.path() + "/invalid.json";
        QFile bad(broken);
        require(bad.open(QIODevice::WriteOnly), "invalid archive setup");
        bad.write(QJsonDocument(invalid).toJson());
        bad.close();
        require(std::holds_alternative<std::string>(project::readToolLibrary(native(broken))),
                "same ID accepted in active library and archive");

        auto pendingTool = pair.punch;
        pendingTool.lengthMm = 0;
        pendingTool.parametersConfirmed = false;
        pendingTool.segments = std::vector<core::ToolSegment>{{835, 3}, {100, 1}};
        const auto pendingPath = dir.path() + "/pending.json";
        require(!project::writeToolCatalog(native(pendingPath), {pendingTool}),
                "pending profile fixture failed");
        const auto pendingBytes = bytes(pendingPath);
        application::ToolLibrary pendingLibrary(
            {pendingTool}, native(pendingPath),
            {project::readToolLibrary, project::writeToolLibrary});
        const auto optionIssue = [&](const auto &tool, auto g) {
            return selection.optionIssue(tool, g);
        };
        {
            ui::ToolLibraryDialog view(pendingLibrary, controller, {}, optionIssue, nullptr);
            require(!view.findChild<QPushButton *>("libraryUse")->isEnabled(),
                    "unconfirmed tool became assignable on read");
            view.reject();
            require(bytes(pendingPath) == pendingBytes,
                    "opening/closing automatically completed the profile");
        }
        ui::ToolLibraryDialog complete(pendingLibrary, controller, {}, optionIssue, nullptr);
        complete.show();
        require(selection.optionIssue(pendingTool, 0).contains(QStringLiteral("длину")),
                "missing length hidden behind a generic confirmation warning");
        // Save without re-entering the already available stock.
        complete.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(pendingLibrary.data().tools.front().parametersConfirmed &&
                    pendingLibrary.data().tools.front().lengthMm == 835,
                "explicit save did not complete a previously imported profile");
        require(complete.findChild<QPushButton *>("libraryUse")->isEnabled(),
                "completed profile still unavailable");
        require(pendingLibrary.data().tools.front().drawing == pendingTool.drawing &&
                    pendingLibrary.data().tools.front().segments == pendingTool.segments,
                "completion modified geometry or quantities");
        complete.findChild<QPushButton *>("libraryUse")->click();
        require(complete.choice() && complete.choice()->first.parametersConfirmed,
                "explicit use did not capture the completed profile");
        auto noStock = pendingTool;
        noStock.segments.reset();
        require(pendingLibrary.upsert(noStock).empty(), "nominal-only setup failed");
        ui::ToolLibraryDialog nominal(pendingLibrary, controller, {}, optionIssue, nullptr);
        nominal.findChild<QDoubleSpinBox *>("libraryNominalLength")->setValue(415);
        nominal.findChild<QPushButton *>("stockSave")->click();
        QApplication::processEvents();
        require(pendingLibrary.data().tools.front().parametersConfirmed &&
                    pendingLibrary.data().tools.front().lengthMm == 415 &&
                    nominal.findChild<QPushButton *>("libraryUse")->isEnabled(),
                "editing nominal length left confirmation stale");
        nominal.reject();
        controller.shutdown();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
