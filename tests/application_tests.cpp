#include "bending/application/DocumentSession.h"

#include <iostream>
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
} // namespace
int main() {
    try {
        testSession();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout
        << "Document session: revisions, stale replies, errors, cancellation and close passed\n";
}
