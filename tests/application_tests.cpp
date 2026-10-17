#include "bending/application/DocumentSession.h"

#include <iostream>
#include <limits>
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

} // namespace
int main() {
    try {
        testSession();
        testProjectState();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout
        << "Document session: revisions, stale replies, errors, cancellation and close passed\n";
}
