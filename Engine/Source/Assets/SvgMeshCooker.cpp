#include "Assets/SvgMeshCooker.h"

#include "Assets/SvgRasterizer.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Matrix3x3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>

#if defined(GE_HAVE_THORVG) && defined(GE_HAVE_LIBTESS2)
#include <thorvg.h>
#include <tesselator.h>
#endif

namespace GameEngine
{

#if defined(GE_HAVE_THORVG) && defined(GE_HAVE_LIBTESS2) && \
    defined(TVG_VERSION_MAJOR) && TVG_VERSION_MAJOR >= 1
namespace
{
constexpr float kEpsilon = 1.0e-5f;

using Point = Mathematics::Vector2;
using Matrix = Mathematics::Matrix3x3;

struct Subpath
{
    std::vector<Point> Points;
    bool Closed = false;
};

struct CookContext
{
    SvgCookedMesh& Mesh;
    const SvgMeshCookOptions& Options;
    float CurveTolerance = 0.05f;
    std::string* Reason = nullptr;
    bool Unsupported = false;
    bool Failed = false;
};

struct ThorvgPaintDeleter
{
    void operator()(tvg::Paint* paint) const noexcept { tvg::Paint::rel(paint); }
};

using PicturePtr = std::unique_ptr<tvg::Picture, ThorvgPaintDeleter>;
using TessPtr = std::unique_ptr<TESStesselator, decltype(&tessDeleteTess)>;

void SetReason(CookContext& ctx, std::string reason, bool unsupported)
{
    if (ctx.Reason && ctx.Reason->empty())
        *ctx.Reason = std::move(reason);
    ctx.Unsupported = ctx.Unsupported || unsupported;
    ctx.Failed = ctx.Failed || !unsupported;
}

float Cross(Point a, Point b)
{
    return Mathematics::Vector3::Cross({a.x, a.y, 0.0f}, {b.x, b.y, 0.0f}).z;
}

// SVG path segments below the cooker tolerance have no stroke direction.
Point StrokeDirection(Point p)
{
    return p.Length() > kEpsilon ? p.Normalize() : Point{};
}

Point LeftNormal(Point direction) { return {-direction.y, direction.x}; }

bool NearlyEqual(Point a, Point b)
{
    return std::abs(a.x - b.x) <= kEpsilon && std::abs(a.y - b.y) <= kEpsilon;
}

Matrix FromThorvg(const tvg::Matrix& m)
{
    // ThorVG exposes row coefficients; Mathematics matrices store columns.
    return Matrix(glm::mat3(m.e11, m.e21, 0.0f,
                            m.e12, m.e22, 0.0f,
                            m.e13, m.e23, 1.0f));
}

Point TransformSvgPoint(const Matrix& m, Point p)
{
    const glm::vec3 transformed = m.GetGLM() * glm::vec3(p.x, p.y, 1.0f);
    return {transformed.x, transformed.y};
}

struct ViewportVertex
{
    Point Position;
    // Bit 0: silhouette; bit 1: shared-edge owner. Describes the incoming edge.
    uint8_t Incoming = 0;
};

std::vector<ViewportVertex> ClipViewportEdge(const std::vector<ViewportVertex>& polygon,
                                            bool horizontal, float boundary, bool keepGreater)
{
    std::vector<ViewportVertex> clipped;
    if (polygon.empty())
        return clipped;
    auto previous = polygon.back();
    for (const auto& current : polygon)
    {
        const float a = horizontal ? previous.Position.x : previous.Position.y;
        const float b = horizontal ? current.Position.x : current.Position.y;
        const bool aInside = keepGreater ? a >= boundary : a <= boundary;
        const bool bInside = keepGreater ? b >= boundary : b <= boundary;
        if (aInside != bInside)
        {
            // Adjacent triangles traverse their shared edge in opposite directions.
            // Compute its intersection in one canonical direction so both retain the
            // exact same endpoint after clipping, including fractional coordinates.
            const bool reverse = previous.Position.x > current.Position.x ||
                (previous.Position.x == current.Position.x && previous.Position.y > current.Position.y);
            const Point start = reverse ? current.Position : previous.Position;
            const Point end = reverse ? previous.Position : current.Position;
            const float startCoordinate = horizontal ? start.x : start.y;
            const float endCoordinate = horizontal ? end.x : end.y;
            const float t = (boundary - startCoordinate) / (endCoordinate - startCoordinate);
            Point intersection = start + (end - start) * t;
            if (horizontal)
                intersection.x = boundary;
            else
                intersection.y = boundary;
            clipped.push_back({intersection, aInside ? current.Incoming : uint8_t{1}});
        }
        if (bInside)
            clipped.push_back(current);
        previous = current;
    }
    return clipped;
}

uint8_t IncomingEdge(const SvgMeshTriangle& triangle, uint8_t edge)
{
    return static_cast<uint8_t>(((triangle.EdgeFlags >> edge) & 1u) |
                               (((triangle.EdgeFlags >> (edge + 3u)) & 1u) << 1u));
}

void SetTriangleEdge(SvgMeshTriangle& triangle, uint8_t edge, uint8_t flags)
{
    triangle.EdgeFlags |= static_cast<uint8_t>(((flags & 1u) << edge) |
                                              (((flags >> 1u) & 1u) << (edge + 3u)));
}

bool ClipToViewport(CookContext& ctx)
{
    std::vector<SvgMeshTriangle> clipped;
    clipped.reserve(ctx.Mesh.Triangles.size());
    for (const auto& triangle : ctx.Mesh.Triangles)
    {
        std::vector<ViewportVertex> polygon{
            {{triangle.X0, triangle.Y0}, IncomingEdge(triangle, 2u)},
            {{triangle.X1, triangle.Y1}, IncomingEdge(triangle, 0u)},
            {{triangle.X2, triangle.Y2}, IncomingEdge(triangle, 1u)}};
        polygon = ClipViewportEdge(polygon, true, 0.0f, true);
        polygon = ClipViewportEdge(polygon, true, ctx.Mesh.Width, false);
        polygon = ClipViewportEdge(polygon, false, 0.0f, true);
        polygon = ClipViewportEdge(polygon, false, ctx.Mesh.Height, false);
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            const Point a = polygon[0].Position;
            const Point b = polygon[i].Position;
            const Point c = polygon[i + 1].Position;
            if (std::abs(Cross(b - a, c - a)) <= kEpsilon)
                continue;
            if (clipped.size() >= ctx.Options.MaxTriangles)
            {
                SetReason(ctx, "SVG viewport clipping exceeds the triangle budget", true);
                return false;
            }
            SvgMeshTriangle out{a.x, a.y, b.x, b.y, c.x, c.y, triangle.Color};
            SetTriangleEdge(out, 0u, i == 1 ? polygon[i].Incoming : uint8_t{0});
            SetTriangleEdge(out, 1u, polygon[i + 1].Incoming);
            SetTriangleEdge(out, 2u, i + 2 == polygon.size() ? polygon[0].Incoming : uint8_t{2});
            clipped.push_back(out);
        }
    }
    ctx.Mesh.Triangles = std::move(clipped);
    return true;
}

uint32 PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a, float opacity)
{
    const uint32 alpha = static_cast<uint32>(std::clamp(
        std::lround(static_cast<float>(a) * std::clamp(opacity, 0.0f, 1.0f)),
        0l, 255l));
    return static_cast<uint32>(r) |
           (static_cast<uint32>(g) << 8u) |
           (static_cast<uint32>(b) << 16u) |
           (alpha << 24u);
}

