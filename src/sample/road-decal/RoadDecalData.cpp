#include "RoadDecalData.hpp"

#include <atomic>
#include <cmath>
#include <vector>

#include <d3d.h>
#include <d3dtypes.h>
#include <ddraw.h>

#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay{nullptr};
std::atomic<int> gRoadDecalZBias{1};

namespace
{
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
    };

    DWORD StyleToColor(const int styleId)
    {
        switch (styleId) {
        case 1:
            return D3DRGBA(1.0f, 0.9f, 0.2f, 0.9f);
        case 2:
            return D3DRGBA(1.0f, 0.3f, 0.3f, 0.9f);
        default:
            return D3DRGBA(1.0f, 1.0f, 1.0f, 0.9f);
        }
    }

    void BuildStrokeVertices(const RoadDecalStroke& stroke, std::vector<RoadDecalVertex>& outVertices)
    {
        if (stroke.points.size() < 2 || stroke.width <= 0.0f) {
            return;
        }

        const float halfWidth = stroke.width * 0.5f;
        const DWORD color = StyleToColor(stroke.styleId);

        for (size_t i = 0; i + 1 < stroke.points.size(); ++i) {
            const auto& p0 = stroke.points[i];
            const auto& p1 = stroke.points[i + 1];

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

            RoadDecalVertex v[6] = {
                {p0Lx, p0.y, p0Lz, color},
                {p1Lx, p1.y, p1Lz, color},
                {p1Rx, p1.y, p1Rz, color},
                {p0Lx, p0.y, p0Lz, color},
                {p1Rx, p1.y, p1Rz, color},
                {p0Rx, p0.y, p0Rz, color},
            };

            outVertices.insert(outVertices.end(), std::begin(v), std::end(v));
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
        device->SetRenderState(D3DRENDERSTATE_FOGENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

        device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_ALWAYS);
        device->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0);
        device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);

        device->SetRenderState(D3DRENDERSTATE_ZBIAS, gRoadDecalZBias.load(std::memory_order_relaxed));

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
                                float width)
{
    gRoadDecalPreviewVertices.clear();
    if (!enabled) {
        return;
    }

    RoadDecalStroke previewStroke{};
    previewStroke.styleId = styleId;
    previewStroke.width = width;
    previewStroke.points.push_back(from);
    previewStroke.points.push_back(to);
    BuildStrokeVertices(previewStroke, gRoadDecalPreviewVertices);
}
