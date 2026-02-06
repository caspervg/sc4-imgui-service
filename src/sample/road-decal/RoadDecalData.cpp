#include "RoadDecalData.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <vector>

#include <d3d.h>
#include <d3dtypes.h>
#include <ddraw.h>

#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "GZServPtrs.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay{nullptr};

namespace
{
    constexpr float kDecalTerrainOffset = 0.05f;
    constexpr float kTerrainGridSpacing = 16.0f;
    constexpr uint32_t kRoadDecalZBias = 1;
    constexpr float kDashLength = 1.0f;
    constexpr float kGapLength = 2.0f;

    struct RoadDecalVertex
    {
        float x;
        float y;
        float z;
        DWORD diffuse;
    };

    std::vector<RoadDecalVertex> gRoadDecalVertices;
    std::vector<RoadDecalVertex> gRoadDecalActiveVertices;
    std::vector<RoadDecalVertex> gRoadDecalPreviewVertices;

    struct RoadDecalStateGuard
    {
        explicit RoadDecalStateGuard(IDirect3DDevice7* dev)
            : device(dev)
        {
            if (!device) {
                return;
            }

            okZEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZENABLE, &zEnable));
            okZWrite = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZWRITEENABLE, &zWrite));
            okLighting = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_LIGHTING, &lighting));
            okAlphaBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, &alphaBlend));
            okAlphaTest = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, &alphaTest));
            okAlphaFunc = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHAFUNC, &alphaFunc));
            okAlphaRef = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ALPHAREF, &alphaRef));
            okStencilEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_STENCILENABLE, &stencilEnable));
            okSrcBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_SRCBLEND, &srcBlend));
            okDstBlend = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_DESTBLEND, &dstBlend));
            okCullMode = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_CULLMODE, &cullMode));
            okFogEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_FOGENABLE, &fogEnable));
            okRangeFogEnable = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, &rangeFogEnable));
            okZFunc = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZFUNC, &zFunc));
            okZBias = SUCCEEDED(device->GetRenderState(D3DRENDERSTATE_ZBIAS, &zBias));
            okTexture0 = SUCCEEDED(device->GetTexture(0, &texture0));
            okTexture1 = SUCCEEDED(device->GetTexture(1, &texture1));
            okTs0ColorOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLOROP, &ts0ColorOp));
            okTs0ColorArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_COLORARG1, &ts0ColorArg1));
            okTs0AlphaOp = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAOP, &ts0AlphaOp));
            okTs0AlphaArg1 = SUCCEEDED(device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &ts0AlphaArg1));
            okTs1ColorOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_COLOROP, &ts1ColorOp));
            okTs1AlphaOp = SUCCEEDED(device->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1AlphaOp));
        }

        ~RoadDecalStateGuard()
        {
            if (!device) {
                return;
            }

            if (okZEnable) {
                device->SetRenderState(D3DRENDERSTATE_ZENABLE, zEnable);
            }
            if (okZWrite) {
                device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, zWrite);
            }
            if (okLighting) {
                device->SetRenderState(D3DRENDERSTATE_LIGHTING, lighting);
            }
            if (okAlphaBlend) {
                device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, alphaBlend);
            }
            if (okAlphaTest) {
                device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, alphaTest);
            }
            if (okAlphaFunc) {
                device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, alphaFunc);
            }
            if (okAlphaRef) {
                device->SetRenderState(D3DRENDERSTATE_ALPHAREF, alphaRef);
            }
            if (okStencilEnable) {
                device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, stencilEnable);
            }
            if (okSrcBlend) {
                device->SetRenderState(D3DRENDERSTATE_SRCBLEND, srcBlend);
            }
            if (okDstBlend) {
                device->SetRenderState(D3DRENDERSTATE_DESTBLEND, dstBlend);
            }
            if (okCullMode) {
                device->SetRenderState(D3DRENDERSTATE_CULLMODE, cullMode);
            }
            if (okFogEnable) {
                device->SetRenderState(D3DRENDERSTATE_FOGENABLE, fogEnable);
            }
            if (okRangeFogEnable) {
                device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, rangeFogEnable);
            }
            if (okZFunc) {
                device->SetRenderState(D3DRENDERSTATE_ZFUNC, zFunc);
            }
            if (okZBias) {
                device->SetRenderState(D3DRENDERSTATE_ZBIAS, zBias);
            }
            if (okTexture0) {
                device->SetTexture(0, texture0);
            }
            if (okTexture1) {
                device->SetTexture(1, texture1);
            }
            if (okTs0ColorOp) {
                device->SetTextureStageState(0, D3DTSS_COLOROP, ts0ColorOp);
            }
            if (okTs0ColorArg1) {
                device->SetTextureStageState(0, D3DTSS_COLORARG1, ts0ColorArg1);
            }
            if (okTs0AlphaOp) {
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, ts0AlphaOp);
            }
            if (okTs0AlphaArg1) {
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, ts0AlphaArg1);
            }
            if (okTs1ColorOp) {
                device->SetTextureStageState(1, D3DTSS_COLOROP, ts1ColorOp);
            }
            if (okTs1AlphaOp) {
                device->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1AlphaOp);
            }
            if (texture0) {
                texture0->Release();
                texture0 = nullptr;
            }
            if (texture1) {
                texture1->Release();
                texture1 = nullptr;
            }
        }

        IDirect3DDevice7* device = nullptr;

        bool okZEnable = false;
        bool okZWrite = false;
        bool okLighting = false;
        bool okAlphaBlend = false;
        bool okAlphaTest = false;
        bool okAlphaFunc = false;
        bool okAlphaRef = false;
        bool okStencilEnable = false;
        bool okSrcBlend = false;
        bool okDstBlend = false;
        bool okCullMode = false;
        bool okFogEnable = false;
        bool okRangeFogEnable = false;
        bool okZFunc = false;
        bool okZBias = false;
        bool okTexture0 = false;
        bool okTexture1 = false;
        bool okTs0ColorOp = false;
        bool okTs0ColorArg1 = false;
        bool okTs0AlphaOp = false;
        bool okTs0AlphaArg1 = false;
        bool okTs1ColorOp = false;
        bool okTs1AlphaOp = false;

        DWORD zEnable = 0;
        DWORD zWrite = 0;
        DWORD lighting = 0;
        DWORD alphaBlend = 0;
        DWORD alphaTest = 0;
        DWORD alphaFunc = 0;
        DWORD alphaRef = 0;
        DWORD stencilEnable = 0;
        DWORD srcBlend = 0;
        DWORD dstBlend = 0;
        DWORD cullMode = 0;
        DWORD fogEnable = 0;
        DWORD rangeFogEnable = 0;
        DWORD zFunc = 0;
        DWORD zBias = 0;
        DWORD ts0ColorOp = 0;
        DWORD ts0ColorArg1 = 0;
        DWORD ts0AlphaOp = 0;
        DWORD ts0AlphaArg1 = 0;
        DWORD ts1ColorOp = 0;
        DWORD ts1AlphaOp = 0;
        IDirectDrawSurface7* texture0 = nullptr;
        IDirectDrawSurface7* texture1 = nullptr;
    };

    DWORD StyleToColor(const int styleId)
    {
        switch (styleId) {
        case 1:
            return D3DRGBA(0.90f, 0.82f, 0.24f, 0.76f);
        case 2:
            return D3DRGBA(0.88f, 0.36f, 0.30f, 0.74f);
        default:
            return D3DRGBA(0.90f, 0.90f, 0.88f, 0.72f);
        }
    }

    float Distance3(const RoadDecalPoint& a, const RoadDecalPoint& b)
    {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    RoadDecalPoint LerpByT(const RoadDecalPoint& a, const RoadDecalPoint& b,
                           const float ta, const float tb, const float t)
    {
        const float denom = tb - ta;
        if (std::fabs(denom) < 1.0e-5f) {
            return b;
        }

        const float wa = (tb - t) / denom;
        const float wb = (t - ta) / denom;
        return {
            wa * a.x + wb * b.x,
            wa * a.y + wb * b.y,
            wa * a.z + wb * b.z,
            false
        };
    }

    RoadDecalPoint CentripetalCatmullRomPoint(const RoadDecalPoint& p0,
                                              const RoadDecalPoint& p1,
                                              const RoadDecalPoint& p2,
                                              const RoadDecalPoint& p3,
                                              const float u)
    {
        constexpr float kAlpha = 0.5f;
        const float t0 = 0.0f;
        const float t1 = t0 + std::pow((std::max)(Distance3(p0, p1), 1.0e-4f), kAlpha);
        const float t2 = t1 + std::pow((std::max)(Distance3(p1, p2), 1.0e-4f), kAlpha);
        const float t3 = t2 + std::pow((std::max)(Distance3(p2, p3), 1.0e-4f), kAlpha);

        const float t = t1 + (t2 - t1) * u;

        const auto a1 = LerpByT(p0, p1, t0, t1, t);
        const auto a2 = LerpByT(p1, p2, t1, t2, t);
        const auto a3 = LerpByT(p2, p3, t2, t3, t);

        const auto b1 = LerpByT(a1, a2, t0, t2, t);
        const auto b2 = LerpByT(a2, a3, t1, t3, t);

        return LerpByT(b1, b2, t1, t2, t);
    }

    RoadDecalPoint ClampPointToSegmentBounds(const RoadDecalPoint& p,
                                             const RoadDecalPoint& a,
                                             const RoadDecalPoint& b)
    {
        const float minX = (std::min)(a.x, b.x);
        const float maxX = (std::max)(a.x, b.x);
        const float minZ = (std::min)(a.z, b.z);
        const float maxZ = (std::max)(a.z, b.z);

        RoadDecalPoint out = p;
        out.x = std::clamp(out.x, minX, maxX);
        out.z = std::clamp(out.z, minZ, maxZ);
        out.hardCorner = false;
        return out;
    }

    cISTETerrain* GetActiveTerrain()
    {
        cISC4AppPtr app;
        cISC4City* city = app ? app->GetCity() : nullptr;
        return city ? city->GetTerrain() : nullptr;
    }

    void ConformPointsToTerrainGrid(std::vector<RoadDecalPoint>& points, cISTETerrain* terrain)
    {
        if (!terrain) {
            return;
        }

        auto sampleHeightFromGridVertices = [terrain](const float x, const float z) {
            const float cellX = std::floor(x / kTerrainGridSpacing);
            const float cellZ = std::floor(z / kTerrainGridSpacing);
            const float x0 = cellX * kTerrainGridSpacing;
            const float z0 = cellZ * kTerrainGridSpacing;
            const float x1 = x0 + kTerrainGridSpacing;
            const float z1 = z0 + kTerrainGridSpacing;

            const float tx = std::clamp((x - x0) / kTerrainGridSpacing, 0.0f, 1.0f);
            const float tz = std::clamp((z - z0) / kTerrainGridSpacing, 0.0f, 1.0f);

            const float h00 = terrain->GetAltitudeAtNearestGrid(x0, z0);
            const float h10 = terrain->GetAltitudeAtNearestGrid(x1, z0);
            const float h01 = terrain->GetAltitudeAtNearestGrid(x0, z1);
            const float h11 = terrain->GetAltitudeAtNearestGrid(x1, z1);

            const float hx0 = h00 + (h10 - h00) * tx;
            const float hx1 = h01 + (h11 - h01) * tx;
            return hx0 + (hx1 - hx0) * tz;
        };

        for (auto& point : points) {
            point.y = sampleHeightFromGridVertices(point.x, point.z) + kDecalTerrainOffset;
        }
    }

    void BuildSmoothedPolyline(const std::vector<RoadDecalPoint>& points, std::vector<RoadDecalPoint>& outPoints)
    {
        outPoints.clear();
        if (points.size() < 3) {
            outPoints = points;
            return;
        }

        outPoints.reserve(points.size() * 4);
        outPoints.push_back(points.front());

        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const auto& p1 = points[i];
            const auto& p2 = points[i + 1];

            const bool forceLinear = p1.hardCorner || p2.hardCorner;
            if (forceLinear) {
                outPoints.push_back(p2);
                continue;
            }

            const auto& p0Raw = (i == 0) ? points[i] : points[i - 1];
            const auto& p3Raw = (i + 2 < points.size()) ? points[i + 2] : points[i + 1];
            const bool p0Hard = (i > 0) && points[i - 1].hardCorner;
            const bool p3Hard = (i + 2 < points.size()) && points[i + 2].hardCorner;
            const auto& p0 = p0Hard ? p1 : p0Raw;
            const auto& p3 = p3Hard ? p2 : p3Raw;

            const float dx = p2.x - p1.x;
            const float dz = p2.z - p1.z;
            const float segmentLength = std::sqrt(dx * dx + dz * dz);
            const int steps = std::clamp(static_cast<int>(std::ceil(segmentLength / 1.0f)), 3, 12);

            for (int step = 1; step <= steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                const RoadDecalPoint sample = CentripetalCatmullRomPoint(p0, p1, p2, p3, t);
                outPoints.push_back(ClampPointToSegmentBounds(sample, p1, p2));
            }
        }
    }

    void BuildStrokeVertices(const RoadDecalStroke& stroke, std::vector<RoadDecalVertex>& outVertices)
    {
        if (stroke.points.size() < 2 || stroke.width <= 0.0f) {
            return;
        }

        std::vector<RoadDecalPoint> pathPoints;
        BuildSmoothedPolyline(stroke.points, pathPoints);
        if (pathPoints.empty()) {
            pathPoints = stroke.points;
        }

        auto* terrain = GetActiveTerrain();
        ConformPointsToTerrainGrid(pathPoints, terrain);
        const auto& points = pathPoints;

        const float halfWidth = stroke.width * 0.5f;
        const DWORD color = StyleToColor(stroke.styleId);
        const float cycleLength = kDashLength + kGapLength;
        float cyclePos = 0.0f;

        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const auto& p0 = points[i];
            const auto& p1 = points[i + 1];

            const float dx = p1.x - p0.x;
            const float dz = p1.z - p0.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len <= 0.0001f) {
                continue;
            }

            const float tx = dx / len;
            const float tz = dz / len;

            const float nx = -tz;
            const float nz = tx;

            const float p0Lx = p0.x - nx * halfWidth;
            const float p0Lz = p0.z - nz * halfWidth;
            const float p0Rx = p0.x + nx * halfWidth;
            const float p0Rz = p0.z + nz * halfWidth;

            const float p1Lx = p1.x - nx * halfWidth;
            const float p1Lz = p1.z - nz * halfWidth;
            const float p1Rx = p1.x + nx * halfWidth;
            const float p1Rz = p1.z + nz * halfWidth;

            auto emitQuad = [&](const float startT, const float endT) {
                const float ax = p0.x + (p1.x - p0.x) * startT;
                const float ay = p0.y + (p1.y - p0.y) * startT;
                const float az = p0.z + (p1.z - p0.z) * startT;
                const float bx = p0.x + (p1.x - p0.x) * endT;
                const float by = p0.y + (p1.y - p0.y) * endT;
                const float bz = p0.z + (p1.z - p0.z) * endT;

                const float aLx = ax - nx * halfWidth;
                const float aLz = az - nz * halfWidth;
                const float aRx = ax + nx * halfWidth;
                const float aRz = az + nz * halfWidth;

                const float bLx = bx - nx * halfWidth;
                const float bLz = bz - nz * halfWidth;
                const float bRx = bx + nx * halfWidth;
                const float bRz = bz + nz * halfWidth;

                RoadDecalVertex v[6] = {
                    {aLx, ay, aLz, color},
                    {bLx, by, bLz, color},
                    {bRx, by, bRz, color},
                    {aLx, ay, aLz, color},
                    {bRx, by, bRz, color},
                    {aRx, ay, aRz, color},
                };

                outVertices.insert(outVertices.end(), std::begin(v), std::end(v));
            };

            if (!stroke.dashed || cycleLength <= 0.0f) {
                RoadDecalVertex v[6] = {
                    {p0Lx, p0.y, p0Lz, color},
                    {p1Lx, p1.y, p1Lz, color},
                    {p1Rx, p1.y, p1Rz, color},
                    {p0Lx, p0.y, p0Lz, color},
                    {p1Rx, p1.y, p1Rz, color},
                    {p0Rx, p0.y, p0Rz, color},
                };
                outVertices.insert(outVertices.end(), std::begin(v), std::end(v));
                continue;
            }

            float segPos = 0.0f;
            while (segPos < len) {
                const float boundary = (cyclePos < kDashLength) ? kDashLength : cycleLength;
                float step = boundary - cyclePos;
                if (step <= 0.0f) {
                    cyclePos = 0.0f;
                    continue;
                }
                step = (std::min)(step, len - segPos);

                if (cyclePos < kDashLength) {
                    const float dashStart = segPos;
                    const float dashEnd = segPos + step;
                    if (dashEnd > dashStart) {
                        emitQuad(dashStart / len, dashEnd / len);
                    }
                }

                segPos += step;
                cyclePos += step;
                if (cyclePos >= cycleLength - 1.0e-4f) {
                    cyclePos = 0.0f;
                }
            }
        }
    }

    void DrawVertexBuffer(IDirect3DDevice7* device, const std::vector<RoadDecalVertex>& verts)
    {
        if (verts.empty()) {
            return;
        }

        const HRESULT hr = device->DrawPrimitive(D3DPT_TRIANGLELIST,
                                                 D3DFVF_XYZ | D3DFVF_DIFFUSE,
                                                 const_cast<RoadDecalVertex*>(verts.data()),
                                                 static_cast<DWORD>(verts.size()),
                                                 D3DDP_WAIT);

        if (FAILED(hr)) {
            LOG_WARN("RoadDecalSample: DrawPrimitive failed hr=0x{:08X}",
                     static_cast<uint32_t>(hr));
        }
    }
}