float DistanceToLine(Point p, Point a, Point b)
{
    const Point ab = b - a;
    const float len = (ab).Length();
    return len > kEpsilon ? std::abs(Cross(ab, p - a)) / len : (p - a).Length();
}

void FlattenCubic(Point p0, Point p1, Point p2, Point p3,
                  float tolerance, uint32 depth, std::vector<Point>& out)
{
    if (depth >= 12u ||
        std::max(DistanceToLine(p1, p0, p3), DistanceToLine(p2, p0, p3)) <= tolerance)
    {
        if (out.empty() || !NearlyEqual(out.back(), p3))
            out.push_back(p3);
        return;
    }

    const Point p01 = (p0 + p1) * 0.5f;
    const Point p12 = (p1 + p2) * 0.5f;
    const Point p23 = (p2 + p3) * 0.5f;
    const Point p012 = (p01 + p12) * 0.5f;
    const Point p123 = (p12 + p23) * 0.5f;
    const Point mid = (p012 + p123) * 0.5f;

    FlattenCubic(p0, p01, p012, mid, tolerance, depth + 1u, out);
    FlattenCubic(mid, p123, p23, p3, tolerance, depth + 1u, out);
}

void FinishSubpath(Subpath& current, std::vector<Subpath>& result)
{
    if (current.Points.size() >= 2u)
    {
        if (current.Closed && current.Points.size() > 2u &&
            NearlyEqual(current.Points.front(), current.Points.back()))
            current.Points.pop_back();
        result.push_back(std::move(current));
    }
    current = {};
}

