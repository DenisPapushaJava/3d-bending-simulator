#include "bending/application/ToolLibrary.h"
#include <algorithm>
#include <unordered_set>

namespace bending::application {
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
                         ToolLibraryStorage storage)
    : path_(std::move(path)), storage_(std::move(storage)) {
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
    snapshot_.data.archived = std::move(saved.data.archived);
    // An explicitly loaded external catalogue remains the current workshop until saved.
    // Opening a project never feeds its captured tools into this constructor.
    std::erase_if(snapshot_.data.archived, [&](const auto &tool) {
        return std::any_of(snapshot_.data.tools.begin(), snapshot_.data.tools.end(),
                           [&](const auto &active) { return active.id == tool.id; });
    });
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
