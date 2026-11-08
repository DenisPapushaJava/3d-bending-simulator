#pragma once
#include "bending/application/BendRoute.h"
#include "bending/application/SingleBendPreview.h"
#include <cstdint>
#include <list>
#include <utility>

namespace bending::cad {
// Owned by one RouteCache source/thickness, under its worker lock. Requests must
// pass the complete input validation before lookup. Only visual dependencies
// belong here; each hit receives the caller's current full request metadata.
class RouteDraftCache final {
  public:
    [[nodiscard]] application::RouteDraftPtr find(application::DocumentPtr,
                                                  const application::RouteDraftRequest &);
    void remember(const application::RouteDraftPtr &);
    // Downgrade just the checked flat contact position; never adapt a bending
    // trajectory, joint operation or retraction to the manual 21-position path.
    void rememberBlank(application::DocumentPtr, application::RouteOperation,
                       const application::BendPreviewPtr &);

  private:
    struct Key {
        std::uint64_t formed;
        application::RouteOperation active;
        bool blank, withTools;
        bool operator==(const Key &) const = default;
    };
    struct Visual {
        application::BendPreviewPtr preview;
        std::size_t checkedToolPositions;
        std::string notice;
        std::optional<application::BendRouteToolLengthIssue> lengths;
        std::optional<double> centredPunchOpeningMm;
    };
    static Key key(const application::RouteDraftRequest &);
    void put(Key, Visual);
    std::list<std::pair<Key, Visual>> entries_;
};
} // namespace bending::cad