std::vector<Subpath> FlattenShapePath(const tvg::Shape& shape, float tolerance,
                                      CookContext& ctx)
{
    const tvg::PathCommand* commands = nullptr;
    const tvg::Point* points = nullptr;
    uint32_t commandCount = 0;
    uint32_t pointCount = 0;
    if (shape.path(&commands, &commandCount, &points, &pointCount) != tvg::Result::Success)
    {
        SetReason(ctx, "ThorVG did not expose the SVG path", false);
        return {};
    }

    std::vector<Subpath> result;
    Subpath current;
    Point cursor{};
    uint32_t pointIndex = 0;



    for (uint32_t i = 0; i < commandCount; ++i)
    {
        switch (commands[i])
        {
        case tvg::PathCommand::MoveTo:
            if (pointIndex >= pointCount)
            {
                SetReason(ctx, "SVG path command data is truncated", false);
                return {};
            }
            FinishSubpath(current, result);
            cursor = {points[pointIndex].x, points[pointIndex].y};
            ++pointIndex;
            current.Points.push_back(cursor);
            break;
        case tvg::PathCommand::LineTo:
            if (pointIndex >= pointCount)
            {
                SetReason(ctx, "SVG path command data is truncated", false);
                return {};
            }
            cursor = {points[pointIndex].x, points[pointIndex].y};
            ++pointIndex;
            if (current.Points.empty() || !NearlyEqual(current.Points.back(), cursor))
                current.Points.push_back(cursor);
            break;
        case tvg::PathCommand::CubicTo:
            if (pointIndex + 2u >= pointCount || current.Points.empty())
            {
                SetReason(ctx, "SVG cubic path command data is truncated", false);
                return {};
            }
            {
                const Point c1{points[pointIndex].x, points[pointIndex].y};
                const Point c2{points[pointIndex + 1u].x, points[pointIndex + 1u].y};
                const Point end{points[pointIndex + 2u].x, points[pointIndex + 2u].y};
                pointIndex += 3u;
                FlattenCubic(cursor, c1, c2, end, tolerance, 0u, current.Points);
                cursor = end;
            }
            break;
        case tvg::PathCommand::Close:
            current.Closed = true;
            FinishSubpath(current, result);
            break;
        }
    }
    FinishSubpath(current, result);
    return result;
}

void TransformContours(std::vector<std::vector<Point>>& contours, const Matrix& transform)
{
    for (auto& contour : contours)
        for (Point& point : contour)
            point = TransformSvgPoint(transform, point);
}