std::vector<RoadDecalStroke> gRoadDecalStrokes;

void RebuildRoadDecalGeometry()
{
    gRoadDecalVertices.clear();
    gRoadDecalVertices.reserve(gRoadDecalStrokes.size() * 32);

    for (const auto& stroke : gRoadDecalStrokes) {
        BuildStrokeVertices(stroke, gRoadDecalVertices);
    }

    LOG_INFO("RoadDecalSample: rebuilt geometry, {} strokes -> {} verts",
             static_cast<uint32_t>(gRoadDecalStrokes.size()),
             static_cast<uint32_t>(gRoadDecalVertices.size()));
}

void DrawRoadDecals()
{
    if (gRoadDecalVertices.empty() && gRoadDecalActiveVertices.empty() && gRoadDecalPreviewVertices.empty()) {
        return;
    }

    auto* imguiService = gImGuiServiceForD3DOverlay.load(std::memory_order_acquire);
    if (!imguiService) {
        return;
    }

    IDirect3DDevice7* device = nullptr;
    IDirectDraw7* dd = nullptr;

    if (!imguiService->AcquireD3DInterfaces(&device, &dd)) {
        return;
    }

    if (dd) {
        dd->Release();
        dd = nullptr;
    }
    if (!device) {
        return;
    }

    {
        RoadDecalStateGuard state(device);

        device->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
        device->SetRenderState(D3DRENDERSTATE_FOGENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

        device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0);
        device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);

        device->SetRenderState(D3DRENDERSTATE_ZBIAS, kRoadDecalZBias);

        device->SetTexture(0, nullptr);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        DrawVertexBuffer(device, gRoadDecalVertices);
        DrawVertexBuffer(device, gRoadDecalActiveVertices);
        DrawVertexBuffer(device, gRoadDecalPreviewVertices);
    }

    device->Release();
}

void SetRoadDecalActiveStroke(const RoadDecalStroke* stroke)
{
    gRoadDecalActiveVertices.clear();
    if (!stroke) {
        return;
    }

    BuildStrokeVertices(*stroke, gRoadDecalActiveVertices);
}

void SetRoadDecalPreviewSegment(bool enabled,
                                const RoadDecalPoint& from,
                                const RoadDecalPoint& to,
                                int styleId,
                                float width,
                                bool dashed)
{
    gRoadDecalPreviewVertices.clear();
    if (!enabled) {
        return;
    }

    RoadDecalStroke previewStroke{};
    previewStroke.styleId = styleId;
    previewStroke.width = width;
    previewStroke.dashed = dashed;
    previewStroke.points.push_back(from);
    previewStroke.points.push_back(to);
    BuildStrokeVertices(previewStroke, gRoadDecalPreviewVertices);
}
