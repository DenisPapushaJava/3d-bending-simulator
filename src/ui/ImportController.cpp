#include "bending/ui/ImportController.h"
#include "bending/core/ToolSelection.h"

#include <QPointer>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace bending::ui {
using namespace application;

class ImportController::Impl {
  public:
    Impl(ImportController *controller, std::shared_ptr<const DocumentImporter> service,
         Presenter display)
        : owner(controller), importer(std::move(service)), presenter(std::move(display)) {
        if (!importer || !presenter) {
            throw std::invalid_argument("ImportController needs an importer and presenter");
        }
        worker = new QObject;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.setObjectName(QStringLiteral("cadImportWorker"));
        thread.start();
    }

    void dispatch() {
        if (stopping || running || !pending) {
            return;
        }
        const auto request = *pending;
        pending.reset();
        stop = std::stop_source{};
        const auto token = stop.get_token();
        running = true;
        // Only this task executes in the worker. Session and presentation stay in the GUI thread.
        QMetaObject::invokeMethod(
            worker,
            [this, request, token, service = importer] {
                ImportResult result =
                    ImportError{ImportErrorCode::Cancelled, "Import cancelled", {}};
                if (!token.stop_requested()) {
                    try {
                        result = service->load(
                            request.source, token, [this, request](ImportPhase phase) {
                                QMetaObject::invokeMethod(
                                    owner,
                                    [this, request, phase] {
                                        if (!stopping && session.updateProgress(request, phase)) {
                                            emit owner->stateChanged();
                                        }
                                    },
                                    Qt::QueuedConnection);
                            });
                    } catch (const std::exception &error) {
                        result = ImportError{ImportErrorCode::UnexpectedFailure, error.what(), {}};
                    } catch (...) {
                        result = ImportError{
                            ImportErrorCode::UnexpectedFailure, "Unknown import exception", {}};
                    }
                }
                QMetaObject::invokeMethod(
                    owner,
                    [this, request, result = std::move(result)]() mutable {
                        running = false;
                        if (stopping) {
                            return;
                        }
                        if (session.isCurrent(request)) {
                            if (auto document = resultDocument(result)) {
                                try {
                                    const auto *project = std::get_if<OpenedProject>(&result);
                                    const auto invalid =
                                        project ? projectSettingsError(*project) : std::nullopt;
                                    if (invalid) {
                                        result = ImportError{
                                            ImportErrorCode::InvalidProject, *invalid, {}};
                                    } else if (const auto error =
                                                   presenter(document, session.revision() + 1);
                                               !error.isEmpty()) {
                                        result = ImportError{ImportErrorCode::PresentationFailed,
                                                             error.toStdString(),
                                                             {}};
                                    }
                                } catch (const std::exception &error) {
                                    result = ImportError{
                                        ImportErrorCode::PresentationFailed, error.what(), {}};
                                }
                            }
                            session.complete(request, std::move(result));
                        }
                        dispatch();
                        emit owner->stateChanged();
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

    ImportController *owner;
    std::shared_ptr<const DocumentImporter> importer;
    Presenter presenter;
    DocumentSession session;
    QThread thread;
    QObject *worker = nullptr;
    std::stop_source stop;
    std::optional<ImportRequest> pending;
    bool running = false;
    bool stopping = false;
    bool saving = false;
    bool preparingToolProfile = false;
    std::uint64_t toolProfileId = 0;
    std::uint64_t saveId = 0;
    QString writeError;
    BendPreviewPtr preview;
    std::optional<BendPreviewError> previewError;
    std::uint64_t previewId = 0;
    bool preparingPreview = false;
    BendRoutePtr route;
    std::optional<BendRouteError> routeError;
    std::optional<BendRouteProgress> routeProgress;
    std::uint64_t routeId = 0, routeRevision = 0, routeEditRevision = 0;
    bool preparingRoute = false;
    RouteDraftPtr draft;
    std::optional<BendRouteError> draftError;
    std::uint64_t draftId = 0;
    bool runningDraft = false;
    struct DraftJob {
        DocumentPtr document;
        RouteDraftRequest request;
        std::shared_ptr<const SingleBendPreviewBuilder> builder;
        std::uint64_t id, revision, edit;
    };
    std::optional<DraftJob> pendingDraft;
    void dispatchDraft();
    void invalidateDraft() {
        ++draftId;
        draft.reset();
        draftError.reset();
        pendingDraft.reset();
        if (runningDraft)
            stop.request_stop();
    }
    std::weak_ptr<const SingleBendPreviewBuilder> routeBuilder;
    void discardRouteCache() {
        if (const auto builder = routeBuilder.lock())
            builder->discardRouteCache();
    }
    void invalidateRoute() {
        invalidateDraft();
        ++routeId;
        route.reset();
        routeError.reset();
        routeProgress.reset();
        if (preparingRoute) {
            stop.request_stop();
            preparingRoute = false;
        }
    }
    BendingDrawingPtr drawing;
    std::optional<DrawingError> drawingError;
    std::uint64_t drawingId = 0;
    bool preparingDrawing = false;
    std::optional<DrawingRequest> drawingRequest;
    FlatPatternPtr flat;
    std::optional<FlatPatternError> flatError;
    std::optional<FlatPatternRequest> flatRequest;
    std::uint64_t flatId = 0;
    bool preparingFlat = false;
    void invalidateFlat() {
        ++flatId;
        flat.reset();
        flatError.reset();
        flatRequest.reset();
        if (preparingFlat) {
            stop.request_stop();
            preparingFlat = false;
        }
    }
    void invalidateDrawing() {
        ++drawingId;
        drawing.reset();
        drawingError.reset();
        drawingRequest.reset();
        if (preparingDrawing) {
            stop.request_stop();
            preparingDrawing = false;
        }
    }
    void invalidatePreview() {
        ++previewId;
        preview.reset();
        previewError.reset();
        if (preparingPreview) {
            stop.request_stop();
            preparingPreview = false;
        }
    }
};

void ImportController::Impl::dispatchDraft() {
    if (stopping || running || !pendingDraft)
        return;
    auto job = std::move(*pendingDraft);
    pendingDraft.reset();
    stop = std::stop_source{};
    const auto token = stop.get_token();
    running = runningDraft = true;
    QMetaObject::invokeMethod(
        worker,
        [this, job = std::move(job), token]() mutable {
            RouteDraftResult result = BendRouteError{BendRouteErrorCode::Cancelled, "Cancelled"};
            try {
                if (!token.stop_requested())
                    result = job.builder->buildRouteDraft(job.document, job.request, token);
            } catch (const std::exception &e) {
                result = BendRouteError{BendRouteErrorCode::Failed, e.what()};
            } catch (...) {
                result = BendRouteError{BendRouteErrorCode::Failed, "Unexpected draft failure"};
            }
            QMetaObject::invokeMethod(
                owner,
                [this, job = std::move(job), token, result = std::move(result)] {
                    running = runningDraft = false;
                    if (stopping)
                        return;
                    if (job.id == draftId && job.revision == session.revision() &&
                        job.edit == session.editRevision() && job.document == session.document() &&
                        !session.activeRequest() && !token.stop_requested()) {
                        if (const auto *value = std::get_if<RouteDraftPtr>(&result)) {
                            const auto p = *value ? (*value)->visual : nullptr;
                            bool valid =
                                *value && (*value)->source == job.document &&
                                (*value)->request == job.request && p && p->unverifiedDraft() &&
                                !p->punchRetraction() && !p->pressMotion() &&
                                (!p->backgauge() ||
                                 p->backgauge()->addedRetractionPositions <= 4) &&
                                p->frameCount() ==
                                    (job.request.step ? 21u : 1u) +
                                        (p->backgauge() ? p->backgauge()->addedRetractionPositions
                                                        : 0) &&
                                (!p->toolSetup() ||
                                 (*value)->checkedToolPositions == p->frameCount());
                            if (valid && !job.request.operations.empty()) {
                                const auto &op =
                                    job.request
                                        .operations[job.request.step ? job.request.step - 1 : 0];
                                const bool tools = job.request.withTools && op.tools &&
                                                   core::validPressTools(*op.tools);
                                valid = bool(p->toolSetup()) == tools &&
                                        p->workpieceReversed() == op.workpieceReversed;
                                if (tools)
                                    valid =
                                        valid && p->toolSetup() &&
                                        p->toolSetup()->tools == *op.tools &&
                                        (!op.tools->backgauge ||
                                         (p->backgauge() &&
                                          p->backgauge()->setup == *op.tools->backgauge)) &&
                                        p->machineModel() == op.tools->machineModel &&
                                        p->floorChecked() == (op.tools->machineModel ==
                                                              core::MachineModel::DemoPressV3) &&
                                        p->usesDrawingPunch() ==
                                            bool(op.assignment &&
                                                 op.assignment->useDrawingGeometry &&
                                                 op.assignment->punch.drawing) &&
                                        p->usesDrawingDie() ==
                                            bool(op.assignment &&
                                                 op.assignment->useDrawingGeometry &&
                                                 op.assignment->die.drawing);
                                std::optional<BendRouteToolLengthIssue> expectedLengths;
                                if (tools) {
                                    const auto &analysis = job.document->metadata().bendAnalysis;
                                    const BendRouteToolLengthIssue lengths{
                                        analysis.bends[op.bendIndex].lengthMm,
                                        core::punchLengthMm(*op.tools), op.tools->lengthMm};
                                    if (!lengths.coversBend(analysis.lengthToleranceMm))
                                        expectedLengths = lengths;
                                }
                                valid &= (*value)->toolLengths == expectedLengths;
                            } else
                                valid = false;
                            if (valid)
                                draft = *value;
                            else
                                draftError = BendRouteError{BendRouteErrorCode::Failed,
                                                            "Invalid draft result"};
                        } else if (const auto &e = std::get<BendRouteError>(result);
                                   e.code != BendRouteErrorCode::Cancelled)
                            draftError = e;
                    }
                    dispatch();
                    dispatchDraft();
                    emit owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

ImportController::ImportController(std::shared_ptr<const DocumentImporter> importer,
                                   Presenter presenter, QObject *parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(this, std::move(importer), std::move(presenter))) {}

ImportController::~ImportController() {
    shutdown();
}

void ImportController::open(std::filesystem::path path) {
    if (impl_->stopping) {
        return;
    }
    impl_->stop.request_stop();
    impl_->discardRouteCache();
    impl_->invalidateRoute();
    impl_->invalidateFlat();
    impl_->invalidatePreview();
    impl_->invalidateDrawing();
    ++impl_->saveId;
    impl_->writeError.clear();
    impl_->pending = impl_->session.beginImport(std::move(path));
    impl_->dispatch();
    emit stateChanged();
}

void ImportController::cancel() {
    impl_->stop.request_stop();
    impl_->discardRouteCache();
    impl_->invalidateRoute();
    impl_->invalidateFlat();
    impl_->invalidatePreview();
    impl_->invalidateDrawing();
    ++impl_->saveId;
    impl_->writeError.clear();
    impl_->pending.reset();
    impl_->session.cancel();
    emit stateChanged();
}

void ImportController::closeDocument() {
    impl_->invalidateFlat();
    cancel();
    impl_->presenter({}, impl_->session.revision() + 1);
    impl_->session.close();
    emit stateChanged();
}

void ImportController::shutdown() {
    if (impl_->stopping) {
        return;
    }
    impl_->stopping = true;
    impl_->stop.request_stop();
    impl_->pending.reset();
    impl_->session.cancel();
    // No terminate(): a non-interruptible OCCT call must finish before its dependencies die.
    impl_->thread.quit();
    impl_->thread.wait();
    impl_->discardRouteCache();
}

const DocumentSession &ImportController::session() const {
    return impl_->session;
}

bool ImportController::hasWork() const {
    return impl_->running || impl_->pending.has_value() || impl_->pendingDraft.has_value();
}

bool ImportController::prepareToolProfile(ToolProfileInput input,
                                          std::shared_ptr<const ToolProfileImporter> importer,
                                          std::function<void(ToolProfileResult)> completion) {
    if (impl_->stopping || hasWork() || !importer || !completion)
        return false;
    auto *s = impl_.get();
    const auto id = ++s->toolProfileId;
    s->stop = std::stop_source{};
    const auto stop = s->stop.get_token();
    s->running = s->preparingToolProfile = true;
    QMetaObject::invokeMethod(
        s->worker,
        [s, id, input = std::move(input), importer = std::move(importer),
         completion = std::move(completion), stop]() mutable {
            ToolProfileResult result = ToolProfileError{"Импорт отменён."};
            try {
                if (!stop.stop_requested())
                    result = importer->run(input, stop);
            } catch (...) {
                result = ToolProfileError{"Не удалось обработать сечение инструмента."};
            }
            QMetaObject::invokeMethod(
                s->owner,
                [s, id, stop, result = std::move(result),
                 completion = std::move(completion)]() mutable {
                    s->running = s->preparingToolProfile = false;
                    if (s->stopping)
                        return;
                    if (id == s->toolProfileId)
                        completion(stop.stop_requested()
                                       ? ToolProfileResult{ToolProfileError{"Обработка отменена."}}
                                       : std::move(result));
                    s->dispatch();
                    emit s->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
void ImportController::cancelToolProfile() {
    if (impl_->preparingToolProfile) {
        ++impl_->toolProfileId;
        impl_->stop.request_stop();
    }
}

ThicknessValidation ImportController::setManualThickness(double valueMm) {
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setManualThickness(valueMm);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidateFlat();
        impl_->invalidatePreview();
        impl_->invalidateDrawing();
    }
    emit stateChanged();
    return result;
}
void ImportController::useAutomaticThickness() {
    const auto before = impl_->session.editRevision();
    impl_->session.useAutomaticThickness();
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidateFlat();
        impl_->invalidatePreview();
        impl_->invalidateDrawing();
    }
    emit stateChanged();
}
bool ImportController::setBendReview(std::size_t recognizedIndex, BendReview review) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setBendReview(recognizedIndex, review);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendTools(std::size_t recognizedIndex,
                                    std::optional<core::PressTools> tools) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const bool result = impl_->session.setBendTools(recognizedIndex, tools);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendToolAssignment(std::size_t index, core::ToolAssignment assignment) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const bool result = impl_->session.setBendToolAssignment(index, std::move(assignment));
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendWorkpieceOrientation(std::size_t index, bool reversed) {
    if (impl_->stopping || hasWork())
        return false;
    const auto before = impl_->session.editRevision();
    const bool result = impl_->session.setBendWorkpieceOrientation(index, reversed);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::setBendToolAssignments(
    const std::vector<application::BendToolUpdate> &updates) {
    if (impl_->stopping || hasWork())
        return false;
    const auto before = impl_->session.editRevision();
    if (!impl_->session.setBendToolAssignments(updates))
        return false;
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
        emit stateChanged();
    }
    return true;
}
bool ImportController::setAllBendPunchOrientations(
    bool reversed, std::optional<core::PressTools> sharedTools,
    std::optional<core::ToolAssignment> sharedAssignment) {
    if (impl_->stopping || hasWork() || !impl_->session.document())
        return false;
    auto candidate = impl_->session;
    const auto before = candidate.editRevision();
    bool assigned = false;
    for (const auto index : candidate.orderedBends()) {
        const auto *entry = candidate.bend(index);
        if (!entry || !included(entry->review))
            continue;
        auto assignment = sharedAssignment ? sharedAssignment
                          : sharedTools    ? std::nullopt
                                           : entry->catalogTools;
        auto tools = assignment    ? core::toolParameters(*assignment)
                     : sharedTools ? sharedTools
                                   : entry->tools;
        if (!tools) {
            if (assignment || sharedTools)
                return false;
            continue; // Unassigned bends remain unassigned.
        }
        assigned = true;
        const bool changed = tools->punchReversed != reversed;
        tools->punchReversed = reversed;
        if (changed && tools->machineModel == core::MachineModel::DemoPressV1)
            tools->machineModel = core::MachineModel::DemoPressV2;
        if (assignment) {
            assignment->punchReversed = reversed;
            assignment->machineModel = tools->machineModel;
        }
        if (assignment ? !candidate.setBendToolAssignment(index, std::move(*assignment))
                       : !candidate.setBendTools(index, *tools))
            return false;
    }
    if (!assigned)
        return false;
    if (before != candidate.editRevision()) {
        impl_->session = std::move(candidate);
        impl_->invalidateRoute();
        impl_->invalidatePreview();
        emit stateChanged();
    }
    return true;
}
bool ImportController::setBendTarget(std::size_t recognizedIndex,
                                     std::optional<core::BendTarget> target) {
    if (impl_->stopping || hasWork()) {
        return false;
    }
    const auto before = impl_->session.editRevision();
    const auto result = impl_->session.setBendTarget(recognizedIndex, target);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidatePreview();
    }
    emit stateChanged();
    return result;
}
bool ImportController::isSaving() const {
    return impl_->saving;
}
QString ImportController::saveError() const {
    return impl_->writeError;
}
void ImportController::saveProject(std::filesystem::path path,
                                   std::shared_ptr<const ProjectWriter> writer) {
    if (impl_->stopping || hasWork() || !impl_->session.document() || !writer) {
        return;
    }
    auto *state = impl_.get();
    const auto document = state->session.document();
    const auto manual = state->session.manualThicknessMm();
    const auto unfold = state->session.unfoldSettings();
    const auto bends = state->session.bends();
    const auto revision = state->session.editRevision();
    const auto id = ++state->saveId;
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->saving = true;
    state->writeError.clear();
    QMetaObject::invokeMethod(
        state->worker,
        [state, document, manual, unfold, bends, revision, id, stop, path = std::move(path),
         writer = std::move(writer)] {
            std::optional<std::string> error;
            try {
                error = writer->save(path, *document, manual, bends, stop, unfold);
            } catch (const std::exception &failure) {
                error = failure.what();
            } catch (...) {
                error = "Unknown project save exception";
            }
            QMetaObject::invokeMethod(
                state->owner,
                [state, revision, id, path, error] {
                    state->running = false;
                    state->saving = false;
                    if (state->stopping) {
                        return;
                    }
                    if (id == state->saveId) {
                        if (error) {
                            state->writeError = QString::fromStdString(*error);
                        } else {
                            state->session.markSaved(revision, path);
                        }
                    }
                    state->dispatch();
                    emit state->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
}
bool ImportController::prepareBendPreview(std::size_t recognizedIndex, bool fixedSecond,
                                          std::shared_ptr<const SingleBendPreviewBuilder> builder,
                                          BendPreviewMode mode, double maximumEndSupportLossMm) {
    if (impl_->stopping || hasWork() || !builder || !impl_->session.document() ||
        !validEndSupportLimitMm(maximumEndSupportLossMm)) {
        return false;
    }
    const auto *entry = impl_->session.bend(recognizedIndex);
    if (!entry || !included(entry->review) || !impl_->session.bendTarget(recognizedIndex) ||
        (mode != BendPreviewMode::Kinematic && !entry->tools)) {
        return false;
    }
    const auto &metadata = impl_->session.document()->metadata();
    if (recognizedIndex >= metadata.bendAnalysis.bends.size() ||
        (impl_->session.manualThicknessMm() &&
         std::abs(*impl_->session.manualThicknessMm() -
                  metadata.bendAnalysis.bends[recognizedIndex].thicknessMm) >
             metadata.bendAnalysis.lengthToleranceMm)) {
        return false;
    }
    if (mode != BendPreviewMode::Kinematic && entry->tools &&
        !core::standardAirBendOpening(metadata.bendAnalysis.bends[recognizedIndex].thicknessMm,
                                      entry->tools->openingMm)) {
        impl_->invalidatePreview();
        impl_->previewError = BendPreviewError{BendPreviewErrorCode::NarrowDieOpening,
                                               "V is below the current standard air-bending scope"};
        emit stateChanged();
        return false;
    }
    auto *state = impl_.get();
    state->invalidatePreview();
    if (const auto previous = state->routeBuilder.lock(); previous && previous != builder)
        previous->discardRouteCache();
    state->routeBuilder = builder;
    const bool workpieceReversed =
        mode == BendPreviewMode::Kinematic ? fixedSecond : entry->workpieceReversed;
    const auto id = state->previewId;
    const auto document = state->session.document();
    const auto revision = state->session.revision();
    const auto editRevision = state->session.editRevision();
    const auto target = *state->session.bendTarget(recognizedIndex);
    const auto tools = mode == BendPreviewMode::Kinematic ? std::nullopt : entry->tools;
    const auto assignment = mode == BendPreviewMode::Kinematic ? std::nullopt : entry->catalogTools;
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->preparingPreview = true;
    QMetaObject::invokeMethod(
        state->worker,
        [state, document, revision, editRevision, id, target, recognizedIndex, fixedSecond, stop,
         tools, assignment, mode, maximumEndSupportLossMm, workpieceReversed,
         builder = std::move(builder)] {
            BendPreviewResult result =
                BendPreviewError{BendPreviewErrorCode::Cancelled, "Cancelled"};
            try {
                result = mode == BendPreviewMode::ToolMotion
                             ? (assignment ? builder->buildAssignedToolMotion(
                                                 document, recognizedIndex, target, *assignment,
                                                 stop, maximumEndSupportLossMm, workpieceReversed)
                                           : builder->buildToolMotion(
                                                 document, recognizedIndex, target, *tools, stop,
                                                 maximumEndSupportLossMm, workpieceReversed))
                         : tools
                             ? (assignment ? builder->buildAssignedToolSetup(
                                                 document, recognizedIndex, target, *assignment,
                                                 stop, maximumEndSupportLossMm, workpieceReversed)
                                           : builder->buildToolSetup(
                                                 document, recognizedIndex, target, *tools, stop,
                                                 maximumEndSupportLossMm, workpieceReversed))
                             : builder->build(document, recognizedIndex, target, fixedSecond, stop);
            } catch (const std::exception &error) {
                result = BendPreviewError{BendPreviewErrorCode::Failed, error.what()};
            } catch (...) {
                result = BendPreviewError{BendPreviewErrorCode::Failed, "Unknown preview failure"};
            }
            QMetaObject::invokeMethod(
                state->owner,
                [state, revision, editRevision, id, mode, maximumEndSupportLossMm,
                 expectedPunch = assignment && assignment->useDrawingGeometry &&
                                 assignment->punch.drawing.has_value(),
                 expectedDie = assignment && assignment->useDrawingGeometry &&
                               assignment->die.drawing.has_value(),
                 expectedMachine = tools ? tools->machineModel : core::MachineModel::None,
                 expectedGauge = tools ? tools->backgauge : std::nullopt,
                 expectedReversed = tools && tools->punchReversed, workpieceReversed,
                 result = std::move(result)] {
                    state->running = false;
                    state->preparingPreview = false;
                    if (state->stopping) {
                        return;
                    }
                    if (id == state->previewId && revision == state->session.revision() &&
                        editRevision == state->session.editRevision() &&
                        !state->session.activeRequest()) {
                        if (const auto *preview = std::get_if<BendPreviewPtr>(&result)) {
                            bool matchingMode =
                                *preview && !(*preview)->unverifiedDraft() &&
                                (mode == BendPreviewMode::Kinematic ||
                                 (*preview)->workpieceReversed() == workpieceReversed) &&
                                (*preview)->machineModel() == expectedMachine &&
                                (*preview)->floorChecked() ==
                                    (expectedMachine == core::MachineModel::DemoPressV3) &&
                                (!(*preview)->toolSetup() ||
                                 ((*preview)->toolSetup()->tools.punchReversed ==
                                      expectedReversed &&
                                  (*preview)->toolSetup()->tools.backgauge == expectedGauge)) &&
                                (*preview)->usesDrawingPunch() == expectedPunch &&
                                (*preview)->usesDrawingDie() == expectedDie &&
                                (!expectedGauge ||
                                 ((*preview)->toolSetup() &&
                                  (*preview)->toolSetup()->tools.backgauge == expectedGauge &&
                                  (*preview)->backgauge() &&
                                  (*preview)->backgauge()->setup == *expectedGauge &&
                                  (*preview)->backgauge()->status == BackgaugeStatus::Checked)) &&
                                (mode == BendPreviewMode::ToolSetup
                                     ? (*preview)->toolSetup() && !(*preview)->pressMotion() &&
                                           (*preview)->frameCount() == 1
                                 : mode == BendPreviewMode::ToolMotion
                                     ? (*preview)->toolSetup() && (*preview)->pressMotion() &&
                                           (*preview)->frameCount() >= 2 &&
                                           (*preview)->pressState(0) &&
                                           (*preview)->pressState(0)->phase ==
                                               simulation::PressPhase::Approach &&
                                           (*preview)->pressState((*preview)->frameCount() - 1) &&
                                           (*preview)
                                                   ->pressState((*preview)->frameCount() - 1)
                                                   ->phase == simulation::PressPhase::Retracting &&
                                           (*preview)->punchRetraction() &&
                                           std::isfinite((*preview)->punchRetraction()->liftMm) &&
                                           (*preview)->punchRetraction()->liftMm > 0 &&
                                           std::isfinite(
                                               (*preview)->punchRetraction()->finalClearanceMm) &&
                                           (*preview)->punchRetraction()->finalClearanceMm > 0
                                     : !(*preview)->toolSetup() && !(*preview)->pressMotion() &&
                                           (*preview)->frameCount() >= 2);
                            if (*preview)
                                for (std::size_t f = 0; f < (*preview)->frameCount(); ++f)
                                    matchingMode &= !(*preview)->machineCollisionAtFrame(f);
                            if (*preview && (*preview)->partialEndSupport())
                                matchingMode &= withinEndSupportLimit(
                                    *(*preview)->partialEndSupport(), maximumEndSupportLossMm,
                                    state->session.document()
                                                ->metadata()
                                                .bendAnalysis.lengthToleranceMm *
                                            4 +
                                        1.e-8);
                            if (matchingMode) {
                                state->preview = *preview;
                            } else {
                                state->previewError = BendPreviewError{
                                    BendPreviewErrorCode::Failed, "Empty or mismatched preview"};
                            }
                        } else if (const auto &error = std::get<BendPreviewError>(result);
                                   error.code != BendPreviewErrorCode::Cancelled) {
                            state->previewError = error;
                            if (error.diagnostic) {
                                const auto count = error.diagnostic->frameCount();
                                const auto last = count ? count - 1 : 0;
                                const bool collision =
                                    (error.code == BendPreviewErrorCode::ToolIntersection &&
                                     error.diagnostic->collisionAtFrame(last)) ||
                                    (error.code == BendPreviewErrorCode::MachineIntersection &&
                                     error.diagnostic->machineCollisionAtFrame(last));
                                bool prefix = count == 1;
                                const auto gauge = error.diagnostic->backgauge();
                                const auto added = gauge ? gauge->addedRetractionPositions : 0;
                                if (count > 1 && added <= 4 && count <= 61 + added &&
                                    mode == BendPreviewMode::ToolMotion &&
                                    error.frameIndex == last && error.diagnostic->pressMotion() &&
                                    !error.diagnostic->punchRetraction()) {
                                    prefix = true;
                                    for (std::size_t f = 0; f < count; ++f)
                                        prefix &= error.diagnostic->pressState(f) &&
                                                  (f == last ||
                                                   (!error.diagnostic->collisionAtFrame(f) &&
                                                    !error.diagnostic->machineCollisionAtFrame(f)));
                                }
                                const bool matchingSupport =
                                    !error.diagnostic->partialEndSupport() ||
                                    withinEndSupportLimit(
                                        *error.diagnostic->partialEndSupport(),
                                        maximumEndSupportLossMm,
                                        state->session.document()
                                                    ->metadata()
                                                    .bendAnalysis.lengthToleranceMm *
                                                4 +
                                            1.e-8);
                                if (!collision || !prefix || !matchingSupport ||
                                    error.diagnostic->workpieceReversed() != workpieceReversed ||
                                    !error.diagnostic->toolSetup() ||
                                    error.diagnostic->machineModel() != expectedMachine ||
                                    error.diagnostic->floorChecked() !=
                                        (expectedMachine == core::MachineModel::DemoPressV3) ||
                                    error.diagnostic->toolSetup()->tools.punchReversed !=
                                        expectedReversed ||
                                    error.diagnostic->toolSetup()->tools.backgauge != expectedGauge)
                                    state->previewError->diagnostic.reset();
                            }
                        }
                    }
                    state->dispatch();
                    emit state->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
void ImportController::discardBendPreview() {
    impl_->invalidatePreview();
    emit stateChanged();
}
void ImportController::discardBendRoute() {
    impl_->invalidateRoute();
    emit stateChanged();
}
void ImportController::discardToolChecks() {
    impl_->invalidateRoute();
    impl_->invalidatePreview();
    emit stateChanged();
}
bool ImportController::setBendOrder(const std::vector<std::string> &ids) {
    if (impl_->stopping || impl_->session.activeRequest() || (hasWork() && !impl_->runningDraft))
        return false;
    const auto before = impl_->session.editRevision();
    const bool changed = impl_->session.setBendOrder(ids);
    if (before != impl_->session.editRevision())
        impl_->invalidateRoute();
    emit stateChanged();
    return changed;
}
BendRoutePtr ImportController::bendRoute() const {
    return impl_->routeRevision == impl_->session.revision() &&
                   impl_->routeEditRevision == impl_->session.editRevision()
               ? impl_->route
               : nullptr;
}
std::optional<BendRouteError> ImportController::bendRouteError() const {
    return impl_->routeError;
}
bool ImportController::isPreparingRoute() const {
    return impl_->preparingRoute;
}
std::optional<BendRouteProgress> ImportController::bendRouteProgress() const {
    return impl_->routeProgress;
}
bool ImportController::isPreparingDraft() const {
    return impl_->runningDraft || impl_->pendingDraft.has_value();
}
RouteDraftPtr ImportController::routeDraft() const {
    return impl_->draft;
}
std::optional<BendRouteError> ImportController::routeDraftError() const {
    return impl_->draftError;
}
void ImportController::discardRouteDraft() {
    impl_->invalidateDraft();
    emit stateChanged();
}
bool ImportController::prepareRouteDraft(RouteDraftRequest request,
                                         std::shared_ptr<const SingleBendPreviewBuilder> builder) {
    auto *s = impl_.get();
    if (s->stopping || s->session.activeRequest() || (hasWork() && !s->runningDraft) || !builder ||
        !s->session.document())
        return false;
    const auto &session = s->session;
    const auto indices = session.orderedBends();
    if (request.operations.size() != indices.size() || request.step > indices.size() ||
        indices.size() != session.document()->metadata().bendAnalysis.bends.size())
        return false;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto *entry = session.bend(indices[i]);
        const auto &op = request.operations[i];
        if (!entry || !included(entry->review) || op.bendIndex != indices[i] ||
            op.id != entry->id || op.workpieceReversed != entry->workpieceReversed ||
            session.bendTarget(indices[i]) != std::optional{op.target})
            return false;
    }
    request.manualThicknessMm = session.manualThicknessMm();
    const auto previous = s->routeBuilder.lock();
    // Reselecting the current immutable, checked draft needs no worker round-trip.
    if (previous == builder && s->draft && s->draft->source == session.document() &&
        s->draft->request == request) {
        emit stateChanged();
        return true;
    }
    if (previous && previous != builder)
        previous->discardRouteCache();
    s->routeBuilder = builder;
    s->invalidateDraft();
    s->routeError.reset();
    s->pendingDraft =
        Impl::DraftJob{session.document(), std::move(request), std::move(builder),
                       s->draftId,         session.revision(), session.editRevision()};
    s->dispatchDraft();
    emit stateChanged();
    return true;
}
bool ImportController::prepareBendRoute(BendRouteRequest request,
                                        std::shared_ptr<const SingleBendPreviewBuilder> builder) {
    if (impl_->stopping || hasWork() || !builder || !impl_->session.document())
        return false;
    const auto &session = impl_->session;
    if (request.operations.size() != session.document()->metadata().bendAnalysis.bends.size())
        return false;
    std::set<std::size_t> seen;
    for (const auto &op : request.operations) {
        const auto *entry = session.bend(op.bendIndex);
        if (!entry || !included(entry->review) || entry->id != op.id ||
            session.bendTarget(op.bendIndex) != std::optional{op.target} ||
            !seen.insert(op.bendIndex).second ||
            (request.withTools &&
             (!op.tools || !core::validPressTools(*op.tools) ||
              (op.assignment && core::toolParameters(*op.assignment) != op.tools))))
            return false;
    }
    request.manualThicknessMm = session.manualThicknessMm();
    if (!request.toolVariants.empty()) {
        if (request.toolVariants.size() > 64)
            return false;
        for (std::size_t i = 0; i < request.toolVariants.size(); ++i) {
            auto candidate = request;
            if (!applyRouteToolVariant(candidate, i))
                return false;
        }
    }
    auto *s = impl_.get();
    if (const auto previous = s->routeBuilder.lock(); previous && previous != builder)
        previous->discardRouteCache();
    s->routeBuilder = builder;
    s->invalidateRoute();
    s->invalidatePreview();
    const auto id = s->routeId, revision = session.revision(), edit = session.editRevision();
    const auto document = session.document();
    s->stop = std::stop_source{};
    const auto stop = s->stop.get_token();
    s->running = s->preparingRoute = true;
    s->routeProgress = BendRouteProgress{};
    struct ProgressGate {
        std::chrono::steady_clock::time_point sent = {};
        std::optional<BendRouteProgress> previous;
    };
    request.onProgress = [owner = QPointer<ImportController>(this), id, revision, edit, stop,
                          gate = std::make_shared<ProgressGate>()](BendRouteProgress progress) {
        if (!owner || stop.stop_requested())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (gate->previous && gate->previous->phase == progress.phase &&
            gate->previous->variant == progress.variant &&
            now - gate->sent < std::chrono::milliseconds(100))
            return;
        gate->previous = progress;
        gate->sent = now;
        QMetaObject::invokeMethod(
            owner,
            [owner, id, revision, edit, stop, progress] {
                if (!owner)
                    return;
                auto *state = owner->impl_.get();
                if (state->preparingRoute && state->routeId == id &&
                    state->session.revision() == revision &&
                    state->session.editRevision() == edit && !stop.stop_requested()) {
                    state->routeProgress = progress;
                    emit owner->stateChanged();
                }
            },
            Qt::QueuedConnection);
    };
    QMetaObject::invokeMethod(
        s->worker,
        [s, id, revision, edit, document, stop, request = std::move(request), builder]() mutable {
            BendRouteResult result = BendRouteError{BendRouteErrorCode::Cancelled, "Cancelled"};
            try {
                if (!stop.stop_requested())
                    result = builder->buildRoute(document, request, stop);
            } catch (const std::exception &e) {
                result = BendRouteError{BendRouteErrorCode::Failed, e.what()};
            } catch (...) {
                result = BendRouteError{BendRouteErrorCode::Failed, "Unexpected route failure"};
            }
            QMetaObject::invokeMethod(
                s->owner,
                [s, id, revision, edit, document, stop, request = std::move(request),
                 result = std::move(result)]() mutable {
                    s->running = s->preparingRoute = false;
                    if (s->stopping)
                        return;
                    if (id == s->routeId && revision == s->session.revision() &&
                        edit == s->session.editRevision() && !stop.stop_requested() &&
                        !s->session.activeRequest()) {
                        if (const auto *route = std::get_if<BendRoutePtr>(&result)) {
                            bool selectedInput =
                                *route && (request.toolVariants.empty()
                                               ? !(*route)->selectedToolVariant
                                               : (*route)->selectedToolVariant &&
                                                     applyRouteToolVariant(
                                                         request, *(*route)->selectedToolVariant));
                            bool valid = selectedInput && *route && (*route)->source == document &&
                                         (*route)->withTools == request.withTools &&
                                         (*route)->steps.size() == request.operations.size();
                            std::set<std::string> ids;
                            std::vector<std::string> order;
                            bool partialEndSupport = false;
                            if (valid)
                                for (const auto &step : (*route)->steps) {
                                    const auto found =
                                        std::find_if(request.operations.begin(),
                                                     request.operations.end(), [&](const auto &op) {
                                                         return step.id == op.id &&
                                                                step.bendIndex == op.bendIndex;
                                                     });
                                    const auto &p = step.preview;
                                    valid &= found != request.operations.end() &&
                                             ids.insert(step.id).second && p &&
                                             !p->unverifiedDraft() && !p->collision() &&
                                             p->frameCount() >= 2 &&
                                             p->workpieceReversed() == step.fixedSecond &&
                                             (request.allowWorkpieceRotation ||
                                              (found != request.operations.end() &&
                                               found->workpieceReversed == step.fixedSecond)) &&
                                             (request.withTools
                                                  ? p->pressMotion() && p->punchRetraction()
                                                  : !p->toolSetup());
                                    if (p && found != request.operations.end()) {
                                        if (const auto *support = p->partialEndSupport()) {
                                            partialEndSupport = true;
                                            valid &=
                                                request.withTools &&
                                                request.allowPartialEndSupport &&
                                                std::isfinite(support->bendLengthMm) &&
                                                std::isfinite(support->supportedFromMm) &&
                                                std::isfinite(support->supportedToMm) &&
                                                support->bendLengthMm == p->model().widthMm &&
                                                support->supportedFromMm >= 0 &&
                                                support->supportedFromMm <
                                                    support->bendLengthMm / 2 &&
                                                support->supportedToMm >
                                                    support->bendLengthMm / 2 &&
                                                support->supportedToMm <= support->bendLengthMm;
                                            if (request.maximumEndSupportLossMm)
                                                valid &= withinEndSupportLimit(
                                                    *support, *request.maximumEndSupportLossMm,
                                                    s->session.document()
                                                                ->metadata()
                                                                .bendAnalysis.lengthToleranceMm *
                                                            4 +
                                                        1.e-8);
                                        }
                                        const bool drawingPunch =
                                            request.withTools && found->assignment &&
                                            found->assignment->useDrawingGeometry &&
                                            found->assignment->punch.drawing.has_value();
                                        const bool drawingDie =
                                            request.withTools && found->assignment &&
                                            found->assignment->useDrawingGeometry &&
                                            found->assignment->die.drawing.has_value();
                                        valid &= p->usesDrawingPunch() == drawingPunch &&
                                                 p->usesDrawingDie() == drawingDie &&
                                                 p->otherFormedBends() == order.size();
                                        valid &= p->machineModel() ==
                                                 (request.withTools ? found->tools->machineModel
                                                                    : core::MachineModel::None);
                                        valid &=
                                            p->floorChecked() ==
                                            (p->machineModel() == core::MachineModel::DemoPressV3);
                                        for (std::size_t f = 0; f < p->frameCount(); ++f)
                                            valid &= !p->machineCollisionAtFrame(f);
                                        if (request.withTools) {
                                            valid &= !found->tools->backgauge ||
                                                     (p->backgauge() &&
                                                      p->backgauge()->setup ==
                                                          *found->tools->backgauge &&
                                                      p->backgauge()->status ==
                                                          BackgaugeStatus::Checked);
                                            const auto *ret = p->punchRetraction();
                                            valid &= p->frameCount() >= 2 && p->toolSetup() &&
                                                     p->toolSetup()->tools == *found->tools &&
                                                     p->pressState(0) &&
                                                     p->pressState(0)->phase ==
                                                         simulation::PressPhase::Approach &&
                                                     p->pressState(p->frameCount() - 1) &&
                                                     p->pressState(p->frameCount() - 1)->phase ==
                                                         simulation::PressPhase::Retracting &&
                                                     ret && std::isfinite(ret->liftMm) &&
                                                     ret->liftMm > 0 &&
                                                     std::isfinite(ret->finalClearanceMm) &&
                                                     ret->finalClearanceMm > 0;
                                        }
                                    }
                                    order.push_back(step.id);
                                }
                            valid &= *route && (*route)->partialEndSupport == partialEndSupport;
                            if (valid) {
                                auto candidate = s->session;
                                if (request.withTools)
                                    for (const auto &op : request.operations) {
                                        if (op.assignment)
                                            valid &= candidate.setBendToolAssignment(
                                                op.bendIndex, *op.assignment);
                                        else
                                            valid &= candidate.setBendTools(op.bendIndex, op.tools);
                                    }
                                if (request.withTools)
                                    for (const auto &step : (*route)->steps)
                                        valid &= candidate.setBendWorkpieceOrientation(
                                            step.bendIndex, step.fixedSecond);
                                valid &= candidate.setBendOrder(order);
                                if (valid)
                                    s->session = std::move(candidate);
                            }
                            if (valid) {
                                s->route = *route;
                                s->routeRevision = revision;
                                s->routeEditRevision = s->session.editRevision();
                            } else
                                s->routeError = BendRouteError{BendRouteErrorCode::Failed,
                                                               "Invalid route result"};
                        } else if (const auto &e = std::get<BendRouteError>(result);
                                   e.code != BendRouteErrorCode::Cancelled)
                            s->routeError = e;
                    }
                    s->dispatch();
                    emit s->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
BendPreviewPtr ImportController::bendPreview() const {
    return impl_->preview;
}
const std::optional<BendPreviewError> &ImportController::bendPreviewError() const {
    return impl_->previewError;
}
bool ImportController::isPreparingPreview() const {
    return impl_->preparingPreview;
}
} // namespace bending::ui

namespace bending::ui {
bool ImportController::prepareDrawing(
    std::shared_ptr<const application::BendingDrawingBuilder> builder,
    application::DrawingRequest request) {
    if (impl_->stopping || hasWork() || !builder || !impl_->session.document())
        return false;
    auto *state = impl_.get();
    request.manualThicknessMm = state->session.manualThicknessMm();
    request.unfoldSettings = state->session.unfoldSettings();
    request.preparedFlatPattern = state->flat;
    if (state->drawing && state->drawingRequest == request) {
        emit stateChanged();
        return true;
    }
    state->invalidateDrawing();
    state->drawingRequest = request;
    const auto id = state->drawingId;
    const auto document = state->session.document();
    const auto revision = state->session.revision();
    const auto editRevision = state->session.editRevision();
    request.manualThicknessMm = state->session.manualThicknessMm();
    state->stop = std::stop_source{};
    const auto stop = state->stop.get_token();
    state->running = true;
    state->preparingDrawing = true;
    QMetaObject::invokeMethod(
        state->worker,
        [state, builder = std::move(builder), request, id, document, revision, editRevision, stop] {
            application::DrawingResult result = application::DrawingError{"Drawing failed"};
            try {
                result = builder->build(document, request, stop);
            } catch (const std::exception &e) {
                result = application::DrawingError{e.what()};
            } catch (...) {
                result = application::DrawingError{"Unexpected drawing failure"};
            }
            QMetaObject::invokeMethod(
                state->owner,
                [state, result = std::move(result), id, revision, editRevision, stop] {
                    state->running = false;
                    state->preparingDrawing = false;
                    if (state->stopping)
                        return;
                    if (!stop.stop_requested() && id == state->drawingId &&
                        revision == state->session.revision() &&
                        editRevision == state->session.editRevision() &&
                        !state->session.activeRequest()) {
                        if (const auto *drawing =
                                std::get_if<application::BendingDrawingPtr>(&result)) {
                            if (*drawing && application::drawingScale(**drawing))
                                state->drawing = *drawing;
                            else
                                state->drawingError =
                                    application::DrawingError{"Invalid drawing snapshot"};
                        } else {
                            state->drawingError = std::get<application::DrawingError>(result);
                        }
                    }
                    state->dispatch();
                    emit state->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
application::BendingDrawingPtr ImportController::drawing() const {
    return impl_->drawing;
}
std::optional<application::DrawingError> ImportController::drawingError() const {
    return impl_->drawingError;
}
bool ImportController::isPreparingDrawing() const {
    return impl_->preparingDrawing;
}
void ImportController::cancelDrawing() {
    impl_->invalidateDrawing();
    emit stateChanged();
}
} // namespace bending::ui

namespace bending::ui {
bool ImportController::setPartInputs(std::uint64_t revision, std::uint64_t editRevision,
                                     std::optional<double> manualThicknessMm,
                                     core::UnfoldSettings settings) {
    if (impl_->stopping || hasWork())
        return false;
    const auto before = impl_->session.editRevision();
    const auto oldThickness = impl_->session.manualThicknessMm();
    const bool accepted =
        impl_->session.setPartInputs(revision, editRevision, manualThicknessMm, settings);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidateFlat();
        impl_->invalidateDrawing();
        if (oldThickness != impl_->session.manualThicknessMm())
            impl_->invalidatePreview();
    }
    emit stateChanged();
    return accepted;
}

bool ImportController::setUnfoldSettings(core::UnfoldSettings settings) {
    if (impl_->stopping || hasWork())
        return false;
    const auto before = impl_->session.editRevision();
    const bool changed = impl_->session.setUnfoldSettings(settings);
    if (before != impl_->session.editRevision()) {
        impl_->invalidateRoute();
        impl_->invalidateFlat();
        impl_->invalidateDrawing();
    }
    emit stateChanged();
    return changed;
}
bool ImportController::prepareFlatPattern(
    std::shared_ptr<const application::FlatPatternBuilder> builder) {
    if (impl_->stopping || hasWork() || !builder || !impl_->session.document())
        return false;
    auto *s = impl_.get();
    const application::FlatPatternRequest request{s->session.unfoldSettings(),
                                                  s->session.manualThicknessMm()};
    if (s->flat && s->flatRequest == request) {
        emit stateChanged();
        return true;
    }
    s->invalidateFlat();
    s->flatRequest = request;
    const auto id = s->flatId, revision = s->session.revision();
    const auto document = s->session.document();
    s->stop = std::stop_source{};
    const auto stop = s->stop.get_token();
    s->running = true;
    s->preparingFlat = true;
    QMetaObject::invokeMethod(
        s->worker,
        [s, builder = std::move(builder), id, revision, document, request, stop] {
            application::FlatPatternResult result = application::FlatPatternError{
                application::FlatPatternErrorCode::Failed, "Flat pattern failed"};
            try {
                result = builder->build(document, request, stop);
            } catch (const std::exception &e) {
                result = application::FlatPatternError{application::FlatPatternErrorCode::Failed,
                                                       e.what()};
            } catch (...) {
                result = application::FlatPatternError{application::FlatPatternErrorCode::Failed,
                                                       "Unexpected flat pattern failure"};
            }
            QMetaObject::invokeMethod(
                s->owner,
                [s, id, revision, result = std::move(result), stop] {
                    s->running = false;
                    s->preparingFlat = false;
                    if (s->stopping)
                        return;
                    if (!stop.stop_requested() && id == s->flatId &&
                        revision == s->session.revision() && !s->session.activeRequest()) {
                        if (const auto *flat = std::get_if<application::FlatPatternPtr>(&result);
                            flat && *flat &&
                            (*flat)->info().settings == s->session.unfoldSettings() &&
                            std::isfinite((*flat)->info().thicknessMm) &&
                            (*flat)->info().thicknessMm > 0 &&
                            std::isfinite((*flat)->info().bounds.maximum.x) &&
                            (*flat)->info().bounds.maximum.x > 0 &&
                            std::isfinite((*flat)->info().bounds.maximum.y) &&
                            (*flat)->info().bounds.maximum.y > 0)
                            s->flat = *flat;
                        else if (const auto *error =
                                     std::get_if<application::FlatPatternError>(&result))
                            s->flatError = *error;
                        else
                            s->flatError = application::FlatPatternError{
                                application::FlatPatternErrorCode::Failed, "Empty flat pattern"};
                    }
                    s->dispatch();
                    emit s->owner->stateChanged();
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    emit stateChanged();
    return true;
}
application::FlatPatternPtr ImportController::flatPattern() const {
    return impl_->flat;
}
std::optional<application::FlatPatternError> ImportController::flatPatternError() const {
    return impl_->flatError;
}
bool ImportController::isPreparingFlatPattern() const {
    return impl_->preparingFlat;
}
} // namespace bending::ui
