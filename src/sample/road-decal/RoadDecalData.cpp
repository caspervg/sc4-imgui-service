// ReSharper disable CppDFAConstantConditions
#define NOMINMAX

#include "RoadDecalData.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d.h>
#include <d3dtypes.h>
#include <ddraw.h>

#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "GZServPtrs.h"
#include "cIGZIStream.h"
#include "cIGZOStream.h"
#include "cIGZSerializable.h"
#include "cIGZVariant.h"
#include "public/cIGZImGuiService.h"
#include "utils/Logger.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

std::atomic<cIGZImGuiService*> gImGuiServiceForD3DOverlay{nullptr};
std::vector<RoadMarkupLayer> gRoadMarkupLayers;
int gActiveLayerIndex = 0;
int gSelectedLayerIndex = -1;
int gSelectedStrokeIndex = -1;

namespace
{
    constexpr float kDecalTerrainOffset = 0.05f;
    constexpr float kTerrainGridSpacing = 16.0f;
    constexpr uint32_t kRoadDecalZBias = 1;
    constexpr float kMinLen = 1.0e-4f;
    constexpr float kDoubleYellowSpacing = 0.10f;
    constexpr float kTileSize = 16.0f;
    constexpr float kMinorGridSize = 2.0f;
    constexpr float kGridLineWidth = 0.10f;
    constexpr uint32_t kGridColor = 0x30FFFFFF;
    constexpr uint32_t kMarkupFileMagic = 0x4B4D4452; // RDMK
    constexpr uint32_t kMarkupFileVersion = 1;
    constexpr uint32_t kRoadMarkupSerializableClsid = 0xA6D45122;
    constexpr uint32_t kSelectionHighlightColor = 0xF000A5FF;

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

