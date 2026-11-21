#include "bending/application/DocumentSession.h"

#include <algorithm>
#include <set>
#include <utility>

namespace bending::application {
std::vector<std::size_t> DocumentSession::orderedBends() const {
    std::vector<std::size_t> result;
    for (std::size_t entry = 0; entry < bends_.size(); ++entry)
        for (std::size_t i = 0; i < bendBindings_.byRecognizedBend.size(); ++i)
            if (bendBindings_.byRecognizedBend[i] == entry)
                result.push_back(i);
    return result;
}
std::vector<BendStroke> DocumentSession::bendStrokes(std::span<const std::size_t> order) const {
    if (!document_)
        return {};
    const auto &analysis = document_->metadata().bendAnalysis;
    std::vector<unsigned> partitions(analysis.bends.size());
    for (const auto index : order)
        if (const auto *entry = bend(index))
            partitions[index] = entry->strokePartition;
    return routeBendStrokes(analysis, order, true, partitions);
}
bool DocumentSession::canSplitStroke(std::span<const std::size_t> selected,
                                     std::span<const std::size_t> remainder) const {
    if (!document_ || selected.empty() || remainder.empty())
        return false;
    const auto &zones = document_->metadata().bendAnalysis.bends;
    if (selected.front() >= zones.size())
        return false;
    const auto &first = zones[selected.front()];
    if (!first.interruptedStroke)
        return false;
    using Wall = std::array<std::size_t, 2>;
    const auto wall = [&](std::size_t index, std::size_t side) {
        const auto &faces = zones[index].flangeFaces;
        return Wall{faces[side * 2], faces[side * 2 + 1]};
    };
    for (std::size_t side = 0; side < 2; ++side) {
        const auto fixed = wall(selected.front(), side);
        const auto moving = [&](std::size_t index) -> std::optional<Wall> {
            if (!bend(index) || zones[index].interruptedStroke != first.interruptedStroke)
                return {};
            if (wall(index, 0) == fixed)
                return wall(index, 1);
            if (wall(index, 1) == fixed)
                return wall(index, 0);
            return {};
        };
        std::set<Wall> selectedWalls;
        for (const auto index : selected)
            if (const auto flange = moving(index))
                selectedWalls.insert(*flange);
        if (!selectedWalls.empty() &&
            std::all_of(selected.begin(), selected.end(),
                        [&](auto index) { return moving(index).has_value(); }) &&
            std::all_of(remainder.begin(), remainder.end(), [&](auto index) {
                const auto flange = moving(index);
                return flange && !selectedWalls.contains(*flange);
            }))
            return true;
    }
    return false;
}
bool DocumentSession::splitStroke(const BendStroke &stroke, const BendStroke &selected) {
    if (active_ || stroke.empty())
        return false;
    const auto strokes = bendStrokes(orderedBends());
    const auto current = std::find_if(strokes.begin(), strokes.end(), [&](const auto &group) {
        return std::is_permutation(group.begin(), group.end(), stroke.begin(), stroke.end());
    });
    const std::set<std::size_t> chosen(selected.begin(), selected.end());
    if (current == strokes.end() || chosen.size() != selected.size() ||
        !std::all_of(chosen.begin(), chosen.end(), [&](auto index) {
            return std::find(stroke.begin(), stroke.end(), index) != stroke.end();
        }))
        return false;
    BendStroke remainder;
    for (const auto index : stroke)
        if (!chosen.contains(index))
            remainder.push_back(index);
    if (!canSplitStroke(selected, remainder))
        return false;
    std::array<bool, 513> used{};
    for (const auto &entry : bends_)
        used[entry.strokePartition] = true;
    const auto available = std::find(used.begin() + 1, used.end(), false);
    if (available == used.end())
        return false;
    const auto partition = static_cast<unsigned>(available - used.begin());
    const auto originalPartition = bend(remainder.front())->strokePartition;
    if (!originalPartition) {
        const auto remaining = std::find(available + 1, used.end(), false);
        if (remaining == used.end())
            return false;
        for (const auto index : remainder)
            bends_[*bendBindings_.byRecognizedBend[index]].strokePartition =
                static_cast<unsigned>(remaining - used.begin());
    }
    for (const auto index : selected)
        bends_[*bendBindings_.byRecognizedBend[index]].strokePartition = partition;
    ++editRevision_;
    modified_ = true;
    return true;
}
bool DocumentSession::mergeStrokes(const BendStroke &zones, std::size_t setupSource) {
    const auto *selected = bend(setupSource);
    if (active_ || !selected || !selected->strokePartition || zones.empty())
        return false;
    const auto &bends = document_->metadata().bendAnalysis.bends;
    const auto group = bends[setupSource].interruptedStroke;
    const std::set<std::size_t> wanted(zones.begin(), zones.end());
    if (!group || wanted.size() != zones.size() || !wanted.contains(setupSource))
        return false;
    std::size_t matched = 0, count = 0, total = 0;
    for (const auto &stroke : bendStrokes(orderedBends())) {
        if (bends[stroke.front()].interruptedStroke != group)
            continue;
        total += stroke.size();
        const auto members = static_cast<std::size_t>(std::count_if(
            stroke.begin(), stroke.end(), [&](auto index) { return wanted.contains(index); }));
        if (!members)
            continue;
        if (members != stroke.size())
            return false;
        matched += members;
        ++count;
    }
    if (count < 2 || matched != zones.size())
        return false;
    const auto settings = *selected;
    for (const auto index : zones) {
        auto &entry = bends_[*bendBindings_.byRecognizedBend[index]];
        entry.strokePartition = matched == total ? 0 : settings.strokePartition;
        entry.tools = settings.tools;
        entry.catalogTools = settings.catalogTools;
        entry.workpieceReversed = settings.workpieceReversed;
        entry.workpieceOffsetMm = settings.workpieceOffsetMm;
        entry.target = settings.target;
        entry.review = settings.review;
    }
    ++editRevision_;
    modified_ = true;
    return true;
}
bool DocumentSession::setBendOrder(const std::vector<std::string> &ids) {
    if (!document_ || active_ || ids.size() != bendBindings_.byRecognizedBend.size() ||
        std::set<std::string>(ids.begin(), ids.end()).size() != ids.size())
        return false;
    std::vector<BendPreparation> reordered;
    for (const auto &id : ids) {
        const auto found =
            std::find_if(bends_.begin(), bends_.end(), [&](const auto &b) { return b.id == id; });
        if (found == bends_.end() ||
            std::none_of(bendBindings_.byRecognizedBend.begin(),
                         bendBindings_.byRecognizedBend.end(), [&](auto binding) {
                             return binding &&
                                    *binding == static_cast<std::size_t>(found - bends_.begin());
                         }))
            return false;
        reordered.push_back(*found);
    }
    for (const auto &entry : bends_)
        if (std::find(ids.begin(), ids.end(), entry.id) == ids.end())
            reordered.push_back(entry);
    bool changed = false;
    for (std::size_t i = 0; i < bends_.size(); ++i)
        changed |= bends_[i].id != reordered[i].id;
    if (!changed)
        return true;
    bends_ = std::move(reordered);
    bendBindings_ = restoreBendPreparations(document_->metadata().bendAnalysis, bends_);
    ++editRevision_;
    modified_ = true;
    return true;
}
std::optional<std::string> projectSettingsError(const OpenedProject &project) {
    if (!project.document || !validBendPreparations(project.bends) ||
        !core::validUnfoldSettings(project.unfold)) {
        return "Invalid saved bend preparations";
    }
    if (project.drawing && !validProjectDrawingSettings(*project.drawing))
        return "Invalid saved drawing settings";
    if (project.manualThicknessMm) {
        const auto validation =
            validateThickness(*project.manualThicknessMm, project.document->metadata().thickness);
        if (validation == ThicknessValidation::Invalid ||
            validation == ThicknessValidation::ConflictsWithGeometry) {
            return "Saved thickness conflicts with geometry";
        }
    }
    return {};
}
ImportRequest DocumentSession::beginImport(std::filesystem::path source) {
    active_ = ImportRequest{++nextRequest_, revision_, std::move(source)};
    error_.reset();
    phase_ = ImportPhase::Waiting;
    return *active_;
}

bool DocumentSession::isCurrent(const ImportRequest &request) const noexcept {
    return active_ && active_->id == request.id && request.baseRevision == revision_ &&
           active_->baseRevision == request.baseRevision;
}

bool DocumentSession::updateProgress(const ImportRequest &request, ImportPhase phase) {
    if (!isCurrent(request)) {
        return false;
    }
    phase_ = phase;
    return true;
}

bool DocumentSession::complete(const ImportRequest &request, ImportResult result) {
    if (!isCurrent(request)) {
        return false;
    }
    active_.reset();
    if (auto document = resultDocument(result)) {
        const auto *project = std::get_if<OpenedProject>(&result);
        if (project) {
            if (const auto error = projectSettingsError(*project)) {
                error_ = ImportError{ImportErrorCode::InvalidProject, *error, {}};
                return true;
            }
        }
        auto bends = project ? project->bends : std::vector<BendPreparation>{};
        auto bindings = restoreBendPreparations(document->metadata().bendAnalysis, bends);
        document_ = std::move(document);
        bends_ = std::move(bends);
        bendBindings_ = std::move(bindings);
        ++revision_;
        ++editRevision_;
        manualThickness_ = project ? project->manualThicknessMm : std::nullopt;
        unfold_ = project ? project->unfold : core::UnfoldSettings{};
        drawing_ = project ? project->drawing : std::nullopt;
        ++drawingRevision_;
        projectPath_ = project ? project->path : std::filesystem::path{};
        modified_ = false;
        error_.reset();
    } else if (const auto *error = std::get_if<ImportError>(&result)) {
        if (error->code != ImportErrorCode::Cancelled) {
            error_ = *error;
        }
    } else {
        error_ = ImportError{
            ImportErrorCode::UnexpectedFailure, "Importer returned a null document", {}};
    }
    return true;
}

void DocumentSession::cancel() {
    active_.reset();
    error_.reset();
}

void DocumentSession::close() {
    cancel();
    document_.reset();
    ++revision_;
    ++editRevision_;
    manualThickness_.reset();
    projectPath_.clear();
    modified_ = false;
    bends_.clear();
    bendBindings_ = {};
    unfold_ = {};
    drawing_.reset();
    ++drawingRevision_;
}

const BendPreparation *DocumentSession::bend(std::size_t recognizedIndex) const {
    if (recognizedIndex >= bendBindings_.byRecognizedBend.size() ||
        !bendBindings_.byRecognizedBend[recognizedIndex]) {
        return nullptr;
    }
    return &bends_[*bendBindings_.byRecognizedBend[recognizedIndex]];
}
bool DocumentSession::setBendReview(std::size_t recognizedIndex, BendReview review) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry ||
        (review != BendReview::Pending && review != BendReview::Confirmed &&
         review != BendReview::Excluded && review != BendReview::Automatic)) {
        return false;
    }
    if (entry->review != review) {
        bends_[*bendBindings_.byRecognizedBend[recognizedIndex]].review = review;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}