bool Tessellate(const std::vector<std::vector<Point>>& contours, int windingRule,
                uint32 color, CookContext& ctx)
{
    TessPtr tess(tessNewTess(nullptr), &tessDeleteTess);
    if (!tess)
    {
        SetReason(ctx, "libtess2 allocation failed", false);
        return false;
    }

    uint32_t contourCount = 0;
    for (const auto& contour : contours)
    {
        if (contour.size() < 3u)
            continue;
        tessAddContour(tess.get(), 2, contour.data(), static_cast<int>(sizeof(Point)),
                       static_cast<int>(contour.size()));
        ++contourCount;
    }
    if (contourCount == 0u)
        return true;

    if (tessTesselate(tess.get(), windingRule, TESS_CONNECTED_POLYGONS,
                      3, 2, nullptr) == 0)
    {
        SetReason(ctx, "libtess2 could not tessellate the SVG path", false);
        return false;
    }

    const int triangleCount = tessGetElementCount(tess.get());
    if (triangleCount < 0 ||
        ctx.Mesh.Triangles.size() + static_cast<size_t>(triangleCount) > ctx.Options.MaxTriangles)
    {
        SetReason(ctx, "SVG exceeds the cooked mesh triangle limit", true);
        return false;
    }

    const TESSreal* vertices = tessGetVertices(tess.get());
    const TESSindex* elements = tessGetElements(tess.get());
    for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        const TESSindex* triangle = elements + triangleIndex * 6;
        if (triangle[0] == TESS_UNDEF || triangle[1] == TESS_UNDEF || triangle[2] == TESS_UNDEF)
            continue;

        SvgMeshTriangle out;
        out.X0 = vertices[triangle[0] * 2];
        out.Y0 = vertices[triangle[0] * 2 + 1];
        out.X1 = vertices[triangle[1] * 2];
        out.Y1 = vertices[triangle[1] * 2 + 1];
        out.X2 = vertices[triangle[2] * 2];
        out.Y2 = vertices[triangle[2] * 2 + 1];
        out.Color = color;
        for (uint8_t edge = 0; edge < 3u; ++edge)
        {
            if (triangle[3u + edge] == TESS_UNDEF)
                out.EdgeFlags |= static_cast<uint8_t>(1u << edge);
            else if (triangleIndex < triangle[3u + edge])
                out.EdgeFlags |= static_cast<uint8_t>(1u << (edge + 3u));
        }

        const float twiceArea = Cross({out.X1 - out.X0, out.Y1 - out.Y0},
                                      {out.X2 - out.X0, out.Y2 - out.Y0});
        if (std::abs(twiceArea) > kEpsilon)
            ctx.Mesh.Triangles.push_back(out);
    }
    return true;
}

