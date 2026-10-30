#include "GeometryCacheInternal.h"
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <BinTools.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Version.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <list>
#include <locale>
#include <mutex>
#include <stdexcept>

namespace bending::cad {
namespace {
using Clock = std::chrono::steady_clock;
constexpr double meshAngle = .35;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
std::string serialize(const TopoDS_Shape &shape, bool mesh) {
    std::ostringstream out(std::ios::out | std::ios::binary);
    BinTools::Write(shape, out, mesh, false, BinTools_FormatVersion_CURRENT);
    if (!out)
        throw std::runtime_error("Cannot serialize tool geometry");
    return out.str();
}
bool valid(const TopoDS_Shape &shape, bool requireMesh) {
    if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid())
        return false;
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    if (!std::isfinite(properties.Mass()) || std::abs(properties.Mass()) <= 1.e-10)
        return false;
    if (requireMesh)
        for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), location);
            if (mesh.IsNull() || mesh->NbNodes() < 3 || mesh->NbTriangles() < 1)
                return false;
            for (int i = 1; i <= mesh->NbNodes(); ++i) {
                const auto p = mesh->Node(i);
                if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z()))
                    return false;
            }
            for (int i = 1; i <= mesh->NbTriangles(); ++i) {
                int a, b, c;
                mesh->Triangle(i).Get(a, b, c);
                if (a < 1 || b < 1 || c < 1 || a > mesh->NbNodes() || b > mesh->NbNodes() ||
                    c > mesh->NbNodes())
                    return false;
            }
        }
    return true;
}
TopoDS_Shape prepare(const std::function<TopoDS_Shape()> &factory, double deflection) {
    const auto source = factory();
    if (source.IsNull())
        throw std::runtime_error("Empty tool cache source");
    // Boolean outputs may reuse faces of a previously published tool. Mesh only our own copy.
    const auto shape = BRepBuilderAPI_Copy(source, true, false).Shape();
    if (deflection > 0) {
        BRepMesh_IncrementalMesh mesh(shape, deflection, false, meshAngle, false);
        if (!mesh.IsDone())
            throw std::runtime_error("Tool geometry meshing failed");
    }
    if (!valid(shape, deflection > 0))
        throw std::runtime_error("Invalid prepared tool geometry");
    return shape;
}
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Tool preparation cancelled");
}
} // namespace
class ToolGeometryCache::Impl {
  public:
    struct Entry {
        std::string key;
        TopoDS_Shape shape;
        std::size_t bytes;
    };
    explicit Impl(std::shared_ptr<const application::GeometryArtifactStore> files,
                  std::size_t budget)
        : store(std::move(files)), maximumBytes(budget) {}
    TopoDS_Shape get(const std::string &key, const std::function<TopoDS_Shape()> &factory,
                     double deflection, std::stop_token stop) {
        cancelled(stop);
        {
            std::lock_guard lock(mutex);
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->key == key) {
                    const auto shape = it->shape;
                    entries.splice(entries.begin(), entries, it);
                    ++stats.memoryHits;
                    return shape;
                }
        }
        if (store) {
            const auto start = Clock::now();
            try {
                if (const auto bytes = store->read(key)) {
                    TopoDS_Shape shape;
                    std::istringstream in(*bytes, std::ios::in | std::ios::binary);
                    BinTools::Read(shape, in);
                    if (in && valid(shape, deflection > 0)) {
                        cancelled(stop);
                        insert(key, shape, bytes->size());
                        std::lock_guard lock(mutex);
                        ++stats.diskHits;
                        stats.loadMs += elapsed(start);
                        return shape;
                    }
                    std::lock_guard lock(mutex);
                    ++stats.rejected;
                }
            } catch (...) {
                cancelled(stop);
                std::lock_guard lock(mutex);
                ++stats.rejected;
            }
            std::lock_guard lock(mutex);
            stats.loadMs += elapsed(start);
        }
        const auto start = Clock::now();
        auto shape = prepare(factory, deflection);
        cancelled(stop);
        {
            std::lock_guard lock(mutex);
            ++stats.builds;
            stats.buildMs += elapsed(start);
        }
        std::string bytes;
        try {
            bytes = serialize(shape, deflection > 0);
        } catch (...) {
            std::lock_guard lock(mutex);
            ++stats.writeFailures;
            return shape;
        }
        cancelled(stop);
        if (store) {
            const auto saving = Clock::now();
            bool written = false;
            try {
                written = store->write(key, bytes);
            } catch (...) {
                // An unavailable cache must not turn valid geometry into an application error.
            }
            std::lock_guard lock(mutex);
            stats.saveMs += elapsed(saving);
            if (written)
                ++stats.writes;
            else
                ++stats.writeFailures;
        }
        insert(key, shape, bytes.size());
        return shape;
    }
    void insert(const std::string &key, const TopoDS_Shape &shape, std::size_t bytes) {
        bytes += key.size();
        std::lock_guard lock(mutex);
        if (bytes > maximumBytes)
            return;
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->key == key) {
                usedBytes -= it->bytes;
                entries.erase(it);
                break;
            }
        entries.push_front({key, shape, bytes});
        usedBytes += bytes;
        while (usedBytes > maximumBytes) {
            usedBytes -= entries.back().bytes;
            entries.pop_back();
        }
    }
    std::shared_ptr<const application::GeometryArtifactStore> store;
    mutable std::mutex mutex;
    std::list<Entry> entries;
    std::size_t usedBytes = 0, maximumBytes;
    Statistics stats;
};

