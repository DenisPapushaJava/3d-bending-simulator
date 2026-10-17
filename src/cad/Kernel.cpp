#include "bending/cad/Kernel.h"

#include <Standard_Version.hxx>

namespace bending::cad {
std::string_view kernelVersion() noexcept {
    return OCC_VERSION_COMPLETE;
}
} // namespace bending::cad