bool LineIntersection(Point p, Point r, Point q, Point s, Point& out)
{
    const float denominator = Cross(r, s);
    if (std::abs(denominator) <= kEpsilon)
        return false;
    out = p + r * (Cross(q - p, s) / denominator);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

void AppendArc(std::vector<Point>& out, Point center, Point fromVector,
               Point toVector, float signedDelta, float tolerance)
{
    const float radius = (fromVector).Length();
    if (radius <= kEpsilon)
        return;

    const float safeTolerance = std::clamp(tolerance, 0.001f, radius);
    const float maxStep = std::max(0.05f,
        2.0f * std::acos(std::clamp(1.0f - safeTolerance / radius, -1.0f, 1.0f)));
    const uint32_t segments = std::max(1u, static_cast<uint32_t>(
        std::ceil(std::abs(signedDelta) / maxStep)));
    const float start = std::atan2(fromVector.y, fromVector.x);

    for (uint32_t i = 1; i <= segments; ++i)
    {
        if (i == segments)
        {
            out.push_back(center + toVector);
            continue;
        }
        const float angle = start + signedDelta * (static_cast<float>(i) / segments);
        out.push_back({center.x + std::cos(angle) * radius,
                       center.y + std::sin(angle) * radius});
    }
}

float SignedAngle(Point from, Point to)
{
    return std::atan2(Cross(from, to), Point::Dot(from, to));
}

void AppendStrokeJoin(std::vector<Point>& out, const std::vector<Point>& points,
                      size_t index, size_t previous, size_t next, float side,
                      float radius, tvg::StrokeJoin join, float miterLimit, float tolerance)
{
    const Point p = points[index];
    const Point prevDirection = StrokeDirection(p - points[previous]);
    const Point nextDirection = StrokeDirection(points[next] - p);
    if ((prevDirection).Length() <= kEpsilon || (nextDirection).Length() <= kEpsilon)
        return;

    const Point prevNormal = LeftNormal(prevDirection) * side;
    const Point nextNormal = LeftNormal(nextDirection) * side;
    const Point prevOffset = p + prevNormal * radius;
    const Point nextOffset = p + nextNormal * radius;
    const float turn = Cross(prevDirection, nextDirection);

    if (std::abs(turn) <= kEpsilon && Point::Dot(prevDirection, nextDirection) > 0.0f)
    {
        out.push_back(nextOffset);
        return;
    }

    Point intersection{};
    const bool intersects = LineIntersection(prevOffset, prevDirection,
                                             nextOffset, nextDirection, intersection);
    const bool outer = turn * side < 0.0f;
    if (!outer)
    {
        if (intersects)
            out.push_back(intersection);
        else
        {
            out.push_back(prevOffset);
            out.push_back(nextOffset);
        }
        return;
    }

    if (join == tvg::StrokeJoin::Round)
    {
        out.push_back(prevOffset);
        float delta = SignedAngle(prevNormal, nextNormal);
        if (turn > 0.0f && delta < 0.0f)
            delta += 2.0f * std::numbers::pi_v<float>;
        else if (turn < 0.0f && delta > 0.0f)
            delta -= 2.0f * std::numbers::pi_v<float>;
        AppendArc(out, p, prevNormal * radius, nextNormal * radius, delta, tolerance);
        return;
    }

    if (join == tvg::StrokeJoin::Miter && intersects &&
        (intersection - p).Length() <= std::max(1.0f, miterLimit) * radius)
    {
        out.push_back(intersection);
        return;
    }

    out.push_back(prevOffset);
    out.push_back(nextOffset);
}

std::vector<Point> BuildStrokeSide(const std::vector<Point>& points, bool closed,
                                   float side, float radius, tvg::StrokeJoin join,
                                   float miterLimit, float tolerance)
{
    std::vector<Point> out;
    const size_t count = points.size();
    if (count < 2u)
        return out;



    if (!closed)
    {
        const Point firstDirection = StrokeDirection(points[1] - points[0]);
        out.push_back(points[0] + LeftNormal(firstDirection) * (side * radius));
        for (size_t i = 1; i + 1 < count; ++i)
            AppendStrokeJoin(out, points, i, i - 1u, i + 1u, side, radius, join, miterLimit, tolerance);
        const Point lastDirection = StrokeDirection(points[count - 1u] - points[count - 2u]);
        out.push_back(points.back() + LeftNormal(lastDirection) * (side * radius));
    }
    else
    {
        for (size_t i = 0; i < count; ++i)
            AppendStrokeJoin(out, points, i, (i + count - 1u) % count, (i + 1u) % count,
                             side, radius, join, miterLimit, tolerance);
    }
    return out;
}

float SignedArea(const std::vector<Point>& contour)
{
    float area = 0.0f;
    for (size_t i = 0; i < contour.size(); ++i)
    {
        const Point a = contour[i];
        const Point b = contour[(i + 1u) % contour.size()];
        area += Cross(a, b);
    }
    return area * 0.5f;
}

void SetPositiveWinding(std::vector<Point>& contour, bool positive)
{
    if ((SignedArea(contour) > 0.0f) != positive)
        std::reverse(contour.begin(), contour.end());
}

void AppendRoundCap(std::vector<Point>& contour, Point center, Point from, Point to,
                    Point desiredMidDirection, float tolerance)
{
    const float start = std::atan2(from.y, from.x);
    const float candidateA = std::numbers::pi_v<float>;
    const float candidateB = -candidateA;
    const Point midA{std::cos(start + candidateA * 0.5f),
                     std::sin(start + candidateA * 0.5f)};
    const float delta = Point::Dot(midA, desiredMidDirection) >= 0.0f ? candidateA : candidateB;
    AppendArc(contour, center, from, to, delta, tolerance);
}

std::vector<std::vector<Point>> BuildStrokeContours(const std::vector<Subpath>& subpaths,
                                                    float width, tvg::StrokeCap cap,
                                                    tvg::StrokeJoin join, float miterLimit,
                                                    float tolerance)
{
    const float radius = width * 0.5f;
    std::vector<std::vector<Point>> result;
    for (const Subpath& subpath : subpaths)
    {
        if (subpath.Points.size() < 2u)
            continue;

        std::vector<Point> positive = BuildStrokeSide(subpath.Points, subpath.Closed,
                                                      1.0f, radius, join, miterLimit, tolerance);
        std::vector<Point> negative = BuildStrokeSide(subpath.Points, subpath.Closed,
                                                      -1.0f, radius, join, miterLimit, tolerance);
        if (positive.size() < 2u || negative.size() < 2u)
            continue;

        if (subpath.Closed)
        {
            const bool positiveIsOuter = std::abs(SignedArea(positive)) >=
                                         std::abs(SignedArea(negative));
            SetPositiveWinding(positive, positiveIsOuter);
            SetPositiveWinding(negative, !positiveIsOuter);
            result.push_back(std::move(positive));
            result.push_back(std::move(negative));
            continue;
        }

        const Point firstDirection = StrokeDirection(subpath.Points[1] - subpath.Points[0]);
        const Point lastDirection = StrokeDirection(subpath.Points.back() -
                                              subpath.Points[subpath.Points.size() - 2u]);
        if (cap == tvg::StrokeCap::Square)
        {
            positive.front() = positive.front() - firstDirection * radius;
            negative.front() = negative.front() - firstDirection * radius;
            positive.back() = positive.back() + lastDirection * radius;
            negative.back() = negative.back() + lastDirection * radius;
        }

        std::vector<Point> outline = std::move(positive);
        if (cap == tvg::StrokeCap::Round)
        {
            const Point center = subpath.Points.back();
            AppendRoundCap(outline, center, outline.back() - center,
                           negative.back() - center, lastDirection, tolerance);
        }
        else
        {
            outline.push_back(negative.back());
        }

        for (auto it = std::next(negative.rbegin()); it != negative.rend(); ++it)
            outline.push_back(*it);

        if (cap == tvg::StrokeCap::Round)
        {
            const Point center = subpath.Points.front();
            AppendRoundCap(outline, center, outline.back() - center,
                           outline.front() - center, firstDirection * -1.0f, tolerance);
        }
        SetPositiveWinding(outline, true);
        result.push_back(std::move(outline));
    }
    return result;
}

bool HasUnsupportedComposition(const tvg::Paint& paint, bool allowViewportClip = false)
{
    const tvg::Paint* maskTarget = nullptr;
    return (!allowViewportClip && paint.clip() != nullptr) ||
           paint.mask(&maskTarget) != tvg::MaskMethod::None ||
           maskTarget != nullptr;
}

bool CookShape(const tvg::Shape& shape, const Matrix& world, float opacity,
               CookContext& ctx, bool allowViewportClip)
{
    if (HasUnsupportedComposition(shape, allowViewportClip))
    {
        SetReason(ctx, "clip paths and masks require raster fallback", true);
        return false;
    }

    // Object opacity composites the fill and stroke together. Baking it into
    // both paints would apply opacity twice where their geometry overlaps.
    uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 0;
    uint8_t strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 0;
    if (opacity < 1.0f && shape.strokeWidth() > 0.0f &&
        shape.fill(&fillR, &fillG, &fillB, &fillA) == tvg::Result::Success && fillA > 0 &&
        shape.strokeFill(&strokeR, &strokeG, &strokeB, &strokeA) == tvg::Result::Success && strokeA > 0)
    {
        SetReason(ctx, "fill/stroke opacity compositing requires raster fallback", true);
        return false;
    }

    const std::vector<Subpath> subpaths = FlattenShapePath(shape, ctx.CurveTolerance, ctx);
    if (ctx.Failed)
        return false;

    uint8_t r = 0, g = 0, b = 0, a = 0;
    if (shape.fill() != nullptr)
    {
        SetReason(ctx, "gradient fills require raster fallback", true);
        return false;
    }
    if (shape.fill(&r, &g, &b, &a) == tvg::Result::Success && a > 0u)
    {
        std::vector<std::vector<Point>> contours;
        for (const Subpath& subpath : subpaths)
            if (subpath.Points.size() >= 3u)
                contours.push_back(subpath.Points);
        TransformContours(contours, world);
        const int winding = shape.fillRule() == tvg::FillRule::EvenOdd
            ? TESS_WINDING_ODD : TESS_WINDING_NONZERO;
        if (!Tessellate(contours, winding, PackColor(r, g, b, a, opacity), ctx))
            return false;
    }

    const float strokeWidth = shape.strokeWidth();
    if (strokeWidth <= 0.0f)
        return true;
    if (shape.strokeFill() != nullptr)
    {
        SetReason(ctx, "gradient strokes require raster fallback", true);
        return false;
    }
    const float* dashPattern = nullptr;
    if (shape.strokeDash(&dashPattern) > 0u)
    {
        SetReason(ctx, "dashed strokes require raster fallback", true);
        return false;
    }
    if (shape.strokeFill(&r, &g, &b, &a) != tvg::Result::Success || a == 0u)
        return true;

    auto strokeContours = BuildStrokeContours(subpaths, strokeWidth, shape.strokeCap(),
                                              shape.strokeJoin(), shape.strokeMiterlimit(),
                                              ctx.CurveTolerance);
    TransformContours(strokeContours, world);
    return Tessellate(strokeContours, TESS_WINDING_NONZERO,
                      PackColor(r, g, b, a, opacity), ctx);
}

bool WalkPaint(const tvg::Paint& paint, const Matrix& parentTransform,
               float parentOpacity, CookContext& ctx, bool allowViewportClip = false,
               uint32 depth = 0u)
{
    if (!paint.visible())
        return true;
    if (HasUnsupportedComposition(paint, allowViewportClip))
    {
        SetReason(ctx, "clip paths, masks, and filtered scenes require raster fallback at scene depth " +
                       std::to_string(depth), true);
        return false;
    }

    const tvg::Matrix& localTvg = const_cast<tvg::Paint&>(paint).transform();
    const Matrix world = Matrix(parentTransform.GetGLM() * FromThorvg(localTvg).GetGLM());
    const float opacity = parentOpacity * (static_cast<float>(paint.opacity()) / 255.0f);

    switch (paint.type())
    {
    case tvg::Type::Shape:
        return CookShape(static_cast<const tvg::Shape&>(paint), world, opacity,
                         ctx, allowViewportClip);
    case tvg::Type::Scene:
        // Group opacity applies after children are composited, not separately
        // to each child. Single-child groups can still pass opacity through;
        // nested branching groups are checked again with inherited opacity.
        if (opacity < 1.0f && static_cast<const tvg::Scene&>(paint).paints().size() > 1u)
        {
            SetReason(ctx, "group opacity compositing requires raster fallback", true);
            return false;
        }
        for (const tvg::Paint* child : static_cast<const tvg::Scene&>(paint).paints())
            if (child && !WalkPaint(*child, world, opacity, ctx,
                                    allowViewportClip && depth == 0u, depth + 1u))
                return false;
        return true;
    case tvg::Type::Picture:
    case tvg::Type::Text:
        SetReason(ctx, "embedded images and text require raster fallback", true);
        return false;
    default:
        SetReason(ctx, "unsupported SVG paint node", true);
        return false;
    }
}
} // namespace
#endif

