#include "bending/application/ToolLibrary.h"
#include "bending/core/ToolGeometry.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace bending::application {
namespace {
bool supportedProfile(const core::ToolPreset &tool) {
    if (!core::hasLibraryProfile(tool))
        return false;
    if (tool.kind == core::ToolKind::Punch) {
        const auto p = core::detectPunchProfile(*tool.drawing);
        return p && tool.noseRadiusMm && std::abs(p->noseRadiusMm - *tool.noseRadiusMm) <= 1e-5 &&
               std::abs(p->angleRadians - tool.angleRadians) < 1e-6;
    }
    for (std::size_t g = 0; g < tool.openingsMm.size(); ++g) {
        const auto d = core::activeDieDrawing(tool, g);
        const auto settings = core::dieGroove(tool, g);
        if (!d || !settings)
            return false;
        const auto grooves = core::detectDieGrooves(*d);
        if (std::none_of(grooves.begin(), grooves.end(), [&](const auto &detected) {
                return std::abs(detected.settings.rotationRadians) < 1e-6 &&
                       std::abs(detected.openingMm - tool.openingsMm[g]) <= 1e-5 &&
                       std::abs(detected.settings.angleRadians - settings->angleRadians) < 1e-6;
            }))
            return false;
    }
    return !tool.openingsMm.empty();
}
void refreshReadiness(core::ToolPreset &tool) {
    // Only an explicit workshop save completes an inventory-only record. Do not
    // migrate on read or infer a length from the DXF cross-section or a template.
    if (tool.lengthMm == 0 && tool.segments)
        for (const auto &section : *tool.segments)
            if (section.shape == core::SegmentShape::Straight && section.quantity > 0)
                tool.lengthMm = std::max(tool.lengthMm, section.lengthMm);
    auto candidate = tool;
    candidate.parametersConfirmed = true;
    tool.parametersConfirmed = core::validToolPreset(candidate) && supportedProfile(candidate);
}
} // namespace
bool validToolLibrary(const ToolLibraryData &data) {
    if (!core::validToolCatalog(data.tools) || !core::validToolCatalog(data.archived))
        return false;
    std::unordered_set<std::string> ids;
    for (const auto *catalog : {&data.tools, &data.archived})
        for (const auto &tool : *catalog)
            if (!ids.insert(tool.id).second)
                return false;
    return true;
}
ToolLibrary::ToolLibrary(core::ToolCatalog initial, std::filesystem::path path,
                         ToolLibraryStorage storage, core::ToolCatalog starter)
    : path_(std::move(path)), storage_(std::move(storage)), starter_(std::move(starter)) {
    snapshot_.data.tools = std::move(initial);
    if (!storage_) {
        error_ = "Хранилище библиотеки недоступно.";
        return;
    }
    auto loaded = storage_.read(path_);
    if (const auto *error = std::get_if<std::string>(&loaded)) {
        error_ = *error;
        return;
    }
    auto saved = std::get<ToolLibrarySnapshot>(std::move(loaded));
    snapshot_.revision = std::move(saved.revision);
    snapshot_.exists = saved.exists;
    // Normal starter-enabled startup can recover the owned workshop even if
    // preferences did not load it. An explicit external catalogue has no starter.
    if (!starter_.empty() && saved.exists && snapshot_.data.tools.empty())
        snapshot_.data.tools = std::move(saved.data.tools);
    snapshot_.data.archived = std::move(saved.data.archived);
    // An explicitly loaded external catalogue remains the current workshop until saved.
    // Opening a project never feeds its captured tools into this constructor.
    std::erase_if(snapshot_.data.archived, [&](const auto &tool) {
        return std::any_of(snapshot_.data.tools.begin(), snapshot_.data.tools.end(),
                           [&](const auto &active) { return active.id == tool.id; });
    });
    if (!snapshot_.exists && snapshot_.data.tools.empty() && !starter_.empty()) {
        if (!core::validToolCatalog(starter_) ||
            std::any_of(starter_.begin(), starter_.end(), [](const auto &tool) {
                return !tool.parametersConfirmed || tool.teaching || !supportedProfile(tool);
            })) {
            error_ = "Некорректная стартовая библиотека. Пользовательские данные не изменены.";
            return;
        }
        auto tools = starter_;
        for (auto &tool : tools)
            tool.segments.reset(); // Unknown stock retains the nominal whole-tool mode.
        error_ = commit({std::move(tools), {}});
    }
}
std::vector<core::ToolSegment> ToolLibrary::sectionTemplate(const core::ToolPreset &tool) const {
    if (!tool.drawing || tool.segments)
        return {};
    for (const auto &reference : starter_)
        if (reference.kind == tool.kind && reference.drawing && reference.segments &&
            reference.drawing->sha256 == tool.drawing->sha256 &&
            reference.drawing->millimeters == tool.drawing->millimeters &&
            reference.drawing->edges == tool.drawing->edges) {
            auto rows = *reference.segments;
            for (auto &row : rows)
                row.quantity = 0;
            return rows;
        }
    return {};
}
std::string ToolLibrary::commit(ToolLibraryData next) {
    if (!error_.empty())
        return error_;
    if (!validToolLibrary(next))
        return "Некорректная библиотека или превышен предел 512 записей в разделе.";
    if (next == snapshot_.data)
        return {};
    auto result = storage_.write(path_, next, snapshot_.revision);
    if (const auto *error = std::get_if<std::string>(&result))
        return *error;
    snapshot_ = std::get<ToolLibrarySnapshot>(std::move(result));
    return {};
}
std::string ToolLibrary::upsert(core::ToolPreset tool) {
    if (!core::hasLibraryProfile(tool))
        return "Для библиотеки нужен полный контур DXF в миллиметрах.";
    refreshReadiness(tool);
    auto next = snapshot_.data;
    auto found = std::find_if(next.tools.begin(), next.tools.end(),
                              [&](const auto &t) { return t.id == tool.id; });
    if (found == next.tools.end())
        next.tools.push_back(tool);
    else
        *found = tool;
    std::erase_if(next.archived, [&](const auto &t) { return t.id == tool.id; });
    return commit(std::move(next));
}
std::string ToolLibrary::replace(core::ToolCatalog catalog) {
    auto next = snapshot_.data;
    next.tools = std::move(catalog);
    if (std::any_of(next.tools.begin(), next.tools.end(),
                    [](const auto &t) { return !core::hasLibraryProfile(t); }))
        return "В библиотеку можно добавлять только профили DXF.";
    for (const auto &old : snapshot_.data.tools)
        if (std::none_of(next.tools.begin(), next.tools.end(),
                         [&](const auto &t) { return t.id == old.id; }))
            next.archived.push_back(old);
    std::erase_if(next.archived, [&](const auto &t) {
        return std::any_of(next.tools.begin(), next.tools.end(),
                           [&](const auto &a) { return a.id == t.id; });
    });
    return commit(std::move(next));
}
std::string ToolLibrary::archive(const std::string &id) {
    auto next = snapshot_.data;
    const auto found = std::find_if(next.tools.begin(), next.tools.end(),
                                    [&](const auto &t) { return t.id == id; });
    if (found == next.tools.end())
        return "Инструмент больше не найден в библиотеке.";
    next.archived.push_back(*found);
    next.tools.erase(found);
    return commit(std::move(next));
}
std::string ToolLibrary::restore(const std::string &id) {
    auto next = snapshot_.data;
    const auto found = std::find_if(next.archived.begin(), next.archived.end(),
                                    [&](const auto &t) { return t.id == id; });
    if (found == next.archived.end())
        return "Инструмент больше не найден в удалённых.";
    if (!core::hasLibraryProfile(*found))
        return "Для восстановления нужен сохранённый профиль DXF.";
    next.tools.push_back(*found);
    next.archived.erase(found);
    return commit(std::move(next));
}
std::string ToolLibrary::purge(const std::string &id) {
    auto next = snapshot_.data;
    std::erase_if(next.archived, [&](const auto &t) { return t.id == id; });
    return commit(std::move(next));
}
} // namespace bending::application
