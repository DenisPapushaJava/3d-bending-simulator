#include "bending/application/Thickness.h"
#include "bending/cad/BendingDrawingBuilder.h"
#include "bending/cad/FlatPatternBuilder.h"
#include "bending/cad/SingleBendPreviewBuilder.h"
#include "bending/core/ToolGeometry.h"
#include "bending/project/ProjectFiles.h"
#include "bending/project/ToolCatalog.h"
#include "bending/ui/DrawingSheet.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
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
std::filesystem::path native(const QString &s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toUtf8().constData());
#endif
}
core::ToolPreset preset(const core::ToolCatalog &tools, const char *name) {
    const auto found = std::find_if(tools.begin(), tools.end(),
                                    [&](const auto &tool) { return tool.code == name; });
    require(found != tools.end(), "Missing starter tool");
    auto copy = *found;
    copy.segments.reset();
    return copy;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    try {
        const auto directory = QString::fromUtf8(BENDING_STARTER_EXAMPLE_DIR);
        QFile manifest(QString::fromUtf8(BENDING_STARTER_EXAMPLE_MANIFEST));
        require(manifest.open(QIODevice::ReadOnly), "Manifest missing");
        const auto parts =
            QJsonDocument::fromJson(manifest.readAll()).object().value("parts").toArray();
        require(parts.size() == 5, "Starter examples not curated");
        require(QDir(directory).entryList({"*.step", "*.stp"}, QDir::Files).size() == parts.size(),
                "Test fixtures leaked into runtime");
        const auto source = project::readStarterToolCatalog();
        require(std::holds_alternative<core::ToolCatalog>(source), "Starter profiles unavailable");
        const auto tools = std::get<core::ToolCatalog>(source);
        const core::UnfoldSettings settings{core::SheetMaterial::MildSteel, .45, 1};
        application::DocumentPtr clearance;
        project::ProjectFiles importer;
        for (const auto &value : parts) {
            const auto part = value.toObject();
            const auto file = part.value("file").toString();
            const auto document =
                application::resultDocument(importer.load(native(directory + "/" + file), {}, {}));
            require(document != nullptr, "Example import failed");
            const auto original = std::string(document->sourceStep());
            const auto expected = static_cast<std::size_t>(part.value("bends").toInt());
            const auto &analysis = document->metadata().bendAnalysis;
            const auto t = application::suggestThickness(document->metadata().thickness);
            require(analysis.bends.size() == expected && t &&
                        std::abs(*t - part.value("thicknessMm").toDouble()) < 1e-5,
                    file.toStdString() + ": recognition/thickness changed");
            const auto result = cad::FlatPatternBuilder{}.build(document, {settings, {}}, {});
            if (const auto *error = std::get_if<application::FlatPatternError>(&result))
                throw std::runtime_error(file.toStdString() + ": " + error->detail);
            const auto flat = std::get<application::FlatPatternPtr>(result);
            require(flat && flat->info().bends.size() == expected,
                    "Example flat pattern incomplete");
            application::DrawingRequest request;
            request.unfoldSettings = settings;
            request.preparedFlatPattern = flat;
            const auto drawingResult = cad::BendingDrawingBuilder{}.build(document, request, {});
            if (const auto *error = std::get_if<application::DrawingError>(&drawingResult))
                throw std::runtime_error(file.toStdString() + ": " + error->detail);
            const auto drawing = std::get<application::BendingDrawingPtr>(drawingResult);
            require(drawing && drawing->flatPattern && !drawing->views[3].paths.empty(),
                    "Example drawing lost flat/isometric view");
            if (expected > 8) {
                require(drawing->sections.size() == 2 && drawing->sections[1].repeatCount == 4 &&
                            drawing->sections[1].view.angles.size() == 4 &&
                            drawing->sections[1].view.dimensionedBends.size() == expected &&
                            drawing->flatPattern->repeatedBendGroups.size() == 4,
                        "Starter panel lost its verified repeated wave");
            }
            const auto pages = ui::DrawingSheet::pageCount(*drawing);
            const auto coverage = ui::DrawingSheet::coverage(*drawing);
            require(pages > 0 && coverage.dimensioned == expected,
                    "Example lacks dimensioned bends");
            const auto audit = qEnvironmentVariable("BENDING_EXAMPLE_AUDIT");
            if (!audit.isEmpty())
                QDir().mkpath(audit);
            for (int page = 0; page < pages; ++page) {
                std::cout << "Render " << file.toStdString() << " page " << page + 1 << "/" << pages
                          << std::endl;
                const auto image = ui::DrawingSheet::preview(*drawing, {}, page);
                require(!image.isNull(), "Drawing page failed");
                if (!audit.isEmpty())
                    require(image.save(audit + "/" + part.value("id").toString() +
                                       QString("-%1.png").arg(page + 1)),
                            "Drawing audit failed");
            }
            if (expected > 8) {
                require(pages == 2 && temporary.isValid(), "Panel layout lost its two sheets");
                ui::DrawingTitle title;
                title.name = part.value("title").toString();
                title.designation = QStringLiteral("wall-panel-1000mm");
                title.material = QStringLiteral("Сталь · t1,5 · K0,45");
                const auto output = (audit.isEmpty() ? temporary.path() : audit) +
                                    QStringLiteral("/wall-panel-1000mm.pdf");
                require(ui::DrawingSheet::writePdf(output, *drawing, title).isEmpty(),
                        "Panel PDF export failed");
            }
            require(document->sourceStep() == original, "Example validation changed the source");
            if (part.value("id") == "clearance")
                clearance = document;
            if (part.value("id") == "profiled") {
                core::ToolAssignment assignment{
                    preset(tools, "ST1405S"), preset(tools, "2067-1"), 3, 10, "", true};
                assignment.punchReversed =
                    core::defaultPunchOrientation(assignment.punch).value_or(false);
                const auto &bend = analysis.bends.front();
                const auto cycle = cad::SingleBendPreviewBuilder{}.buildAssignedToolMotion(
                    document, 0, {bend.angleRadians, bend.innerRadiusMm}, assignment, {}, 3);
                if (const auto *error = std::get_if<application::BendPreviewError>(&cycle))
                    throw std::runtime_error("Starter angle motion: " + error->detail);
                const auto preview = std::get<application::BendPreviewPtr>(cycle);
                require(preview && preview->pressMotion() && preview->frameCount() == 61,
                        "First starter example has no checked tool cycle");
            }
            std::cout << file.toStdString() << ": bends=" << expected << " t=" << *t
                      << " flat=" << flat->info().bends.size() << " drawing_pages=" << pages
                      << std::endl;
        }
        // Actual starter punches, one matrix: compare their bodies without a machine fixture.
        for (const auto *punchName : {"ST1405S", "ST1701S"}) {
            auto punch = preset(tools, punchName), die = preset(tools, "2067-1");
            core::ToolAssignment assignment{punch, die, 0, 10, "", true};
            application::RouteDraftRequest request;
            request.step = 2;
            request.withTools = true;
            const auto &bends = clearance->metadata().bendAnalysis.bends;
            for (std::size_t i = 0; i < bends.size(); ++i)
                request.operations.push_back({"B" + std::to_string(i + 1),
                                              i,
                                              {bends[i].angleRadians, bends[i].innerRadiusMm},
                                              core::toolParameters(assignment),
                                              assignment});
            const auto result =
                cad::SingleBendPreviewBuilder{}.buildRouteDraft(clearance, request, {});
            if (const auto *error = std::get_if<application::BendRouteError>(&result))
                throw std::runtime_error(error->detail);
            const auto draft = std::get<application::RouteDraftPtr>(result);
            bool collision = false;
            require(draft->checkedToolPositions == 21 && !draft->toolLengths,
                    "Collision example not fully checked");
            for (std::size_t f = 0; f < draft->visual->frameCount(); ++f)
                collision |= draft->visual->collisionAtFrame(f).has_value();
            std::cout << punchName << " / 2067-1 V16: collision=" << collision << std::endl;
            require(collision == (std::string(punchName) == "ST1701S"),
                    "Comparison does not demonstrate different punch clearance");
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
