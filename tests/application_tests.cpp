#include "bending/application/DocumentSession.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace bending::application;
void require(bool value, const char *message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
class TestDocument final : public Document {
  public:
    explicit TestDocument(BendAnalysis analysis = {}) {
        metadata_.bendAnalysis = std::move(analysis);
    }
    const DocumentMetadata &metadata() const noexcept override {
        return metadata_;
    }
    const std::vector<FaceInfo> &faces() const noexcept override {
        return faces_;
    }

  private:
    DocumentMetadata metadata_{};
    std::vector<FaceInfo> faces_;
};
void testSession() {
    DocumentSession session;
    auto first = session.beginImport("first.step");
    auto snapshot = std::make_shared<TestDocument>();
    require(session.complete(first, DocumentPtr(snapshot)), "First result must be accepted");
    require(session.revision() == 1 && session.document() == snapshot,
            "First snapshot not committed");
    const auto second = session.beginImport("second.step");
    const auto third = session.beginImport("third.step");
    require(!session.updateProgress(second, ImportPhase::Preparing), "Old progress was accepted");
    require(!session.complete(second, DocumentPtr(std::make_shared<TestDocument>())),
            "Old result was accepted");
    require(session.updateProgress(third, ImportPhase::Reading), "Current progress was dropped");
    require(session.complete(third, ImportError{ImportErrorCode::InvalidStep, "broken", {}}),
            "Failure was dropped");
    require(session.error() && session.document() == snapshot && session.revision() == 1,
            "Failure replaced the snapshot");
    const auto cancelled = session.beginImport("cancelled.step");
    session.cancel();
    require(!session.complete(cancelled, DocumentPtr(std::make_shared<TestDocument>())),
            "Cancelled result was accepted");
    require(session.document() == snapshot && session.revision() == 1 && !session.error(),
            "Cancel changed the document");
    const auto nullResult = session.beginImport("null.step");
    require(session.complete(nullResult, DocumentPtr{}), "Null result completion not handled");
    require(session.error() && session.error()->code == ImportErrorCode::UnexpectedFailure &&
                session.document() == snapshot,
            "Null success must fail and preserve the snapshot");
    const auto closing = session.beginImport("closing.step");
    session.close();
    require(!session.document() && session.revision() == 2,
            "Close did not invalidate the snapshot");
    require(!session.complete(closing, DocumentPtr(snapshot)), "Closed document was resurrected");
    const auto reopened = session.beginImport("reopened.step");
    require(reopened.id > closing.id && reopened.baseRevision == 2,
            "Request IDs or revision were reused");
    require(session.complete(reopened, DocumentPtr(snapshot)) && session.revision() == 3,
            "Reopen failed");
    const auto failedCancel = session.beginImport("stopped.step");
    require(session.complete(failedCancel, ImportError{ImportErrorCode::Cancelled, "stop", {}}) &&
                !session.error(),
            "Cooperative cancellation was shown as an error");
}
void testProjectState() {
    DocumentSession session;
    auto snapshot = std::make_shared<TestDocument>();
    const auto request = session.beginImport("part.step");
    session.complete(request, DocumentPtr(snapshot));
    const auto geometryRevision = session.revision();
    require(session.setManualThickness(2.0) == ThicknessValidation::Unverified,
            "Manual input lost its unverified provenance");
    require(session.modified() && session.manualThicknessMm() == 2.0 &&
                session.revision() == geometryRevision,
            "Editing parameters changed geometry or lost dirty state");
    const auto edit = session.editRevision();
    require(session.setManualThickness(-1.0) == ThicknessValidation::Invalid,
            "Negative thickness was accepted");
    require(session.setManualThickness(std::numeric_limits<double>::quiet_NaN()) ==
                ThicknessValidation::Invalid,
            "NaN thickness was accepted");
    require(session.editRevision() == edit && session.manualThicknessMm() == 2.0,
            "Rejected edit changed project");
    session.useAutomaticThickness();
    require(!session.markSaved(edit, "stale.bend") && session.modified(),
            "Stale save cleared modified state");
    require(session.markSaved(session.editRevision(), "current.bend") && !session.modified(),
            "Save not recorded");
    const auto failed = session.beginImport("bad.bend");
    session.complete(failed, ImportError{ImportErrorCode::InvalidProject, "broken", {}});
    require(session.document() == snapshot && session.projectPath() == "current.bend",
            "Failed open lost project");
    const auto opened = session.beginImport("loaded.bend");
    session.complete(opened, OpenedProject{snapshot, 1.5, "loaded.bend"});
    require(session.manualThicknessMm() == 1.5 && !session.modified() &&
                session.projectPath() == "loaded.bend",
            "Project settings not restored");
    ThicknessAnalysis analysis;
    analysis.status = ThicknessStatus::Detected;
    analysis.thicknessMm = 2.0;
    require(validateThickness(2.0, analysis) == ThicknessValidation::Verified &&
                validateThickness(3.0, analysis) == ThicknessValidation::ConflictsWithGeometry,
            "Detected thickness conflict was not checked");
    require(suggestThickness(analysis) == 2.0, "Confirmed thickness was not proposed");
    analysis.status = ThicknessStatus::Unsupported;
    analysis.evidence = {{1, 2, 1.5}};
    require(validateThickness(1.5, analysis) == ThicknessValidation::LocalMatch,
            "Local measurement was treated as a global proof");
    analysis.evidence = {{1, 2, 6.0}, {3, 4, 6.0 + 1.0e-8}};
    require(suggestThickness(analysis) == 6.0 && analysis.status == ThicknessStatus::Unsupported,
            "Consistent local evidence must be proposed without claiming a global proof");
    analysis.evidence.push_back({5, 6, 4.0});
    require(!suggestThickness(analysis), "Conflicting distances were silently resolved");
    analysis.evidence.pop_back();
    for (const auto status :
         {ThicknessStatus::Ambiguous, ThicknessStatus::LimitReached, ThicknessStatus::Failed}) {
        analysis.status = status;
        require(!suggestThickness(analysis), "Incomplete analysis suggested a thickness");
    }
    analysis.status = ThicknessStatus::Unsupported;
    analysis.evidence.clear();
    require(!suggestThickness(analysis), "Missing geometry invented a default thickness");
}
void testBendPreparation() {
    BendAnalysis analysis;
    analysis.status = BendRecognitionStatus::Completed;
    analysis.bends = {
        {{0, 0, 0}, {0, 0, 80}, std::numbers::pi / 2, 4, 2, 80, 1, 2, {3, 4, 5, 6}, false, 80},
        {{100, 0, 0},
         {100, 0, 80},
         std::numbers::pi / 2,
         4,
         2,
         80,
         7,
         8,
         {9, 10, 11, 12},
         false,
         80}};
    const auto document = std::make_shared<TestDocument>(analysis);
    DocumentSession session;
    session.complete(session.beginImport("part.step"), DocumentPtr(document));
    const auto geometryRevision = session.revision();
    const auto id = session.bend(0)->id;
    const bending::core::BendTarget target{std::numbers::pi * 2 / 3, 5.5};
    require(session.bends().size() == 2 && !session.modified() && !session.bend(0)->target,
            "New measurements became confirmed targets");
    require(session.bend(0)->review == BendReview::Automatic && session.bendTarget(0) &&
                session.bendTarget(0)->innerRadiusMm == 4,
            "Automatic CAD target unavailable");
    require(session.setBendReview(0, BendReview::Pending) && !session.bendTarget(0) &&
                !session.setBendTarget(0, target),
            "Pending zone accepted a target");
    require(session.setBendReview(0, BendReview::Confirmed) && session.setBendTarget(0, target),
            "Confirmed target not accepted");
    require(session.setBendReview(1, BendReview::Excluded), "Zone not excluded");
    const auto editRevision = session.editRevision();
    require(session.setBendTarget(0, target) && session.editRevision() == editRevision,
            "Unchanged target dirtied project again");
    require(!session.setBendTarget(0, bending::core::BendTarget{0, 4}) &&
                !session.setBendReview(99, BendReview::Confirmed) &&
                !session.setBendReview(0, static_cast<BendReview>(99)) &&
                session.editRevision() == editRevision,
            "Invalid edits changed state");
    require(session.document() == document && session.revision() == geometryRevision &&
                session.document()->metadata().bendAnalysis.bends[0].innerRadiusMm == 4 &&
                session.modified(),
            "User target changed the CAD measurement");
    require(!session.markSaved(editRevision - 1, "old.bend") && session.modified(),
            "Old save cleared new bend edits");
    auto entries = session.bends();
    auto reordered = analysis;
    std::reverse(reordered.bends.begin(), reordered.bends.end());
    std::swap(reordered.bends[1].axisStart, reordered.bends[1].axisEnd);
    reordered.bends[1].innerFace = 77;
    reordered.bends[1].axisStart.x += 1.0e-6;
    const auto reorderedDocument = std::make_shared<TestDocument>(reordered);
    session.complete(session.beginImport("saved.bend"),
                     OpenedProject{reorderedDocument, {}, "saved.bend", entries});
    require(session.bend(1)->id == id && session.bend(1)->target == target &&
                session.bend(0)->review == BendReview::Excluded && !session.modified(),
            "Reordering/axis reversal/face renumbering lost user intent");
    session.setBendReview(1, BendReview::Excluded);
    require(!session.setBendTarget(1, {}) && session.bend(1)->target == target,
            "Excluding a zone destroyed its retained target");
    session.setBendReview(1, BendReview::Confirmed);
    require(session.setBendTarget(1, {}) && !session.bend(1)->target, "Explicit reset failed");
    auto invalid = entries;
    invalid[1].id = invalid[0].id;
    session.complete(session.beginImport("invalid.bend"),
                     OpenedProject{document, {}, "invalid.bend", invalid});
    require(session.error() && session.document() == reorderedDocument && session.modified(),
            "Invalid project replaced current edits");
    const auto active = session.beginImport("other.step");
    require(!session.setBendReview(0, BendReview::Confirmed), "Edit accepted during import");
    session.cancel();
    require(!session.complete(active, DocumentPtr(document)), "Stale import replaced preparations");

    auto ambiguous = analysis;
    ambiguous.bends.push_back(ambiguous.bends[0]);
    auto binding = restoreBendPreparations(ambiguous, entries);
    require(!binding.byRecognizedBend[0] && !binding.byRecognizedBend[2] &&
                binding.unresolved == std::vector<std::size_t>{0} && entries[0].target == target,
            "Ambiguous match silently reassigned a saved target");
    auto duplicate = std::vector<BendPreparation>{entries[0], entries[0]};
    duplicate[1].id = "other-id";
    binding = restoreBendPreparations(analysis, duplicate);
    require(!binding.byRecognizedBend[0] && binding.unresolved.size() == 2,
            "Two saved entries bound to the same zone");
    auto changed = analysis;
    changed.bends[0].axisStart.x += 10;
    changed.bends[0].axisEnd.x += 10;
    binding = restoreBendPreparations(changed, entries);
    require(binding.unresolved == std::vector<std::size_t>{0} && binding.byRecognizedBend[0] &&
                entries[0].target == target &&
                entries[*binding.byRecognizedBend[0]].review == BendReview::Automatic &&
                !entries[*binding.byRecognizedBend[0]].target,
            "Unmatched decision lost or applied to a new zone");
    session.close();
    require(session.bends().empty() && !session.bend(0), "Close retained preparations");
}

} // namespace
int main() {
    try {
        testSession();
        testProjectState();
        testBendPreparation();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout
        << "Document session: revisions, stale replies, errors, cancellation and close passed\n";
}
