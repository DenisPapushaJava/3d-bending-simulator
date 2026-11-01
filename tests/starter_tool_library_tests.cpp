#include "ToolLibraryDialog.h"
#include "ToolSelectionWidget.h"
#include "bending/core/ToolGeometry.h"
#include "bending/project/DxfToolImporter.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/ImportController.h"
#include "bending/ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLockFile>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace bending;
namespace {
void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
std::filesystem::path native(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}
QByteArray bytes(const QString &path) {
    QFile f(path);
    require(f.open(QIODevice::ReadOnly), "Cannot read workshop");
    return f.readAll();
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const auto result = project::readStarterToolCatalog();
        if (const auto *error = std::get_if<std::string>(&result))
            throw std::runtime_error(*error);
        const auto starter = std::get<core::ToolCatalog>(result);
        require(starter.size() == 7, "Unexpected starter inventory");
        project::DxfToolImporter importer;
        for (const auto &tool : starter) {
            require(core::hasLibraryProfile(tool) && tool.parametersConfirmed && !tool.teaching,
                    tool.code + ": incomplete or teaching profile");
            require(tool.drawing->sourceFile.find('/') == std::string::npos &&
                        tool.source.find("/home/") == std::string::npos,
                    "Private source path leaked");
            require(tool.segments && !tool.segments->empty(),
                    tool.code + ": missing reference lengths");
            for (const auto &s : *tool.segments)
                require(s.quantity == 0, "Private stock leaked");
            const auto checked = importer.run(*tool.drawing, {});
            if (const auto *e = std::get_if<application::ToolProfileError>(&checked))
                throw std::runtime_error(tool.code + ": " + e->message);
            require(std::holds_alternative<application::CheckedToolProfile>(checked),
                    "DXF solid invalid");
            if (tool.kind == core::ToolKind::Punch) {
                const auto p = core::detectPunchProfile(*tool.drawing);
                require(p && std::abs(p->angleRadians - tool.angleRadians) < 1e-6 &&
                            tool.noseRadiusMm &&
                            std::abs(p->noseRadiusMm - *tool.noseRadiusMm) < 1e-5,
                        tool.code + ": working parameters disagree with DXF");
            } else
                for (std::size_t g = 0; g < tool.openingsMm.size(); ++g) {
                    const auto drawing = core::activeDieDrawing(tool, g);
                    require(drawing && !core::detectDieGrooves(*drawing).empty(),
                            tool.code + ": groove lost");
                }
        }
        QTemporaryDir root;
        require(root.isValid(), "Temporary directory unavailable");
        ui::setPreferencesFile(root.path() + "/settings.ini");
        const auto path = root.path() + "/tools/tool-catalog.json";
        const application::ToolLibraryStorage storage{project::readToolLibrary,
                                                      project::writeToolLibrary};
        const auto missing = project::readToolLibrary(native(path));
        require(!std::get<application::ToolLibrarySnapshot>(missing).exists,
                "Missing library looks existing");
        auto files = std::make_shared<project::ProjectFiles>();
        ui::ImportController controller(files, [](auto, auto) { return QString{}; });
        ui::ToolSelectionWidget selection({}, nullptr);
        selection.setToolProfileImport(controller, std::make_shared<project::DxfToolImporter>(),
                                       storage, starter);
        const auto first = bytes(path);
        auto loaded = project::readToolCatalog(native(path));
        require(std::holds_alternative<core::ToolCatalog>(loaded), "First-run library unreadable");
        auto local = std::get<core::ToolCatalog>(loaded);
        require(local.size() == starter.size(), "Starter not populated in GUI");
        for (const auto &t : local)
            require(!t.segments, "Unknown stock became zero available inventory");
        auto *punch = selection.findChild<QComboBox *>("punchPreset");
        auto *die = selection.findChild<QComboBox *>("diePreset");
        require(punch->count() == 5 && die->count() == 4,
                "Seeded palette has missing or duplicate profiles");
        require(native(ui::toolCatalogPath()) == native(path),
                "First-run library path not saved portably");
        const auto p = std::find_if(local.begin(), local.end(),
                                    [](const auto &t) { return t.code == "ST1405S"; });
        const auto d = std::find_if(local.begin(), local.end(),
                                    [](const auto &t) { return t.code == "2067-1"; });
        require(p != local.end() && d != local.end(), "Expected profiles missing");
        selection.chooseTool(*p, 0);
        selection.chooseTool(*d, 0);
        require(selection.assignment() && selection.parameters() &&
                    selection.assignment()->useDrawingGeometry,
                "Fresh starter pair cannot be used without entering private stock");
        for (std::size_t g = 0; g < d->openingsMm.size(); ++g) {
            require(selection.chooseTool(*d, g) && selection.assignment() &&
                        selection.assignment()->groove == g &&
                        core::drawingToolGeometry(*selection.assignment()).has_value(),
                    "A bundled four-way groove cannot use the captured body");
        }
        application::ToolLibrary library(local, native(path), storage, starter);
        const auto refs = library.sectionTemplate(*p);
        require(refs.size() > 1, "Profile-specific section template missing");
        ui::ToolLibraryDialog dialog(
            library, controller, std::make_shared<project::DxfToolImporter>(),
            [](const auto &, auto) { return QString{}; }, nullptr, {}, p->id);
        auto *enabled = dialog.findChild<QCheckBox *>("stockEnabled");
        auto *rows = dialog.findChild<QTableWidget *>("stockRows");
        require(enabled && !enabled->isChecked() &&
                    rows->rowCount() == static_cast<int>(refs.size()),
                "Reference template is absent or treated as known stock");
        for (int i = 0; i < rows->rowCount(); ++i) {
            const auto *q = qobject_cast<QSpinBox *>(rows->cellWidget(i, 1));
            require(q && q->value() == 0 && !q->isEnabled(),
                    "Template quantities are active inventory");
        }
        if (const auto image = qEnvironmentVariable("BENDING_STARTER_SNAPSHOT"); !image.isEmpty()) {
            dialog.show();
            QApplication::processEvents();
            require(dialog.grab().save(image), "Snapshot failed");
        }
        dialog.reject();
        require(bytes(path) == first, "Opening reference sections rewrote the workshop");
        auto renamed = *p;
        renamed.code = "My own tool";
        require(library.upsert(renamed).empty(), "Rename failed");
        const auto edited = bytes(path);
        application::ToolLibrary restart(library.data().tools, native(path), storage, starter);
        require(bytes(path) == edited && restart.data().tools == library.data().tools,
                "Update overwrote an edit");
        ui::ToolSelectionWidget noInitialCatalogue({}, nullptr);
        noInitialCatalogue.setToolProfileImport(
            controller, std::make_shared<project::DxfToolImporter>(), storage, starter);
        require(bytes(path) == edited &&
                    noInitialCatalogue.findChild<QComboBox *>("punchPreset")->count() == 5,
                "Existing local profiles were hidden when preferences did not preload them");
        QLockFile lock(path + ".lock");
        require(lock.tryLock(0), "Cannot lock library");
        const auto locked = project::writeToolLibrary(native(path), {{}, {}}, "missing");
        require(std::holds_alternative<std::string>(locked) && bytes(path) == edited,
                "Concurrent writer overwrote library");
        lock.unlock();
        const auto deniedPath = root.path() + "/tools/denied.json";
        QLockFile deniedLock(deniedPath + ".lock");
        require(deniedLock.tryLock(0), "Cannot lock first-run destination");
        application::ToolLibrary denied({}, native(deniedPath), storage, starter);
        require(!denied.error().empty() && denied.data().tools.empty() &&
                    !QFile::exists(deniedPath),
                "Failed initial storage published a usable workshop");
        deniedLock.unlock();
        const auto brokenPath = root.path() + "/tools/broken.json";
        QFile broken(brokenPath);
        require(broken.open(QIODevice::WriteOnly) && broken.write("broken") == 6,
                "Cannot prepare unreadable catalogue");
        broken.close();
        application::ToolLibrary damaged({}, native(brokenPath), storage, starter);
        require(!damaged.error().empty() && bytes(brokenPath) == "broken",
                "Starter replaced an unreadable existing file");
        controller.shutdown();
        std::cout << "Starter profiles, solids, first-run seed, unknown stock, templates and "
                     "preservation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
