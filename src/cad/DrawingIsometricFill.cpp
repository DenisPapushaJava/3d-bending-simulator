#include "DrawingIsometricFill.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bending::cad {
std::optional<application::DrawingIsometricFill>
makeDrawingIsometricFill(const TopoDS_Shape &shape, const gp_Ax2 &frame,
                         const application::DrawingView &view, std::stop_token stop) {
    constexpr int resolution = 768;
    constexpr std::size_t triangleBudget = 200000;
    constexpr std::size_t pixelBudget = 24000000;
    constexpr double edgeTolerance = 1.e-9;
    const double spanX = view.maximum.x - view.minimum.x;
    const double spanY = view.maximum.y - view.minimum.y;
    if (!std::isfinite(spanX) || !std::isfinite(spanY) || spanX <= 0 || spanY <= 0)
        return {};
    const double scale = resolution / std::max(spanX, spanY);
    const int width = std::clamp(static_cast<int>(std::ceil(spanX * scale)), 1, resolution);
    const int height = std::clamp(static_cast<int>(std::ceil(spanY * scale)), 1, resolution);
    const auto size = static_cast<std::size_t>(width) * height;
    application::DrawingIsometricFill fill{width, height, std::vector<std::uint32_t>(size)};
    std::vector<double> depth(size, -std::numeric_limits<double>::infinity());
    const gp_Vec u(frame.XDirection()), v(frame.YDirection()), n(frame.Direction());
    const gp_Vec light = (u * -.25 + v * .7 + n * .65).Normalized();
    const auto shadeColor = [](double illumination) {
        const double brightness = std::clamp((illumination + .25) / 1.25, 0., 1.);
        // Deepen the midtones while keeping the lit faces close to paper white.
        const auto gray = static_cast<std::uint32_t>(180.5 + 70. * brightness * brightness);
        return 0xff000000u | gray * 0x00010101u;
    };
    std::size_t triangles = 0, pixels = 0;
    for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
        if (stop.stop_requested())
            return {};
        const auto face = TopoDS::Face(faces.Current());
        const bool reversed = face.Orientation() == TopAbs_REVERSED;
        TopLoc_Location location;
        const auto &mesh = BRep_Tool::Triangulation(face, location);
        if (mesh.IsNull() || mesh->NbTriangles() == 0)
            return {}; // Partial shading could make a solid wall look like an opening.
        triangles += static_cast<std::size_t>(mesh->NbTriangles());
        if (triangles > triangleBudget)
            return {};
        const BRepAdaptor_Surface surface(face, false);
        bool smoothShading = surface.GetType() == GeomAbs_Cylinder;
        gp_Pnt cylinderOrigin;
        gp_Vec cylinderDirection, cylinderLight = light;
        if (smoothShading) {
            const auto cylinder = surface.Cylinder();
            cylinderOrigin = cylinder.Location();
            cylinderDirection = gp_Vec(cylinder.Axis().Direction());
            if (cylinder.Direct() == reversed)
                cylinderLight.Reverse();
        }
        struct PixelPoint {
            double x, y, z, illumination;
        };
        if (mesh->NbNodes() > static_cast<int>(triangleBudget * 3))
            return {};
        std::vector<PixelPoint> nodes;
        nodes.reserve(static_cast<std::size_t>(mesh->NbNodes()));
        for (int i = 1; i <= mesh->NbNodes(); ++i) {
            if ((i & 255) == 0 && stop.stop_requested())
                return {};
            const gp_Pnt worldPoint = mesh->Node(i).Transformed(location.Transformation());
            const gp_Vec point(frame.Location(), worldPoint);
            double illumination = 0.;
            if (smoothShading) {
                const gp_Vec fromAxis(cylinderOrigin, worldPoint);
                const gp_Vec radial =
                    fromAxis - cylinderDirection * fromAxis.Dot(cylinderDirection);
                const double length = radial.Magnitude();
                smoothShading = std::isfinite(length) && length > 1.e-10;
                if (smoothShading)
                    illumination = radial.Dot(cylinderLight) / length;
            }
            const PixelPoint projected{(point.Dot(u) - view.minimum.x) * (width / spanX),
                                       (view.maximum.y - point.Dot(v)) * (height / spanY),
                                       point.Dot(n), illumination};
            if (!std::isfinite(projected.x) || !std::isfinite(projected.y) ||
                !std::isfinite(projected.z))
                return {};
            nodes.push_back(projected);
        }
        for (int i = 1; i <= mesh->NbTriangles(); ++i) {
            if ((i & 127) == 0 && stop.stop_requested())
                return {};
            int p, q, r;
            mesh->Triangle(i).Get(p, q, r);
            const auto &a = nodes[p - 1], &b = nodes[q - 1], &c = nodes[r - 1];
            const double bx = b.x - a.x, by = b.y - a.y, bz = b.z - a.z;
            const double cx = c.x - a.x, cy = c.y - a.y, cz = c.z - a.z;
            const double area = bx * cy - by * cx;
            // Image Y runs downwards. Cull faces pointing away from the viewer;
            // the CAD document already contains a closed sheet solid.
            if (std::abs(area) < 1.e-10 || (area > 0) != reversed)
                continue;
            const auto lower = [](double x, double y, double z, int limit) {
                return static_cast<int>(
                    std::clamp(std::floor(std::min({x, y, z})), 0., limit - 1.));
            };
            const auto upper = [](double x, double y, double z, int limit) {
                return static_cast<int>(std::clamp(std::ceil(std::max({x, y, z})), 0., limit - 1.));
            };
            const int left = lower(a.x, b.x, c.x, width), right = upper(a.x, b.x, c.x, width);
            const int top = lower(a.y, b.y, c.y, height), bottom = upper(a.y, b.y, c.y, height);
            // Long, thin triangles may cover very little of their bounding rectangle.
            // Clip their pixel spans with the same three barycentric half-spaces.
            const int rows = bottom - top + 1, columns = right - left + 1;
            const auto rectanglePixels = static_cast<std::size_t>(columns) * rows;
            const bool clipRows =
                columns >= 8 && rectanglePixels > 64 && rectanglePixels > 2 * std::abs(area);
            pixels += clipRows ? static_cast<std::size_t>(3 * rows) : rectanglePixels;
            if (pixels > pixelBudget)
                return {};
            const auto slopes = clipRows ? std::array{cy / area, -by / area, (by - cy) / area}
                                         : std::array<double, 3>{};
            const auto worldVector = [&](double x, double y, double z) {
                return u * (x * spanX / width) - v * (y * spanY / height) + n * z;
            };
            auto normal = worldVector(bx, by, bz).Crossed(worldVector(cx, cy, cz));
            if (normal.SquareMagnitude() <= 1.e-20)
                continue;
            if (normal.Dot(n) < 0)
                normal.Reverse();
            const auto color = shadeColor(normal.Dot(light) / normal.Magnitude());
            // Shared nodal illumination keeps the cylinder's tone continuous across facets.
            const double illuminationB = b.illumination - a.illumination;
            const double illuminationC = c.illumination - a.illumination;
            for (int y = top; y <= bottom; ++y) {
                const double dy = y + .5 - a.y;
                int first = left, last = right;
                if (clipRows) {
                    const double b = -dy * cx / area, c = dy * bx / area;
                    const std::array offsets{b, c, 1 - b - c};
                    double low = left + .5 - a.x, high = right + .5 - a.x;
                    for (std::size_t edge = 0; edge < slopes.size(); ++edge) {
                        const double slope = slopes[edge], offset = offsets[edge];
                        if (slope > 0)
                            low = std::max(low, (-edgeTolerance - offset) / slope);
                        else if (slope < 0)
                            high = std::min(high, (-edgeTolerance - offset) / slope);
                        else if (offset < -edgeTolerance)
                            high = -std::numeric_limits<double>::infinity();
                    }
                    if (low > high + 1)
                        continue;
                    // Round outwards. The original test below decides coverage at each pixel.
                    first = static_cast<int>(
                        std::clamp(std::floor(low + a.x - .5), double(left), double(right)));
                    last = static_cast<int>(
                        std::clamp(std::ceil(high + a.x - .5), double(left), double(right)));
                    if (last < first)
                        continue;
                    pixels += static_cast<std::size_t>(last - first + 1);
                    if (pixels > pixelBudget)
                        return {};
                }
                for (int x = first; x <= last; ++x) {
                    const double dx = x + .5 - a.x;
                    const double wb = (dx * cy - dy * cx) / area;
                    const double wc = (bx * dy - by * dx) / area;
                    if (wb < -edgeTolerance || wc < -edgeTolerance || wb + wc > 1. + edgeTolerance)
                        continue;
                    const double z = a.z + wb * bz + wc * cz;
                    const auto index = static_cast<std::size_t>(y) * width + x;
                    if (z > depth[index]) {
                        depth[index] = z;
                        fill.pixels[index] = smoothShading
                                                 ? shadeColor(a.illumination + wb * illuminationB +
                                                              wc * illuminationC)
                                                 : color;
                    }
                }
            }
        }
    }
    if (std::none_of(fill.pixels.begin(), fill.pixels.end(), [](auto pixel) { return pixel != 0; }))
        return {};
    return fill;
}
} // namespace bending::cad