ToolGeometryCache::ToolGeometryCache(
    std::shared_ptr<const application::GeometryArtifactStore> store, std::size_t memoryBytes)
    : impl_(std::make_unique<Impl>(std::move(store), memoryBytes)) {}
ToolGeometryCache::~ToolGeometryCache() = default;
ToolGeometryCache::Statistics ToolGeometryCache::statistics() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}
void ToolGeometryCache::clearMemory() {
    std::lock_guard lock(impl_->mutex);
    impl_->entries.clear();
    impl_->usedBytes = 0;
}
thread_local ToolGeometryCache::Impl *GeometryCacheScope::active_ = nullptr;
thread_local std::stop_token GeometryCacheScope::stop_;
thread_local std::vector<GeometryCacheScope::KnownShape> *GeometryCacheScope::known_ = nullptr;
GeometryCacheScope::GeometryCacheScope(const std::shared_ptr<ToolGeometryCache> &cache,
                                       std::stop_token stop)
    : owner_(cache), previousKnown_(known_), previous_(active_), previousStop_(stop_) {
    active_ = cache ? cache->impl_.get() : nullptr;
    stop_ = stop;
    known_ = &knownShapes_;
}
GeometryCacheScope::~GeometryCacheScope() {
    active_ = previous_;
    stop_ = previousStop_;
    known_ = previousKnown_;
}
TopoDS_Shape GeometryCacheScope::shape(const std::string &key,
                                       const std::function<TopoDS_Shape()> &factory,
                                       double deflection) {
    cancelled(stop_);
    if (active_ && !key.empty()) {
        auto descriptor = geometryKey("prepared");
        descriptor << deflection << ' ' << (deflection > 0 ? meshAngle : 0) << ' ' << key.size()
                   << ':' << key;
        const auto keyValue = descriptor.str();
        const auto shape = active_->get(keyValue, factory, deflection, stop_);
        const bool known = std::any_of(known_->begin(), known_->end(), [&](const auto &entry) {
            return shape.IsPartner(entry.shape);
        });
        if (!known)
            known_->push_back({keyValue, shape});
        return shape;
    }
    return prepare(factory, deflection);
}
std::string GeometryCacheScope::descriptor(const TopoDS_Shape &shape) {
    if (!active_)
        return {};
    for (const auto &entry : *known_)
        if (shape.IsPartner(entry.shape)) {
            auto key = geometryKey("reference");
            key << entry.descriptor.size() << ':' << entry.descriptor << ' ' << shape.Orientation();
            const auto delta = shape.Location().Transformation() *
                               entry.shape.Location().Transformation().Inverted();
            for (int r = 1; r <= 3; ++r)
                for (int c = 1; c <= 4; ++c)
                    key << ' ' << (delta.Value(r, c) == 0 ? 0 : delta.Value(r, c));
            return key.str();
        }
    try {
        return serialize(shape, false);
    } catch (...) {
        return {};
    }
}
bool GeometryCacheScope::enabled() {
    return active_ != nullptr;
}
std::ostringstream geometryKey(std::string_view kind) {
    std::ostringstream key;
    key.imbue(std::locale::classic());
    key << std::hexfloat << "tool-assets-v1/" << OCC_VERSION_COMPLETE << '/' <<
#ifdef _WIN32
        "windows/"
#else
        "unix/"
#endif
        << sizeof(void *) << '/' << (std::endian::native == std::endian::little ? 'L' : 'B') << '/'
        << BinTools_FormatVersion_CURRENT << '/' << BENDING_GEOMETRY_SDK_REVISION << '/' << kind
        << ' ';
    return key;
}
TopoDS_Shape cachedToolShape(const std::string &key, const std::function<TopoDS_Shape()> &factory,
                             double deflection) {
    return GeometryCacheScope::shape(key, factory, deflection);
}
} // namespace bending::cad