            if (okZEnable) device->SetRenderState(D3DRENDERSTATE_ZENABLE, zEnable);
            if (okZWrite) device->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, zWrite);
            if (okLighting) device->SetRenderState(D3DRENDERSTATE_LIGHTING, lighting);
            if (okAlphaBlend) device->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, alphaBlend);
            if (okAlphaTest) device->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, alphaTest);
            if (okAlphaFunc) device->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, alphaFunc);
            if (okAlphaRef) device->SetRenderState(D3DRENDERSTATE_ALPHAREF, alphaRef);
            if (okStencilEnable) device->SetRenderState(D3DRENDERSTATE_STENCILENABLE, stencilEnable);
            if (okSrcBlend) device->SetRenderState(D3DRENDERSTATE_SRCBLEND, srcBlend);
            if (okDstBlend) device->SetRenderState(D3DRENDERSTATE_DESTBLEND, dstBlend);
            if (okCullMode) device->SetRenderState(D3DRENDERSTATE_CULLMODE, cullMode);
            if (okFogEnable) device->SetRenderState(D3DRENDERSTATE_FOGENABLE, fogEnable);
            if (okRangeFogEnable) device->SetRenderState(D3DRENDERSTATE_RANGEFOGENABLE, rangeFogEnable);
            if (okZFunc) device->SetRenderState(D3DRENDERSTATE_ZFUNC, zFunc);
            if (okZBias) device->SetRenderState(D3DRENDERSTATE_ZBIAS, zBias);
            if (okTexture0) device->SetTexture(0, texture0);
            if (okTexture1) device->SetTexture(1, texture1);
            if (okTs0ColorOp) device->SetTextureStageState(0, D3DTSS_COLOROP, ts0ColorOp);
            if (okTs0ColorArg1) device->SetTextureStageState(0, D3DTSS_COLORARG1, ts0ColorArg1);
            if (okTs0AlphaOp) device->SetTextureStageState(0, D3DTSS_ALPHAOP, ts0AlphaOp);
            if (okTs0AlphaArg1) device->SetTextureStageState(0, D3DTSS_ALPHAARG1, ts0AlphaArg1);
            if (okTs1ColorOp) device->SetTextureStageState(1, D3DTSS_COLOROP, ts1ColorOp);
            if (okTs1AlphaOp) device->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1AlphaOp);
            if (texture0) texture0->Release();
            if (texture1) texture1->Release();
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

    struct RoadDecalVertex
    {
        float x;
        float y;
        float z;
        DWORD diffuse;
    };

    void BuildStrokeVertices(const RoadMarkupStroke& stroke, std::vector<RoadDecalVertex>& outVerts);
    void DrawVertexBuffer(IDirect3DDevice7* device, const std::vector<RoadDecalVertex>& verts);

    std::vector<RoadDecalVertex> gRoadDecalVertices;
    std::vector<RoadDecalVertex> gRoadDecalActiveVertices;
    std::vector<RoadDecalVertex> gRoadDecalPreviewVertices;
    std::vector<RoadDecalVertex> gRoadDecalGridVertices;
    std::vector<RoadDecalVertex> gRoadDecalSelectionVertices;

    struct StrokeRef
    {
        int layerIndex = -1;
        int strokeIndex = -1;
    };

    constexpr std::array<RoadMarkupProperties, 25> kMarkupProps = {{
        {RoadMarkupType::SolidWhiteLine, RoadMarkupCategory::LaneDivider, "Solid White", "Continuous white lane divider.", 0.75f, 0.0f, 0xE0FFFFAA, false, false},
        {RoadMarkupType::DashedWhiteLine, RoadMarkupCategory::LaneDivider, "Dashed White", "Dashed white lane divider.", 0.75f, 0.0f, 0xE0FFFFAA, true, false},
        {RoadMarkupType::SolidYellowLine, RoadMarkupCategory::LaneDivider, "Solid Yellow", "Continuous yellow centerline.", 0.75f, 0.0f, 0xE0FFD700, false, false},
        {RoadMarkupType::DashedYellowLine, RoadMarkupCategory::LaneDivider, "Dashed Yellow", "Dashed yellow centerline.", 0.75f, 0.0f, 0xE0FFD700, true, false},
        {RoadMarkupType::DoubleSolidYellow, RoadMarkupCategory::LaneDivider, "Double Yellow", "Double solid yellow centerline.", 0.75f, 0.0f, 0xE0FFD700, false, true},
        {RoadMarkupType::SolidWhiteEdgeLine, RoadMarkupCategory::LaneDivider, "Edge Line", "Solid white edge line.", 0.325f, 0.0f, 0xE0FFFFAA, false, true},

        {RoadMarkupType::ArrowStraight, RoadMarkupCategory::DirectionalArrow, "Straight Arrow", "Straight lane arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ArrowLeft, RoadMarkupCategory::DirectionalArrow, "Left Arrow", "Left turn arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ArrowRight, RoadMarkupCategory::DirectionalArrow, "Right Arrow", "Right turn arrow.", 1.20f, 3.00f, 0xE0FFFAA, false, true},
        {RoadMarkupType::ArrowLeftRight, RoadMarkupCategory::DirectionalArrow, "Left/Right Arrow", "Left-right arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ArrowStraightLeft, RoadMarkupCategory::DirectionalArrow, "Straight+Left", "Straight-left arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ArrowStraightRight, RoadMarkupCategory::DirectionalArrow, "Straight+Right", "Straight-right arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ArrowUTurn, RoadMarkupCategory::DirectionalArrow, "U-Turn Arrow", "U-turn arrow.", 1.20f, 3.00f, 0xE0FFFFAA, false, true},

        {RoadMarkupType::ZebraCrosswalk, RoadMarkupCategory::Crossing, "Zebra Crosswalk", "Parallel zebra stripes.", 0.50f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::LadderCrosswalk, RoadMarkupCategory::Crossing, "Ladder Crosswalk", "Zebra with side rails.", 0.50f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ContinentalCrosswalk, RoadMarkupCategory::Crossing, "Continental", "Thicker stripe crossing.", 0.80f, 3.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::StopBar, RoadMarkupCategory::Crossing, "Stop Bar", "Stop line bar.", 0.40f, 0.00f, 0xE0FFFFFF, false, true},

        {RoadMarkupType::YieldTriangle, RoadMarkupCategory::ZoneMarking, "Yield Triangle", "Yield marker.", 0.15f, 1.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::ParkingSpace, RoadMarkupCategory::ZoneMarking, "Parking Space", "Parking outline marker.", 0.15f, 5.00f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::BikeSymbol, RoadMarkupCategory::ZoneMarking, "Bike Symbol", "Bike lane symbol.", 0.20f, 2.50f, 0xE0FFFFAA, false, true},
        {RoadMarkupType::BusLane, RoadMarkupCategory::ZoneMarking, "Bus Lane", "Bus lane marker.", 0.20f, 6.00f, 0xE0FFFFAA, false, true},

        {RoadMarkupType::TextStop, RoadMarkupCategory::TextLabel, "STOP", "Text marker phase 2.", 0.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextSlow, RoadMarkupCategory::TextLabel, "SLOW", "Text marker phase 2.", 0.20f, 3.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextSchool, RoadMarkupCategory::TextLabel, "SCHOOL", "Text marker phase 2.", 0.20f, 4.00f, 0xE0FFFFFF, false, true},
        {RoadMarkupType::TextBusOnly, RoadMarkupCategory::TextLabel, "BUS ONLY", "Text marker phase 2.", 0.20f, 5.00f, 0xE0FFFFFF, false, true},
    }};

    const RoadMarkupProperties& FindProps(RoadMarkupType type)
    {
        for (const auto& props : kMarkupProps) {
            if (props.type == type) {
                return props;
            }
        }
        return kMarkupProps[0];
    }

    DWORD ApplyOpacity(uint32_t color, float opacity)
    {
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        const uint32_t baseA = (color >> 24U) & 0xFFU;
        const uint32_t outA = static_cast<uint32_t>(std::clamp(baseA * opacity, 0.0f, 255.0f));
        return (color & 0x00FFFFFFU) | (outA << 24U);
    }

    cISTETerrain* GetActiveTerrain()
    {
        cISC4AppPtr app;
        cISC4City* city = app ? app->GetCity() : nullptr;
        return city ? city->GetTerrain() : nullptr;
    }

    float SampleTerrainHeight(cISTETerrain* terrain, float x, float z)
    {
        if (!terrain) {
            return 0.0f;
        }

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
    }

    void ConformPointsToTerrain(std::vector<RoadDecalPoint>& points)
    {
        auto* terrain = GetActiveTerrain();
        if (!terrain) {
            return;
        }
        for (auto& point : points) {
            point.y = SampleTerrainHeight(terrain, point.x, point.z) + kDecalTerrainOffset;
        }
    }

    bool GetDirectionXZ(const RoadDecalPoint& a, const RoadDecalPoint& b, float& outTx, float& outTz, float& outLen)
    {
        const float dx = b.x - a.x;
        const float dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len <= kMinLen) {
            return false;
        }
        outTx = dx / len;
        outTz = dz / len;
        outLen = len;
        return true;
    }

    float Distance3(const RoadDecalPoint& a, const RoadDecalPoint& b)
    {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float DistanceXZToSegmentSquared(const RoadDecalPoint& p, const RoadDecalPoint& a, const RoadDecalPoint& b)
    {
        const float abX = b.x - a.x;
        const float abZ = b.z - a.z;
        const float apX = p.x - a.x;
        const float apZ = p.z - a.z;
        const float abLen2 = abX * abX + abZ * abZ;
        if (abLen2 <= 1.0e-6f) {
            return apX * apX + apZ * apZ;
        }
        const float t = std::clamp((apX * abX + apZ * abZ) / abLen2, 0.0f, 1.0f);
        const float qx = a.x + abX * t;
        const float qz = a.z + abZ * t;
        const float dx = p.x - qx;
        const float dz = p.z - qz;
        return dx * dx + dz * dz;
    }

    float DistanceXZToStrokeSquared(const RoadDecalPoint& p, const RoadMarkupStroke& stroke)
    {
        if (stroke.points.empty()) {
            return 1.0e12f;
        }
        if (stroke.points.size() == 1) {
            const float dx = p.x - stroke.points[0].x;
            const float dz = p.z - stroke.points[0].z;
            return dx * dx + dz * dz;
        }

        float best = 1.0e12f;
        for (size_t i = 1; i < stroke.points.size(); ++i) {
            best = (std::min)(best, DistanceXZToSegmentSquared(p, stroke.points[i - 1], stroke.points[i]));
        }
        return best;
    }

    StrokeRef GetSelectedStrokeRef()
    {
        if (gSelectedLayerIndex < 0 || gSelectedLayerIndex >= static_cast<int>(gRoadMarkupLayers.size())) {
            return {};
        }
        const auto& layer = gRoadMarkupLayers[static_cast<size_t>(gSelectedLayerIndex)];
        if (gSelectedStrokeIndex < 0 || gSelectedStrokeIndex >= static_cast<int>(layer.strokes.size())) {
            return {};
        }
        return {gSelectedLayerIndex, gSelectedStrokeIndex};
    }

    bool IsSelectionValid()
    {
        const StrokeRef ref = GetSelectedStrokeRef();
        return ref.layerIndex >= 0 && ref.strokeIndex >= 0;
    }

    RoadMarkupStroke* GetStrokeByRef(const StrokeRef& ref)
    {
        if (ref.layerIndex < 0 || ref.strokeIndex < 0) {
            return nullptr;
        }
        auto& layer = gRoadMarkupLayers[static_cast<size_t>(ref.layerIndex)];
        if (ref.strokeIndex >= static_cast<int>(layer.strokes.size())) {
            return nullptr;
        }
        return &layer.strokes[static_cast<size_t>(ref.strokeIndex)];
    }

    class FileOStream final : public cIGZOStream
    {
    public:
        explicit FileOStream(std::ofstream& out)
            : out_(out)
        {
        }

        bool QueryInterface(uint32_t riid, void** ppvObj) override
        {
            if (!ppvObj) {
                return false;
            }
            if (riid == GZIID_cIGZUnknown) {
                *ppvObj = static_cast<cIGZUnknown*>(this);
                AddRef();
                return true;
            }
            *ppvObj = nullptr;
            return false;
        }

        uint32_t AddRef() override { return ++refCount_; }
        uint32_t Release() override
        {
            const uint32_t n = --refCount_;
            if (n == 0) {
                delete this;
            }
            return n;
        }

        void Flush() override { out_.flush(); }
        bool SetSint8(int8_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetUint8(uint8_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetSint16(int16_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetUint16(uint16_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetSint32(int32_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetUint32(uint32_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetSint64(int64_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetUint64(uint64_t v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetFloat32(float v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetFloat64(double v) override { return WriteRaw_(&v, sizeof(v)); }
        bool SetRZCharStr(const char*) override { error_ = 1; return false; }
        bool SetGZStr(cIGZString const&) override { error_ = 1; return false; }
        bool SetGZSerializable(cIGZSerializable const& sData) override
        {
            // cIGZSerializable::Write is non-const in the SDK interface.
            return const_cast<cIGZSerializable&>(sData).Write(*this);
        }
        bool SetVoid(void const* pData, uint32_t size) override
        {
            return WriteRaw_(pData, size);
        }
        int32_t GetError() override { return error_; }
        int32_t SetUserData(cIGZVariant* pData) override
        {
            userData_ = reinterpret_cast<intptr_t>(pData);
            return 0;
        }
        int32_t GetUserData() override { return static_cast<int32_t>(userData_); }

    private:
        bool WriteRaw_(void const* pData, uint32_t size)
        {
            out_.write(reinterpret_cast<const char*>(pData), size);
            if (!out_.good()) {
                error_ = 1;
                return false;
            }
            return true;
        }

        std::atomic<uint32_t> refCount_{1};
        std::ofstream& out_;
        int32_t error_ = 0;
        intptr_t userData_ = 0;
    };

    class FileIStream final : public cIGZIStream
    {
    public:
        explicit FileIStream(std::ifstream& in)
            : in_(in)
        {
        }

        bool QueryInterface(uint32_t riid, void** ppvObj) override
        {
            if (!ppvObj) {
                return false;
            }
            if (riid == GZIID_cIGZUnknown) {
                *ppvObj = static_cast<cIGZUnknown*>(this);
                AddRef();
                return true;
            }
            *ppvObj = nullptr;
            return false;
        }

        uint32_t AddRef() override { return ++refCount_; }
        uint32_t Release() override
        {
            const uint32_t n = --refCount_;
            if (n == 0) {
                delete this;
            }
            return n;
        }

        bool Skip(uint32_t bytes) override
        {
            in_.seekg(bytes, std::ios::cur);
            if (!in_.good()) {
                error_ = 1;
                return false;
            }
            return true;
        }

        bool GetSint8(int8_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetUint8(uint8_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetSint16(int16_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetUint16(uint16_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetSint32(int32_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetUint32(uint32_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetSint64(int64_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetUint64(uint64_t& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetFloat32(float& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetFloat64(double& v) override { return ReadRaw_(&v, sizeof(v)); }
        bool GetRZCharStr(char*, uint32_t) override { error_ = 1; return false; }
        bool GetGZStr(cIGZString&) override { error_ = 1; return false; }
        bool GetGZSerializable(cIGZSerializable& sData) override
        {
            return sData.Read(*this);
        }
        bool GetVoid(void* pDataOut, uint32_t size) override
        {
            return ReadRaw_(pDataOut, size);
        }
        int32_t GetError() override { return error_; }
        int32_t SetUserData(cIGZVariant* pData) override
        {
            userData_ = reinterpret_cast<intptr_t>(pData);
            return 0;
        }
        int32_t GetUserData() override { return static_cast<int32_t>(userData_); }

    private:
        bool ReadRaw_(void* pDataOut, uint32_t size)
        {
            in_.read(reinterpret_cast<char*>(pDataOut), size);
            if (!in_.good()) {
                error_ = 1;
                return false;
            }
            return true;
        }

        std::atomic<uint32_t> refCount_{1};
        std::ifstream& in_;
        int32_t error_ = 0;
        intptr_t userData_ = 0;
    };

    class RoadMarkupSerializable final : public cIGZSerializable
    {
    public:
        bool QueryInterface(uint32_t riid, void** ppvObj) override
        {
            if (!ppvObj) {
                return false;
            }
            if (riid == GZIID_cIGZUnknown || riid == GZIID_cIGZSerializable) {
                *ppvObj = static_cast<cIGZSerializable*>(this);
                AddRef();
                return true;
            }
            *ppvObj = nullptr;
            return false;
        }

        uint32_t AddRef() override { return ++refCount_; }
        uint32_t Release() override
        {
            const uint32_t n = --refCount_;
            if (n == 0) {
                delete this;
            }
            return n;
        }

        bool Write(cIGZOStream& stream) override;
        bool Read(cIGZIStream& stream) override;
        uint32_t GetGZCLSID() override { return kRoadMarkupSerializableClsid; }

    private:
        std::atomic<uint32_t> refCount_{1};
    };

    RoadDecalPoint LerpByT(const RoadDecalPoint& a, const RoadDecalPoint& b, float ta, float tb, float t)
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
                                              float u)
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

    RoadDecalPoint ClampPointToSegmentBounds(const RoadDecalPoint& p, const RoadDecalPoint& a, const RoadDecalPoint& b)
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

    void EmitTriangle(const RoadDecalPoint& a, const RoadDecalPoint& b, const RoadDecalPoint& c, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        outVerts.push_back({a.x, a.y, a.z, color});
        outVerts.push_back({b.x, b.y, b.z, color});
        outVerts.push_back({c.x, c.y, c.z, color});
    }

    void EmitQuad(const RoadDecalPoint& a, const RoadDecalPoint& b, const RoadDecalPoint& c, const RoadDecalPoint& d, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        EmitTriangle(a, b, c, color, outVerts);
        EmitTriangle(a, c, d, color, outVerts);
    }

    void EmitThickSegmentNoConform(const RoadDecalPoint& a,
                                   const RoadDecalPoint& b,
                                   float width,
                                   DWORD color,
                                   std::vector<RoadDecalVertex>& outVerts)
    {
        float tx = 0.0f;
        float tz = 0.0f;
        float len = 0.0f;
        if (!GetDirectionXZ(a, b, tx, tz, len) || width <= 0.0f) {
            return;
        }
        const float halfWidth = width * 0.5f;
        const float nx = -tz;
        const float nz = tx;
        const RoadDecalPoint aL{a.x - nx * halfWidth, a.y, a.z - nz * halfWidth, false};
        const RoadDecalPoint aR{a.x + nx * halfWidth, a.y, a.z + nz * halfWidth, false};
        const RoadDecalPoint bL{b.x - nx * halfWidth, b.y, b.z - nz * halfWidth, false};
        const RoadDecalPoint bR{b.x + nx * halfWidth, b.y, b.z + nz * halfWidth, false};
        EmitQuad(aL, bL, bR, aR, color, outVerts);
    }

    RoadDecalPoint LocalPoint(const RoadDecalPoint& center,
                              float rightX,
                              float rightZ,
                              float fwdX,
                              float fwdZ,
                              float lateral,
                              float forward)
    {
        return {
            center.x + rightX * lateral + fwdX * forward,
            center.y,
            center.z + rightZ * lateral + fwdZ * forward,
            false
        };
    }

    void BuildLine(const std::vector<RoadDecalPoint>& points,
                   float width,
                   DWORD color,
                   bool dashed,
                   float dashLength,
                   float gapLength,
                   std::vector<RoadDecalVertex>& outVerts)
    {
        if (points.size() < 2 || width <= 0.0f) {
            return;
        }

        std::vector<RoadDecalPoint> path;
        BuildSmoothedPolyline(points, path);
        if (path.empty()) {
            path = points;
        }
        ConformPointsToTerrain(path);

        dashLength = std::max(0.05f, dashLength);
        gapLength = std::max(0.0f, gapLength);
        const float cycleLength = dashLength + gapLength;
        float cyclePos = 0.0f;
        const float halfWidth = width * 0.5f;

        for (size_t i = 0; i + 1 < path.size(); ++i) {
            const auto& p0 = path[i];
            const auto& p1 = path[i + 1];
            float tx = 0.0f;
            float tz = 0.0f;
            float segLen = 0.0f;
            if (!GetDirectionXZ(p0, p1, tx, tz, segLen)) {
                continue;
            }
            const float nx = -tz;
            const float nz = tx;

            auto emitSlice = [&](float t0, float t1) {
                const RoadDecalPoint a{p0.x + (p1.x - p0.x) * t0, p0.y + (p1.y - p0.y) * t0, p0.z + (p1.z - p0.z) * t0, false};
                const RoadDecalPoint b{p0.x + (p1.x - p0.x) * t1, p0.y + (p1.y - p0.y) * t1, p0.z + (p1.z - p0.z) * t1, false};
                const RoadDecalPoint aL{a.x - nx * halfWidth, a.y, a.z - nz * halfWidth, false};
                const RoadDecalPoint aR{a.x + nx * halfWidth, a.y, a.z + nz * halfWidth, false};
                const RoadDecalPoint bL{b.x - nx * halfWidth, b.y, b.z - nz * halfWidth, false};
                const RoadDecalPoint bR{b.x + nx * halfWidth, b.y, b.z + nz * halfWidth, false};
                EmitQuad(aL, bL, bR, aR, color, outVerts);
            };

            if (!dashed || cycleLength <= 0.0f) {
                emitSlice(0.0f, 1.0f);
                continue;
            }

            float segPos = 0.0f;
            while (segPos < segLen) {
                const float boundary = (cyclePos < dashLength) ? dashLength : cycleLength;
                float step = boundary - cyclePos;
                if (step <= 0.0f) {
                    cyclePos = 0.0f;
                    continue;
                }
                step = (std::min)(step, segLen - segPos);

                if (cyclePos < dashLength) {
                    emitSlice(segPos / segLen, (segPos + step) / segLen);
                }
                segPos += step;
                cyclePos += step;
                if (cyclePos >= cycleLength - 1.0e-4f) {
                    cyclePos = 0.0f;
                }
            }
        }
    }

    void BuildStraightArrow(const RoadMarkupStroke& stroke, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        if (stroke.points.empty()) {
            return;
        }
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;
        const float width = std::max(0.2f, stroke.width);
        const float length = std::max(0.8f, stroke.length);

        auto p0 = LocalPoint(center, rx, rz, fx, fz, -0.15f * width, -0.50f * length);
        auto p1 = LocalPoint(center, rx, rz, fx, fz, -0.15f * width, 0.10f * length);
        auto p2 = LocalPoint(center, rx, rz, fx, fz, +0.15f * width, 0.10f * length);
        auto p3 = LocalPoint(center, rx, rz, fx, fz, +0.15f * width, -0.50f * length);
        auto tip = LocalPoint(center, rx, rz, fx, fz, 0.00f, 0.50f * length);
        auto hl = LocalPoint(center, rx, rz, fx, fz, -0.50f * width, 0.10f * length);
        auto hr = LocalPoint(center, rx, rz, fx, fz, +0.50f * width, 0.10f * length);

        std::vector<RoadDecalPoint> shape = {p0, p1, p2, p3, tip, hl, hr};
        ConformPointsToTerrain(shape);
        EmitQuad(shape[0], shape[1], shape[2], shape[3], color, outVerts);
        EmitTriangle(shape[5], shape[4], shape[6], color, outVerts);
    }

    void BuildTurnArrow(const RoadMarkupStroke& stroke, bool left, bool withStraight, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        if (withStraight) {
            BuildStraightArrow(stroke, color, outVerts);
        }
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;
        const float sign = left ? -1.0f : 1.0f;

        std::vector<RoadDecalPoint> path = {
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.35f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.05f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, sign * 0.35f * stroke.length, 0.25f * stroke.length),
        };
        BuildLine(path, std::max(0.08f, stroke.width * 0.30f), color, false, 0.0f, 0.0f, outVerts);

        RoadMarkupStroke head = stroke;
        head.points = {path.back()};
        head.rotation += left ? -1.57f : 1.57f;
        head.length = std::max(0.5f, stroke.length * 0.35f);
        head.width = std::max(0.4f, stroke.width * 0.90f);
        BuildStraightArrow(head, color, outVerts);
    }

    void BuildUTurnArrow(const RoadMarkupStroke& stroke, DWORD color, std::vector<RoadDecalVertex>& outVerts)
    {
        const RoadDecalPoint center = stroke.points.front();
        const float fx = std::cos(stroke.rotation);
        const float fz = std::sin(stroke.rotation);
        const float rx = -fz;
        const float rz = fx;

        std::vector<RoadDecalPoint> path = {
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.45f * stroke.length),
            LocalPoint(center, rx, rz, fx, fz, 0.0f, -0.10f * stroke.length),
        };
        const float radius = std::max(0.4f, stroke.length * 0.28f);
        for (int i = 0; i <= 8; ++i) {
            const float t = static_cast<float>(i) / 8.0f;
            const float ang = -0.2f * 3.1415926f + (1.35f * 3.1415926f) * t;
            path.push_back(LocalPoint(center, rx, rz, fx, fz,
                                      -0.18f * stroke.length + std::cos(ang) * radius,
                                      +0.08f * stroke.length + std::sin(ang) * radius));
        }
        BuildLine(path, std::max(0.08f, stroke.width * 0.30f), color, false, 0.0f, 0.0f, outVerts);

        RoadMarkupStroke head = stroke;
        head.points = {path.back()};
        head.rotation += 3.1415926f;
        head.length = std::max(0.5f, stroke.length * 0.35f);
        head.width = std::max(0.4f, stroke.width * 0.90f);
        BuildStraightArrow(head, color, outVerts);
    }

    void BuildCrosswalk(const RoadMarkupStroke& stroke,
                        float stripe,
                        float gap,
                        bool ladderRails,
                        DWORD color,
                        std::vector<RoadDecalVertex>& outVerts)
    {
        const auto& start = stroke.points[0];
        const auto& end = stroke.points[1];
        float dragTx = 0.0f;
        float dragTz = 0.0f;
        float length = 0.0f;
        if (!GetDirectionXZ(start, end, dragTx, dragTz, length)) {
            return;
        }
        // Treat user drag length as curb-to-curb span.
        // Stripe orientation follows stroke.rotation (auto-align writes drag direction;
        // manual rotation works when auto-align is off).
        float tx = std::cos(stroke.rotation);
        float tz = std::sin(stroke.rotation);
        if (std::fabs(tx) < 0.0001f && std::fabs(tz) < 0.0001f) {
            tx = dragTx;
            tz = dragTz;
        }
        const float perpX = -tz;
        const float perpZ = tx;
        // Keep dimensions decoupled:
        // - drag length controls curb-to-curb span
        // - stroke.length controls crossing depth (visual width along road)
        const float span = length;
        const float depth = std::max(stripe, stroke.length);
        const int stripes = std::max(1, static_cast<int>(std::floor((depth + gap) / (stripe + gap))));
        const float usedDepth = static_cast<float>(stripes) * stripe + static_cast<float>(stripes - 1) * gap;
        const float firstOffset = -0.5f * usedDepth + stripe * 0.5f;
        const RoadDecalPoint center{
            (start.x + end.x) * 0.5f,
            (start.y + end.y) * 0.5f,
            (start.z + end.z) * 0.5f,
            false
        };

        for (int i = 0; i < stripes; ++i) {
            const float offset = firstOffset + static_cast<float>(i) * (stripe + gap);
            const RoadDecalPoint a{
                center.x + perpX * offset - tx * (span * 0.5f),
                center.y,
                center.z + perpZ * offset - tz * (span * 0.5f),
                false
            };
            const RoadDecalPoint b{
                center.x + perpX * offset + tx * (span * 0.5f),
                center.y,
                center.z + perpZ * offset + tz * (span * 0.5f),
                false
            };
            BuildLine({a, b}, stripe, color, false, 0.0f, 0.0f, outVerts);
        }

        if (ladderRails) {
            const float railLeft = -0.5f * depth;
            const float railRight = 0.5f * depth;
            const RoadDecalPoint l0{
                center.x + perpX * railLeft - tx * (span * 0.5f),
                center.y,
                center.z + perpZ * railLeft - tz * (span * 0.5f),
                false
            };
            const RoadDecalPoint l1{
                center.x + perpX * railLeft + tx * (span * 0.5f),
                center.y,
                center.z + perpZ * railLeft + tz * (span * 0.5f),
                false
            };
            const RoadDecalPoint r0{
                center.x + perpX * railRight - tx * (span * 0.5f),
                center.y,
                center.z + perpZ * railRight - tz * (span * 0.5f),
                false
            };
            const RoadDecalPoint r1{
                center.x + perpX * railRight + tx * (span * 0.5f),
                center.y,
                center.z + perpZ * railRight + tz * (span * 0.5f),
                false
            };
            BuildLine({l0, l1}, stripe * 0.5f, color, false, 0.0f, 0.0f, outVerts);
            BuildLine({r0, r1}, stripe * 0.5f, color, false, 0.0f, 0.0f, outVerts);
        }
    }
}

const RoadMarkupProperties& GetRoadMarkupProperties(RoadMarkupType type)
{
    return FindProps(type);
}

RoadMarkupCategory GetMarkupCategory(RoadMarkupType type)
{
    return FindProps(type).category;
}

const std::vector<RoadMarkupType>& GetRoadMarkupTypesForCategory(RoadMarkupCategory category)
{
    static const std::vector<RoadMarkupType> laneTypes = {
        RoadMarkupType::SolidWhiteLine,
        RoadMarkupType::DashedWhiteLine,
        RoadMarkupType::SolidYellowLine,
        RoadMarkupType::DashedYellowLine,
        RoadMarkupType::DoubleSolidYellow,
        RoadMarkupType::SolidWhiteEdgeLine,
    };
    static const std::vector<RoadMarkupType> arrowTypes = {
        RoadMarkupType::ArrowStraight,
        RoadMarkupType::ArrowLeft,
        RoadMarkupType::ArrowRight,
        RoadMarkupType::ArrowLeftRight,
        RoadMarkupType::ArrowStraightLeft,
        RoadMarkupType::ArrowStraightRight,
        RoadMarkupType::ArrowUTurn,
    };
    static const std::vector<RoadMarkupType> crossingTypes = {
        RoadMarkupType::ZebraCrosswalk,
        RoadMarkupType::LadderCrosswalk,
        RoadMarkupType::ContinentalCrosswalk,
        RoadMarkupType::StopBar,
    };
    static const std::vector<RoadMarkupType> zoneTypes = {
        RoadMarkupType::YieldTriangle,
        RoadMarkupType::ParkingSpace,
        RoadMarkupType::BikeSymbol,
        RoadMarkupType::BusLane,
    };
    static const std::vector<RoadMarkupType> textTypes = {
        RoadMarkupType::TextStop,
        RoadMarkupType::TextSlow,
        RoadMarkupType::TextSchool,
        RoadMarkupType::TextBusOnly,
    };

    switch (category) {
    case RoadMarkupCategory::LaneDivider:
        return laneTypes;
    case RoadMarkupCategory::DirectionalArrow:
        return arrowTypes;
    case RoadMarkupCategory::Crossing:
        return crossingTypes;
    case RoadMarkupCategory::ZoneMarking:
        return zoneTypes;
    case RoadMarkupCategory::TextLabel:
    default:
        return textTypes;
    }
}

void EnsureDefaultRoadMarkupLayer()
{
    if (!gRoadMarkupLayers.empty()) {
        gActiveLayerIndex = std::clamp(gActiveLayerIndex, 0, static_cast<int>(gRoadMarkupLayers.size()) - 1);
        return;
    }
    gRoadMarkupLayers.push_back({1, "Layer 1", {}, true, false, 0});
    gActiveLayerIndex = 0;
}

RoadMarkupLayer* GetActiveRoadMarkupLayer()
{
    EnsureDefaultRoadMarkupLayer();
    if (gActiveLayerIndex < 0 || gActiveLayerIndex >= static_cast<int>(gRoadMarkupLayers.size())) {
        return nullptr;
    }
    return &gRoadMarkupLayers[static_cast<size_t>(gActiveLayerIndex)];
}

bool AddRoadMarkupLayer(const std::string& name)
{
    EnsureDefaultRoadMarkupLayer();
    if (gRoadMarkupLayers.size() >= 10) {
        return false;
    }
    uint32_t id = 1;
    for (const auto& layer : gRoadMarkupLayers) {
        id = (std::max)(id, layer.id + 1);
    }
    gRoadMarkupLayers.push_back({id, name.empty() ? "Layer" : name, {}, true, false, static_cast<int>(gRoadMarkupLayers.size())});
    gActiveLayerIndex = static_cast<int>(gRoadMarkupLayers.size() - 1);
    return true;
}

void DeleteActiveRoadMarkupLayer()
{
    EnsureDefaultRoadMarkupLayer();
    if (gRoadMarkupLayers.size() <= 1) {
        gRoadMarkupLayers[0].strokes.clear();
        ClearRoadMarkupSelection();
        return;
    }
    if (gSelectedLayerIndex == gActiveLayerIndex) {
        ClearRoadMarkupSelection();
    } else if (gSelectedLayerIndex > gActiveLayerIndex) {
        --gSelectedLayerIndex;
    }
    gRoadMarkupLayers.erase(gRoadMarkupLayers.begin() + gActiveLayerIndex);
    gActiveLayerIndex = std::clamp(gActiveLayerIndex, 0, static_cast<int>(gRoadMarkupLayers.size()) - 1);
}

bool AddRoadMarkupStrokeToActiveLayer(const RoadMarkupStroke& stroke)
{
    auto* layer = GetActiveRoadMarkupLayer();
    if (!layer || layer->locked || layer->strokes.size() >= 100) {
        return false;
    }
    auto stored = stroke;
    stored.layerId = layer->id;
    layer->strokes.push_back(stored);
    return true;
}

void UndoLastRoadMarkupStroke()
{
    auto* layer = GetActiveRoadMarkupLayer();
    if (!layer || layer->strokes.empty()) {
        return;
    }
    if (gSelectedLayerIndex == gActiveLayerIndex &&
        gSelectedStrokeIndex == static_cast<int>(layer->strokes.size()) - 1) {
        ClearRoadMarkupSelection();
    }
    layer->strokes.pop_back();
}

void ClearAllRoadMarkupStrokes()
{
    EnsureDefaultRoadMarkupLayer();
    for (auto& layer : gRoadMarkupLayers) {
        layer.strokes.clear();
    }
    ClearRoadMarkupSelection();
}

size_t GetTotalRoadMarkupStrokeCount()
{
    size_t total = 0;
    for (const auto& layer : gRoadMarkupLayers) {
        total += layer.strokes.size();
    }
    return total;
}

bool SelectRoadMarkupStrokeAtPoint(const RoadDecalPoint& worldPoint, float maxDistanceMeters)
{
    EnsureDefaultRoadMarkupLayer();
    const float maxDistance2 = (std::max)(0.1f, maxDistanceMeters) * (std::max)(0.1f, maxDistanceMeters);
    float bestDistance2 = maxDistance2;
    StrokeRef best{};

    for (size_t layerIndex = 0; layerIndex < gRoadMarkupLayers.size(); ++layerIndex) {
        const auto& layer = gRoadMarkupLayers[layerIndex];
        if (!layer.visible || layer.locked) {
            continue;
        }
        for (size_t strokeIndex = 0; strokeIndex < layer.strokes.size(); ++strokeIndex) {
            const auto& stroke = layer.strokes[strokeIndex];
            if (!stroke.visible) {
                continue;
            }
            const float distance2 = DistanceXZToStrokeSquared(worldPoint, stroke);
            if (distance2 <= bestDistance2) {
                bestDistance2 = distance2;
                best.layerIndex = static_cast<int>(layerIndex);
                best.strokeIndex = static_cast<int>(strokeIndex);
            }
        }
    }

    if (best.layerIndex < 0 || best.strokeIndex < 0) {
        return false;
    }

    gSelectedLayerIndex = best.layerIndex;
    gSelectedStrokeIndex = best.strokeIndex;
    gActiveLayerIndex = best.layerIndex;
    SetRoadDecalSelectedStroke(GetSelectedRoadMarkupStrokeConst());
    return true;
}

void ClearRoadMarkupSelection()
{
    gSelectedLayerIndex = -1;
    gSelectedStrokeIndex = -1;
    SetRoadDecalSelectedStroke(nullptr);
}

bool HasRoadMarkupSelection()
{
    return IsSelectionValid();
}

RoadMarkupStroke* GetSelectedRoadMarkupStroke()
{
    return GetStrokeByRef(GetSelectedStrokeRef());
}

const RoadMarkupStroke* GetSelectedRoadMarkupStrokeConst()
{
    const StrokeRef ref = GetSelectedStrokeRef();
    if (ref.layerIndex < 0 || ref.strokeIndex < 0) {
        return nullptr;
    }
    return &gRoadMarkupLayers[static_cast<size_t>(ref.layerIndex)].strokes[static_cast<size_t>(ref.strokeIndex)];
}

bool DeleteSelectedRoadMarkupStroke()
{
    const StrokeRef ref = GetSelectedStrokeRef();
    if (ref.layerIndex < 0 || ref.strokeIndex < 0) {
        return false;
    }
    auto& layer = gRoadMarkupLayers[static_cast<size_t>(ref.layerIndex)];
    if (layer.locked || ref.strokeIndex >= static_cast<int>(layer.strokes.size())) {
        return false;
    }
    layer.strokes.erase(layer.strokes.begin() + ref.strokeIndex);
    ClearRoadMarkupSelection();
    RebuildRoadDecalGeometry();
    return true;
}

bool MoveSelectedRoadMarkupStroke(float deltaX, float deltaZ)
{
    auto* stroke = GetSelectedRoadMarkupStroke();
    if (!stroke || stroke->points.empty()) {
        return false;
    }
    for (auto& p : stroke->points) {
        p.x += deltaX;
        p.z += deltaZ;
    }
    ConformPointsToTerrain(stroke->points);
    SetRoadDecalSelectedStroke(stroke);
    RebuildRoadDecalGeometry();
    return true;
}

bool RotateSelectedRoadMarkupStroke(float deltaRadians)
{
    auto* stroke = GetSelectedRoadMarkupStroke();
    if (!stroke || stroke->points.empty()) {
        return false;
    }

    float centerX = 0.0f;
    float centerZ = 0.0f;
    for (const auto& p : stroke->points) {
        centerX += p.x;
        centerZ += p.z;
    }
    const float inv = 1.0f / static_cast<float>(stroke->points.size());
    centerX *= inv;
    centerZ *= inv;

    const float s = std::sin(deltaRadians);
    const float c = std::cos(deltaRadians);
    for (auto& p : stroke->points) {
        const float x = p.x - centerX;
        const float z = p.z - centerZ;
        p.x = centerX + (x * c - z * s);
        p.z = centerZ + (x * s + z * c);
    }
    stroke->rotation += deltaRadians;
    ConformPointsToTerrain(stroke->points);
    SetRoadDecalSelectedStroke(stroke);
    RebuildRoadDecalGeometry();
    return true;
}

void RebuildRoadDecalGeometry()
{
    EnsureDefaultRoadMarkupLayer();
    gRoadDecalVertices.clear();
    std::vector<const RoadMarkupLayer*> orderedLayers;
    orderedLayers.reserve(gRoadMarkupLayers.size());
    for (const auto& layer : gRoadMarkupLayers) {
        orderedLayers.push_back(&layer);
    }
    std::stable_sort(orderedLayers.begin(), orderedLayers.end(),
                     [](const RoadMarkupLayer* a, const RoadMarkupLayer* b) {
                         return a->renderOrder < b->renderOrder;
                     });

    for (const auto* layer : orderedLayers) {
        if (!layer) {
            continue;
        }
        if (!layer->visible) {
            continue;
        }
        for (const auto& stroke : layer->strokes) {
            BuildStrokeVertices(stroke, gRoadDecalVertices);
        }
    }
    SetRoadDecalSelectedStroke(GetSelectedRoadMarkupStrokeConst());
}

void DrawRoadDecals()
{
    if (gRoadDecalVertices.empty() &&
        gRoadDecalActiveVertices.empty() &&
        gRoadDecalPreviewVertices.empty() &&
        gRoadDecalGridVertices.empty() &&
        gRoadDecalSelectionVertices.empty()) {
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
        DrawVertexBuffer(device, gRoadDecalSelectionVertices);
        DrawVertexBuffer(device, gRoadDecalActiveVertices);
        DrawVertexBuffer(device, gRoadDecalPreviewVertices);
        DrawVertexBuffer(device, gRoadDecalGridVertices);
    }

    device->Release();
}

void SetRoadDecalActiveStroke(const RoadMarkupStroke* stroke)
{
    gRoadDecalActiveVertices.clear();
    if (stroke) {
        BuildStrokeVertices(*stroke, gRoadDecalActiveVertices);
    }
}

void SetRoadDecalPreviewSegment(bool enabled, const RoadMarkupStroke& stroke)
{
    gRoadDecalPreviewVertices.clear();
    if (enabled) {
        BuildStrokeVertices(stroke, gRoadDecalPreviewVertices);
    }
}

void SetRoadDecalSelectedStroke(const RoadMarkupStroke* stroke)
{
    gRoadDecalSelectionVertices.clear();
    if (!stroke) {
        return;
    }

    RoadMarkupStroke highlight = *stroke;
    highlight.color = kSelectionHighlightColor;
    highlight.opacity = 1.0f;
    if (GetMarkupCategory(highlight.type) == RoadMarkupCategory::LaneDivider) {
        highlight.width += 0.20f;
    }
    BuildStrokeVertices(highlight, gRoadDecalSelectionVertices);
}

void SetRoadDecalGridPreview(bool enabled, const RoadDecalPoint& centerPoint)
{
    gRoadDecalGridVertices.clear();
    if (!enabled) {
        return;
    }

    const float tileX = std::floor(centerPoint.x / kTileSize) * kTileSize;
    const float tileZ = std::floor(centerPoint.z / kTileSize) * kTileSize;

    const float minX = tileX - kTileSize;
    const float maxX = tileX + (2.0f * kTileSize);
    const float minZ = tileZ - kTileSize;
    const float maxZ = tileZ + (2.0f * kTileSize);

    auto* terrain = GetActiveTerrain();
    if (!terrain) {
        return;
    }

    const int xCount = static_cast<int>(std::round((maxX - minX) / kMinorGridSize)) + 1;
    const int zCount = static_cast<int>(std::round((maxZ - minZ) / kMinorGridSize)) + 1;
    if (xCount < 2 || zCount < 2) {
        return;
    }

    std::vector<RoadDecalPoint> gridPoints(static_cast<size_t>(xCount * zCount));
    auto at = [&](int xi, int zi) -> RoadDecalPoint& {
        return gridPoints[static_cast<size_t>(zi * xCount + xi)];
    };

    for (int zi = 0; zi < zCount; ++zi) {
        const float z = minZ + static_cast<float>(zi) * kMinorGridSize;
        for (int xi = 0; xi < xCount; ++xi) {
            const float x = minX + static_cast<float>(xi) * kMinorGridSize;
            auto& p = at(xi, zi);
            p.x = x;
            p.z = z;
            p.y = SampleTerrainHeight(terrain, x, z) + kDecalTerrainOffset;
            p.hardCorner = false;
        }
    }

    gRoadDecalGridVertices.reserve(static_cast<size_t>((xCount - 1) * zCount + (zCount - 1) * xCount) * 6);

    for (int zi = 0; zi < zCount; ++zi) {
        for (int xi = 0; xi + 1 < xCount; ++xi) {
            EmitThickSegmentNoConform(at(xi, zi), at(xi + 1, zi), kGridLineWidth, kGridColor, gRoadDecalGridVertices);
        }
    }
    for (int xi = 0; xi < xCount; ++xi) {
        for (int zi = 0; zi + 1 < zCount; ++zi) {
            EmitThickSegmentNoConform(at(xi, zi), at(xi, zi + 1), kGridLineWidth, kGridColor, gRoadDecalGridVertices);
        }
    }
}

bool SaveMarkupsToFile(const char* filepath)
{
    if (!filepath || !filepath[0]) {
        return false;
    }
    EnsureDefaultRoadMarkupLayer();
    std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    FileOStream stream(out);
    RoadMarkupSerializable serializable;
    return stream.SetGZSerializable(serializable) && stream.GetError() == 0;
}

bool LoadMarkupsFromFile(const char* filepath)
{
    if (!filepath || !filepath[0]) {
        return false;
    }
    std::ifstream in(filepath, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    FileIStream stream(in);
    RoadMarkupSerializable serializable;
    if (!stream.GetGZSerializable(serializable) || stream.GetError() != 0) {
        return false;
    }

    EnsureDefaultRoadMarkupLayer();
    gActiveLayerIndex = std::clamp(gActiveLayerIndex, 0, static_cast<int>(gRoadMarkupLayers.size()) - 1);
    if (!IsSelectionValid()) {
        ClearRoadMarkupSelection();
    }
    RebuildRoadDecalGeometry();
    return true;
}

namespace
{
    bool RoadMarkupSerializable::Write(cIGZOStream& stream)
    {
        if (stream.GetError() != 0) {
            return false;
        }

        if (!stream.SetUint32(kMarkupFileMagic) ||
            !stream.SetUint32(kMarkupFileVersion)) {
            return false;
        }

        const uint32_t layerCount = static_cast<uint32_t>(gRoadMarkupLayers.size());
        if (!stream.SetUint32(layerCount) ||
            !stream.SetSint32(gActiveLayerIndex) ||
            !stream.SetSint32(gSelectedLayerIndex) ||
            !stream.SetSint32(gSelectedStrokeIndex)) {
            return false;
        }

        for (const auto& layer : gRoadMarkupLayers) {
            const uint32_t nameLen = static_cast<uint32_t>(layer.name.size());
            if (!stream.SetUint32(layer.id) ||
                !stream.SetUint32(nameLen) ||
                !stream.SetUint8(layer.visible ? 1 : 0) ||
                !stream.SetUint8(layer.locked ? 1 : 0) ||
                !stream.SetSint32(layer.renderOrder) ||
                !stream.SetUint32(static_cast<uint32_t>(layer.strokes.size()))) {
                return false;
            }

            if (nameLen > 0 && !stream.SetVoid(layer.name.data(), nameLen)) {
                return false;
            }

            for (const auto& stroke : layer.strokes) {
                if (!stream.SetUint32(static_cast<uint32_t>(stroke.type)) ||
                    !stream.SetUint32(static_cast<uint32_t>(stroke.points.size())) ||
                    !stream.SetFloat32(stroke.width) ||
                    !stream.SetFloat32(stroke.length) ||
                    !stream.SetFloat32(stroke.rotation) ||
                    !stream.SetUint8(stroke.dashed ? 1 : 0) ||
                    !stream.SetFloat32(stroke.dashLength) ||
                    !stream.SetFloat32(stroke.gapLength) ||
                    !stream.SetUint32(stroke.color) ||
                    !stream.SetFloat32(stroke.opacity) ||
                    !stream.SetUint8(stroke.visible ? 1 : 0) ||
                    !stream.SetUint32(stroke.layerId)) {
                    return false;
                }

                for (const auto& point : stroke.points) {
                    if (!stream.SetFloat32(point.x) ||
                        !stream.SetFloat32(point.y) ||
                        !stream.SetFloat32(point.z) ||
                        !stream.SetUint8(point.hardCorner ? 1 : 0)) {
                        return false;
                    }
                }
            }
        }

        return stream.GetError() == 0;
    }

    bool RoadMarkupSerializable::Read(cIGZIStream& stream)
    {
        if (stream.GetError() != 0) {
            return false;
        }

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t layerCount = 0;
        int32_t activeLayerIndex = 0;
        int32_t selectedLayerIndex = -1;
        int32_t selectedStrokeIndex = -1;
        if (!stream.GetUint32(magic) ||
            !stream.GetUint32(version) ||
            !stream.GetUint32(layerCount) ||
            !stream.GetSint32(activeLayerIndex) ||
            !stream.GetSint32(selectedLayerIndex) ||
            !stream.GetSint32(selectedStrokeIndex) ||
            magic != kMarkupFileMagic ||
            version != kMarkupFileVersion) {
            return false;
        }

        std::vector<RoadMarkupLayer> layers;
        layers.reserve(layerCount);
        for (uint32_t i = 0; i < layerCount; ++i) {
            RoadMarkupLayer layer{};
            uint32_t nameLen = 0;
            uint8_t visible = 0;
            uint8_t locked = 0;
            uint32_t strokeCount = 0;
            if (!stream.GetUint32(layer.id) ||
                !stream.GetUint32(nameLen) ||
                !stream.GetUint8(visible) ||
                !stream.GetUint8(locked) ||
                !stream.GetSint32(layer.renderOrder) ||
                !stream.GetUint32(strokeCount)) {
                return false;
            }
            layer.visible = visible != 0;
            layer.locked = locked != 0;
            layer.name.assign(nameLen, '\0');
            if (nameLen > 0 && !stream.GetVoid(layer.name.data(), nameLen)) {
                return false;
            }

            layer.strokes.reserve(strokeCount);
            for (uint32_t s = 0; s < strokeCount; ++s) {
                RoadMarkupStroke stroke{};
                uint32_t type = 0;
                uint32_t pointCount = 0;
                uint8_t dashed = 0;
                uint8_t strokeVisible = 0;
                if (!stream.GetUint32(type) ||
                    !stream.GetUint32(pointCount) ||
                    !stream.GetFloat32(stroke.width) ||
                    !stream.GetFloat32(stroke.length) ||
                    !stream.GetFloat32(stroke.rotation) ||
                    !stream.GetUint8(dashed) ||
                    !stream.GetFloat32(stroke.dashLength) ||
                    !stream.GetFloat32(stroke.gapLength) ||
                    !stream.GetUint32(stroke.color) ||
                    !stream.GetFloat32(stroke.opacity) ||
                    !stream.GetUint8(strokeVisible) ||
                    !stream.GetUint32(stroke.layerId)) {
                    return false;
                }
                stroke.type = static_cast<RoadMarkupType>(type);
                stroke.dashed = dashed != 0;
                stroke.visible = strokeVisible != 0;
                stroke.points.reserve(pointCount);

                for (uint32_t p = 0; p < pointCount; ++p) {
                    RoadDecalPoint point{};
                    uint8_t hardCorner = 0;
                    if (!stream.GetFloat32(point.x) ||
                        !stream.GetFloat32(point.y) ||
                        !stream.GetFloat32(point.z) ||
                        !stream.GetUint8(hardCorner)) {
                        return false;
                    }
                    point.hardCorner = hardCorner != 0;
                    stroke.points.push_back(point);
                }
                layer.strokes.push_back(std::move(stroke));
            }
            layers.push_back(std::move(layer));
        }

        gRoadMarkupLayers = std::move(layers);
        gActiveLayerIndex = activeLayerIndex;
        gSelectedLayerIndex = selectedLayerIndex;
        gSelectedStrokeIndex = selectedStrokeIndex;
        return stream.GetError() == 0;
    }

    void BuildStrokeVertices(const RoadMarkupStroke& stroke, std::vector<RoadDecalVertex>& outVerts)
    {
        if (!stroke.visible || stroke.points.empty()) {
            return;
        }

        const auto& props = FindProps(stroke.type);
        const DWORD color = ApplyOpacity(stroke.color != 0 ? stroke.color : props.defaultColor, stroke.opacity);
        const float dashLength = std::max(0.05f, stroke.dashLength);
        const float gapLength = std::max(0.0f, stroke.gapLength);

        switch (props.category) {
        case RoadMarkupCategory::LaneDivider:
            if (stroke.points.size() < 2) {
                return;
            }
            if (stroke.type == RoadMarkupType::DoubleSolidYellow) {
                std::vector<RoadDecalPoint> p1 = stroke.points;
                std::vector<RoadDecalPoint> p2 = stroke.points;
                const float offset = (stroke.width + kDoubleYellowSpacing) * 0.5f;
                for (size_t i = 0; i < stroke.points.size(); ++i) {
                    const auto& prev = stroke.points[(i == 0) ? i : i - 1];
                    const auto& next = stroke.points[(i + 1 < stroke.points.size()) ? i + 1 : i];
                    float tx = 0.0f;
                    float tz = 0.0f;
                    float len = 0.0f;
                    if (!GetDirectionXZ(prev, next, tx, tz, len)) {
                        continue;
                    }
                    const float nx = -tz;
                    const float nz = tx;
                    p1[i].x += nx * offset;
                    p1[i].z += nz * offset;
                    p2[i].x -= nx * offset;
                    p2[i].z -= nz * offset;
                }
                BuildLine(p1, stroke.width, color, false, dashLength, gapLength, outVerts);
                BuildLine(p2, stroke.width, color, false, dashLength, gapLength, outVerts);
            } else {
                const bool dashed = stroke.dashed ||
                                    stroke.type == RoadMarkupType::DashedWhiteLine ||
                                    stroke.type == RoadMarkupType::DashedYellowLine;
                BuildLine(stroke.points, stroke.width, color, dashed, dashLength, gapLength, outVerts);
            }
            break;

        case RoadMarkupCategory::DirectionalArrow:
            switch (stroke.type) {
            case RoadMarkupType::ArrowStraight:
                BuildStraightArrow(stroke, color, outVerts);
                break;
            case RoadMarkupType::ArrowLeft:
                BuildTurnArrow(stroke, true, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowRight:
                BuildTurnArrow(stroke, false, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowLeftRight:
                BuildTurnArrow(stroke, true, false, color, outVerts);
                BuildTurnArrow(stroke, false, false, color, outVerts);
                break;
            case RoadMarkupType::ArrowStraightLeft:
                BuildTurnArrow(stroke, true, true, color, outVerts);
                break;
            case RoadMarkupType::ArrowStraightRight:
                BuildTurnArrow(stroke, false, true, color, outVerts);
                break;
            case RoadMarkupType::ArrowUTurn:
                BuildUTurnArrow(stroke, color, outVerts);
                break;
            default:
                break;
            }
            break;

        case RoadMarkupCategory::Crossing:
            if (stroke.points.size() < 2) {
                return;
            }
            switch (stroke.type) {
            case RoadMarkupType::ZebraCrosswalk:
                BuildCrosswalk(stroke, 0.5f, 0.5f, false, color, outVerts);
                break;
            case RoadMarkupType::LadderCrosswalk:
                BuildCrosswalk(stroke, 0.5f, 0.5f, true, color, outVerts);
                break;
            case RoadMarkupType::ContinentalCrosswalk:
                BuildCrosswalk(stroke, 0.8f, 0.8f, false, color, outVerts);
                break;
            case RoadMarkupType::StopBar:
                BuildLine(stroke.points, std::max(0.1f, stroke.width), color, false, 0.0f, 0.0f, outVerts);
                break;
            default:
                break;
            }
            break;

        case RoadMarkupCategory::ZoneMarking:
        case RoadMarkupCategory::TextLabel:
            break;
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
            LOG_WARN("RoadMarkup: DrawPrimitive failed hr=0x{:08X}", static_cast<uint32_t>(hr));
        }
    }

}