SvgMeshCookStatus CookSvgToMesh(std::string_view svgUtf8,
                                SvgCookedMesh& outMesh,
                                std::string* outReason,
                                const SvgMeshCookOptions& options)
{
    outMesh = {};
    if (outReason)
        outReason->clear();

#if !defined(GE_HAVE_THORVG) || !defined(GE_HAVE_LIBTESS2) || \
    !defined(TVG_VERSION_MAJOR) || TVG_VERSION_MAJOR < 1
    (void)svgUtf8;
    (void)options;
    if (outReason)
        *outReason = "cooked SVG meshes require ThorVG 1.x and libtess2";
    return SvgMeshCookStatus::Unsupported;
#else
    if (svgUtf8.empty())
    {
        if (outReason)
            *outReason = "SVG source is empty";
        return SvgMeshCookStatus::Invalid;
    }
    if (!EnsureSvgBackendInitialized())
    {
        if (outReason)
            *outReason = "ThorVG initialization failed";
        return SvgMeshCookStatus::Invalid;
    }

    PicturePtr picture(tvg::Picture::gen());
    if (!picture)
    {
        if (outReason)
            *outReason = "ThorVG picture allocation failed";
        return SvgMeshCookStatus::Invalid;
    }
    if (picture->load(svgUtf8.data(), static_cast<uint32_t>(svgUtf8.size()),
                      "svg", nullptr, true) != tvg::Result::Success)
    {
        if (outReason)
            *outReason = "ThorVG could not parse the SVG";
        return SvgMeshCookStatus::Invalid;
    }

    if (picture->size(&outMesh.Width, &outMesh.Height) != tvg::Result::Success ||
        outMesh.Width <= 0.0f || outMesh.Height <= 0.0f)
    {
        if (outReason)
            *outReason = "SVG has no positive viewport size";
        outMesh = {};
        return SvgMeshCookStatus::Invalid;
    }

    const tvg::Paint* root = nullptr;
    std::unique_ptr<tvg::Accessor> accessor(tvg::Accessor::gen());
    if (!accessor ||
        accessor->set(picture.get(),
            [](const tvg::Paint* paint, void* data)
            {
                if (paint && paint->type() != tvg::Type::Picture)
                {
                    *static_cast<const tvg::Paint**>(data) = paint;
                    return false;
                }
                return true;
            }, &root) != tvg::Result::Success || !root)
    {
        if (outReason)
            *outReason = "ThorVG did not expose an SVG scene tree";
        outMesh = {};
        return SvgMeshCookStatus::Invalid;
    }

    const float longestAxis = std::max(outMesh.Width, outMesh.Height);
    CookContext context{outMesh, options,
                        options.CurveTolerance > 0.0f
                            ? options.CurveTolerance
                            : std::max(0.01f, longestAxis / 512.0f),
                        outReason};
    // Loader wrapper clips represent the intrinsic SVG viewport. Apply that
    // rectangle to the cooked geometry, before CSS sizing/positioning can make
    // it differ from the element's background clip. Authored clips stay raster.
    if (!WalkPaint(*root, Matrix{}, 1.0f, context, true) || !ClipToViewport(context))
    {
        outMesh = {};
        return context.Unsupported ? SvgMeshCookStatus::Unsupported
                                   : SvgMeshCookStatus::Invalid;
    }
    if (outMesh.Triangles.empty())
    {
        if (outReason && outReason->empty())
            *outReason = "SVG contains no supported visible geometry";
        outMesh = {};
        return SvgMeshCookStatus::Unsupported;
    }
    return SvgMeshCookStatus::Success;
#endif
}

} // namespace GameEngine
