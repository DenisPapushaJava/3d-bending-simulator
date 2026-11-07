#pragma once
#include "bending/application/BendRoute.h"
#include "bending/application/DocumentSession.h"

namespace bending::application {
// Validate the caller-owned request against the session and capture its thickness.
// Session state is never changed; a rejected request must not be dispatched.
[[nodiscard]] bool prepareBendRouteRequest(const DocumentSession &, BendRouteRequest &);

// Resolve the selected input, validate every returned operation and apply all
// assignments/orientations/order atomically. Failure leaves the session intact.
// The caller still owns cancellation and request/document/edit-revision checks.
// Takes ownership of the prepared request to avoid another copy of DXF inputs.
[[nodiscard]] bool applyBendRouteResult(DocumentSession &, BendRouteRequest, const BendRoutePtr &);
} // namespace bending::application
