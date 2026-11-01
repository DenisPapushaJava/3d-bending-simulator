#include "PortablePackage.h"
#include "bending/update/UpdateClient.h"
#include <QCoreApplication>
#include <QDir>
#include <iostream>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() < 4)
        return 2;
    if (app.arguments()[3] == "discover") {
        const auto root = QDir(app.arguments()[2]);
        return bending::update::discoverPortableRoot(root.filePath("runtime/2026.1.0-alpha.2/bin"),
                                                     "2026.1.0-alpha.2") == root.canonicalPath()
                   ? 0
                   : 4;
    }
    std::stop_source stop;
    if (app.arguments().contains("--cancel"))
        stop.request_stop();
    const auto error = bending::update::stageRuntime(app.arguments()[1], "2026.1.0-alpha.2",
                                                     app.arguments()[2], stop.get_token());
    if (!error.isEmpty())
        std::cerr << error.toStdString() << '\n';
    if (error.isEmpty()) {
        const auto bin = QDir(app.arguments()[2]).filePath("runtime/2026.1.0-alpha.2/bin");
        // Inactive versions must not be mistaken for the current portable application.
        if (!bending::update::discoverPortableRoot(bin, "2026.1.0-alpha.2").isEmpty())
            return 3;
    }
    return error.isEmpty() ? 0 : 1;
}