bool DocumentSession::setBendTarget(std::size_t recognizedIndex,
                                    std::optional<core::BendTarget> target) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry || !included(entry->review) ||
        (target && !core::validBendTarget(*target))) {
        return false;
    }
    if (entry->target != target) {
        bends_[*bendBindings_.byRecognizedBend[recognizedIndex]].target = target;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

bool DocumentSession::setBendWorkpieceOrientation(std::size_t index, bool reversed) {
    const auto *entry = bend(index);
    if (!document_ || active_ || !entry || !included(entry->review))
        return false;
    if (entry->workpieceReversed != reversed) {
        bends_[*bendBindings_.byRecognizedBend[index]].workpieceReversed = reversed;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

bool DocumentSession::setBendWorkpieceOffset(std::size_t index, double offsetMm) {
    const auto *entry = bend(index);
    if (!document_ || active_ || !entry || !included(entry->review) ||
        !validWorkpieceOffset(offsetMm))
        return false;
    if (entry->workpieceOffsetMm != offsetMm) {
        bends_[*bendBindings_.byRecognizedBend[index]].workpieceOffsetMm = offsetMm;
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

bool DocumentSession::setBendTools(std::size_t recognizedIndex,
                                   std::optional<core::PressTools> tools) {
    const auto *entry = bend(recognizedIndex);
    if (!document_ || active_ || !entry || !included(entry->review) ||
        (tools && !core::validPressTools(*tools))) {
        return false;
    }
    if (entry->tools != tools || entry->catalogTools) {
        auto &changed = bends_[*bendBindings_.byRecognizedBend[recognizedIndex]];
        changed.tools = tools;
        changed.catalogTools.reset();
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

std::optional<core::BendTarget> DocumentSession::bendTarget(std::size_t index) const {
    const auto *entry = bend(index);
    if (!document_ || !entry || !included(entry->review) ||
        index >= document_->metadata().bendAnalysis.bends.size()) {
        return {};
    }
    if (entry->target) {
        return entry->target;
    }
    const auto &bend = document_->metadata().bendAnalysis.bends[index];
    const core::BendTarget target{bend.angleRadians, bend.innerRadiusMm};
    return core::validBendTarget(target) ? std::optional{target} : std::nullopt;
}
bool DocumentSession::setBendToolAssignment(std::size_t index, core::ToolAssignment assignment) {
    const auto *entry = bend(index);
    const auto parameters = core::toolParameters(assignment);
    if (!document_ || active_ || !entry || !included(entry->review) || !parameters) {
        return false;
    }
    if (entry->catalogTools != assignment || entry->tools != parameters) {
        auto &changed = bends_[*bendBindings_.byRecognizedBend[index]];
        changed.tools = parameters;
        changed.catalogTools = std::move(assignment);
        modified_ = true;
        ++editRevision_;
    }
    return true;
}

bool DocumentSession::setBendToolAssignments(const std::vector<BendToolUpdate> &updates) {
    if (!document_ || active_)
        return false;
    auto candidate = *this;
    std::vector<std::size_t> seen;
    for (const auto &update : updates) {
        if (std::find(seen.begin(), seen.end(), update.bendIndex) != seen.end())
            return false;
        seen.push_back(update.bendIndex);
        if (update.assignment && core::toolParameters(*update.assignment) != update.tools)
            return false;
        if (update.assignment
                ? !candidate.setBendToolAssignment(update.bendIndex, *update.assignment)
                : !candidate.setBendTools(update.bendIndex, update.tools))
            return false;
    }
    if (candidate.editRevision_ != editRevision_) {
        candidate.editRevision_ = editRevision_ + 1;
        *this = std::move(candidate);
    }
    return true;
}

ThicknessValidation DocumentSession::setManualThickness(double valueMm) {
    if (!document_ || active_) {
        return ThicknessValidation::Invalid;
    }
    const auto validation = validateThickness(valueMm, document_->metadata().thickness);
    if (validation != ThicknessValidation::Invalid &&
        validation != ThicknessValidation::ConflictsWithGeometry && manualThickness_ != valueMm) {
        manualThickness_ = valueMm;
        modified_ = true;
        ++editRevision_;
    }
    return validation;
}
void DocumentSession::useAutomaticThickness() {
    if (document_ && !active_ && manualThickness_) {
        manualThickness_.reset();
        modified_ = true;
        ++editRevision_;
    }
}
bool DocumentSession::setDrawingSettings(std::uint64_t revision, ProjectDrawingSettings settings) {
    if (!document_ || active_ || revision != revision_ || !validProjectDrawingSettings(settings) ||
        drawing_ == settings)
        return false;
    drawing_ = std::move(settings);
    ++drawingRevision_;
    modified_ = true;
    return true;
}

bool DocumentSession::markSaved(std::uint64_t editRevision, std::filesystem::path path) {
    if (!document_ || editRevision != editRevision_) {
        return false;
    }
    projectPath_ = std::move(path);
    modified_ = false;
    return true;
}
void DocumentSession::applyUnfoldSettings(core::UnfoldSettings settings) {
    const bool categoryChanged = unfold_.material != settings.material ||
                                 unfold_.customMaterialName != settings.customMaterialName;
    if (categoryChanged && drawing_ && !drawing_->material.empty()) {
        drawing_->material.clear();
        ++drawingRevision_;
    }
    unfold_ = std::move(settings);
}
bool DocumentSession::setUnfoldSettings(core::UnfoldSettings settings) {
    if (!document_ || active_ || !core::validUnfoldSettings(settings))
        return false;
    if (unfold_ != settings) {
        applyUnfoldSettings(std::move(settings));
        ++editRevision_;
        modified_ = true;
    }
    return true;
}

bool DocumentSession::hasPartInputs() const {
    if (!document_ || !core::validUnfoldSettings(unfold_) ||
        (unfold_.material == core::SheetMaterial::Unknown && !unfold_.manualK))
        return false;
    const auto thickness =
        manualThickness_ ? manualThickness_ : suggestThickness(document_->metadata().thickness);
    if (!thickness)
        return false;
    const auto validation = validateThickness(*thickness, document_->metadata().thickness);
    return validation != ThicknessValidation::Invalid &&
           validation != ThicknessValidation::ConflictsWithGeometry;
}

bool DocumentSession::setPartInputs(std::uint64_t revision, std::uint64_t editRevision,
                                    std::optional<double> manualThicknessMm,
                                    core::UnfoldSettings settings) {
    if (!document_ || active_ || revision != revision_ || editRevision != editRevision_ ||
        !core::validUnfoldSettings(settings) ||
        (settings.material == core::SheetMaterial::Unknown && !settings.manualK))
        return false;
    const auto thickness =
        manualThicknessMm ? manualThicknessMm : suggestThickness(document_->metadata().thickness);
    if (!thickness)
        return false;
    const auto validation = validateThickness(*thickness, document_->metadata().thickness);
    if (validation == ThicknessValidation::Invalid ||
        validation == ThicknessValidation::ConflictsWithGeometry)
        return false;
    if (manualThickness_ != manualThicknessMm || unfold_ != settings) {
        manualThickness_ = manualThicknessMm;
        applyUnfoldSettings(std::move(settings));
        modified_ = true;
        ++editRevision_;
    }
    return true;
}
} // namespace bending::application
