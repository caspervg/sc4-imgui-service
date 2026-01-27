#include "public/TransformLogger.h"

#include <cmath>
#include <intrin.h>

#include "imgui.h"
#include "utils/Logger.h"

TransformLogger::TransformLogger() {
    currentFrameLog_.reserve(128);
    previousFrameLog_.reserve(128);
}

TransformLogger& TransformLogger::Instance() {
    static TransformLogger instance;
    return instance;
}

bool TransformLogger::Install(IDirect3DDevice7* device) {
    if (installed_ || !device) {
        return installed_;
    }

    device_ = device;
    vtable_ = *reinterpret_cast<void***>(device);

    if (!vtable_) {
        LOG_ERROR("TransformLogger: vtable null");
        return false;
    }

    s_OriginalSetTransform = reinterpret_cast<decltype(s_OriginalSetTransform)>(
        vtable_[kSetTransformVTableIndex]);
    s_OriginalBeginScene = reinterpret_cast<decltype(s_OriginalBeginScene)>(
        vtable_[kBeginSceneVTableIndex]);
    s_OriginalMultiplyTransform = reinterpret_cast<decltype(s_OriginalMultiplyTransform)>(
        vtable_[kMultiplyTransformVTableIndex]);

    if (!s_OriginalSetTransform || !s_OriginalBeginScene || !s_OriginalMultiplyTransform) {
        LOG_ERROR("TransformLogger: failed to get original functions");
        return false;
    }

    DWORD oldProtect;

    // Hook SetTransform (index 11)
    if (!VirtualProtect(&vtable_[kSetTransformVTableIndex], sizeof(void*),
        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("TransformLogger: VirtualProtect failed for SetTransform");
        return false;
    }
    InterlockedExchange(
        reinterpret_cast<LONG*>(&vtable_[kSetTransformVTableIndex]),
        reinterpret_cast<LONG>(&SetTransformHook));
    VirtualProtect(&vtable_[kSetTransformVTableIndex], sizeof(void*), oldProtect, &oldProtect);

    // Hook BeginScene (index 5)
    if (!VirtualProtect(&vtable_[kBeginSceneVTableIndex], sizeof(void*),
        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("TransformLogger: VirtualProtect failed for BeginScene");
        return false;
    }
    InterlockedExchange(
        reinterpret_cast<LONG*>(&vtable_[kBeginSceneVTableIndex]),
        reinterpret_cast<LONG>(&BeginSceneHook));
    VirtualProtect(&vtable_[kBeginSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);

    // Hook MultiplyTransform (index 14)
    if (!VirtualProtect(&vtable_[kMultiplyTransformVTableIndex], sizeof(void*),
        PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("TransformLogger: VirtualProtect failed for MultiplyTransform");
        return false;
    }
    InterlockedExchange(
        reinterpret_cast<LONG*>(&vtable_[kMultiplyTransformVTableIndex]),
        reinterpret_cast<LONG>(&MultiplyTransformHook));
    VirtualProtect(&vtable_[kMultiplyTransformVTableIndex], sizeof(void*), oldProtect, &oldProtect);

    installed_ = true;
    LOG_INFO("TransformLogger: installed (SetTransform, BeginScene, MultiplyTransform)");
    return true;
}

void TransformLogger::Uninstall() {
    if (!installed_ || !vtable_) return;

    DWORD oldProtect;

    // Restore SetTransform
    if (s_OriginalSetTransform) {
        if (VirtualProtect(&vtable_[kSetTransformVTableIndex], sizeof(void*),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            InterlockedExchange(
                reinterpret_cast<LONG*>(&vtable_[kSetTransformVTableIndex]),
                reinterpret_cast<LONG>(s_OriginalSetTransform));
            VirtualProtect(&vtable_[kSetTransformVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
    }

    // Restore BeginScene
    if (s_OriginalBeginScene) {
        if (VirtualProtect(&vtable_[kBeginSceneVTableIndex], sizeof(void*),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            InterlockedExchange(
                reinterpret_cast<LONG*>(&vtable_[kBeginSceneVTableIndex]),
                reinterpret_cast<LONG>(s_OriginalBeginScene));
            VirtualProtect(&vtable_[kBeginSceneVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
    }

    // Restore MultiplyTransform
    if (s_OriginalMultiplyTransform) {
        if (VirtualProtect(&vtable_[kMultiplyTransformVTableIndex], sizeof(void*),
            PAGE_EXECUTE_READWRITE, &oldProtect)) {
            InterlockedExchange(
                reinterpret_cast<LONG*>(&vtable_[kMultiplyTransformVTableIndex]),
                reinterpret_cast<LONG>(s_OriginalMultiplyTransform));
            VirtualProtect(&vtable_[kMultiplyTransformVTableIndex], sizeof(void*), oldProtect, &oldProtect);
            }
    }

    installed_ = false;
    device_ = nullptr;
    vtable_ = nullptr;
    LOG_INFO("TransformLogger: uninstalled");
}

void TransformLogger::OnBeginScene() {
    std::lock_guard lock(mutex_);
    previousFrameLog_ = std::move(currentFrameLog_);
    currentFrameLog_.clear();
    currentFrameLog_.reserve(128);
    callIndex_ = 0;
    frameNumber_.fetch_add(1, std::memory_order_release);
    cityMatricesValid_ = false;
}

void TransformLogger::OnEndScene() {}

HRESULT STDMETHODCALLTYPE TransformLogger::BeginSceneHook(IDirect3DDevice7* device) {
    Instance().OnBeginScene();
    return s_OriginalBeginScene(device);
}

HRESULT STDMETHODCALLTYPE TransformLogger::SetTransformHook(
    IDirect3DDevice7* device,
    D3DTRANSFORMSTATETYPE state,
    LPD3DMATRIX matrix)
{
    auto& logger = Instance();

    if (logger.captureEnabled_ && matrix) {
        if (state == D3DTRANSFORMSTATE_VIEW ||
            state == D3DTRANSFORMSTATE_PROJECTION ||
            state == D3DTRANSFORMSTATE_WORLD) {
            uint32_t callerAddr = reinterpret_cast<uint32_t>(_ReturnAddress());
            logger.RecordTransform(state, *matrix, callerAddr);
        }
    }

    return s_OriginalSetTransform(device, state, matrix);
}

HRESULT STDMETHODCALLTYPE TransformLogger::MultiplyTransformHook(
    IDirect3DDevice7* device,
    D3DTRANSFORMSTATETYPE state,
    LPD3DMATRIX matrix)
{
    auto& logger = Instance();

    if (logger.captureEnabled_ && matrix) {
        static int multiplyLogCount = 0;
        if (multiplyLogCount < 20) {
            uint32_t callerAddr = reinterpret_cast<uint32_t>(_ReturnAddress());

            const char* stateName = "?";
            if (state == D3DTRANSFORMSTATE_VIEW) stateName = "VIEW";
            else if (state == D3DTRANSFORMSTATE_PROJECTION) stateName = "PROJ";
            else if (state == D3DTRANSFORMSTATE_WORLD) stateName = "WORLD";

            LOG_INFO("MultiplyTransform #{} {} from 0x{:08X}:",
                     multiplyLogCount, stateName, callerAddr);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix->_11, matrix->_12, matrix->_13, matrix->_14);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix->_21, matrix->_22, matrix->_23, matrix->_24);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix->_31, matrix->_32, matrix->_33, matrix->_34);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix->_41, matrix->_42, matrix->_43, matrix->_44);
            multiplyLogCount++;
        }
    }

    return s_OriginalMultiplyTransform(device, state, matrix);
}

void TransformLogger::RecordTransform(D3DTRANSFORMSTATETYPE state,
    const D3DMATRIX& matrix, uint32_t callerAddr)
{
    std::lock_guard lock(mutex_);

    if (targetCallerAddress_ != 0 && callerAddr != targetCallerAddress_) {
        callIndex_++;
        return;
    }

    uint32_t currentFrame = frameNumber_.load(std::memory_order_acquire);

    TransformLogEntry entry{};
    entry.state = state;
    entry.matrix = matrix;
    entry.callIndex = callIndex_++;
    entry.callerAddress = callerAddr;
    entry.frameNumber = currentFrame;

    currentFrameLog_.push_back(entry);

    if (trackUniqueMatrices_) {
        UpdateUniqueMatrixTracking(entry);
    }

    // Log projection matrix details for debugging
    if (state == D3DTRANSFORMSTATE_PROJECTION) {
        static int projLogCount = 0;
        if (projLogCount < 10) {
            LOG_INFO("PROJECTION #{} from 0x{:08X}:", projLogCount, callerAddr);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._11, matrix._12, matrix._13, matrix._14);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._21, matrix._22, matrix._23, matrix._24);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._31, matrix._32, matrix._33, matrix._34);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._41, matrix._42, matrix._43, matrix._44);
            projLogCount++;
        }
    }

    if (state == D3DTRANSFORMSTATE_VIEW) {
        static int viewLogCount = 0;
        if (viewLogCount < 5) {
            LOG_INFO("VIEW #{} from 0x{:08X}:", viewLogCount, callerAddr);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._11, matrix._12, matrix._13, matrix._14);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._21, matrix._22, matrix._23, matrix._24);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._31, matrix._32, matrix._33, matrix._34);
            LOG_INFO("  [{:10.4f} {:10.4f} {:10.4f} {:10.4f}]",
                     matrix._41, matrix._42, matrix._43, matrix._44);
            viewLogCount++;
        }
    }

    // Capture city view matrices: first perspective projection
    if (state == D3DTRANSFORMSTATE_PROJECTION &&
        !cityMatricesValid_ &&
        IsPerspectiveProjection(matrix)) {

        LOG_INFO("TransformLogger: found perspective projection, capturing city matrices");
        cityProjectionMatrix_ = matrix;

        for (auto it = currentFrameLog_.rbegin(); it != currentFrameLog_.rend(); ++it) {
            if (it->state == D3DTRANSFORMSTATE_VIEW) {
                cityViewMatrix_ = it->matrix;
                cityMatricesValid_ = true;
                LOG_INFO("TransformLogger: city matrices captured successfully");
                break;
            }
        }

        if (!cityMatricesValid_) {
            LOG_WARN("TransformLogger: found perspective projection but no view matrix yet");
        }
    }

    // Change city matrix capture logic
    if (state == D3DTRANSFORMSTATE_PROJECTION && !cityMatricesValid_) {
        // Accept the first non-identity projection matrix
        if (!IsIdentityMatrix(matrix)) {
            LOG_INFO("TransformLogger: capturing projection matrix (persp={}, ortho={})",
                     IsPerspectiveProjection(matrix), IsOrthographicProjection(matrix));
            cityProjectionMatrix_ = matrix;

            for (auto it = currentFrameLog_.rbegin(); it != currentFrameLog_.rend(); ++it) {
                if (it->state == D3DTRANSFORMSTATE_VIEW && !IsIdentityMatrix(it->matrix)) {
                    cityViewMatrix_ = it->matrix;
                    cityMatricesValid_ = true;
                    LOG_INFO("TransformLogger: city matrices captured (view+proj)");
                    break;
                }
            }
        }
    }
}

void TransformLogger::UpdateUniqueMatrixTracking(const TransformLogEntry& entry) {
    std::vector<MatrixFingerprint>* list = nullptr;

    if (entry.state == D3DTRANSFORMSTATE_PROJECTION) {
        list = &uniqueProjections_;
    }
    else if (entry.state == D3DTRANSFORMSTATE_VIEW) {
        list = &uniqueViews_;
    }
    else {
        return;
    }

    for (auto& fp : *list) {
        if (MatricesEqual(fp.matrix, entry.matrix, matrixSimilarityThreshold_)) {
            fp.lastSeenFrame = entry.frameNumber;
            fp.hitCount++;
            return;
        }
    }

    MatrixFingerprint fp{};
    fp.hash = HashMatrix(entry.matrix);
    fp.matrix = entry.matrix;
    fp.state = entry.state;
    fp.firstSeenFrame = entry.frameNumber;
    fp.lastSeenFrame = entry.frameNumber;
    fp.callerAddress = entry.callerAddress;
    fp.hitCount = 1;
    fp.analysis = AnalyzeMatrix(entry.matrix, entry.state);
    list->push_back(fp);
}

std::vector<TransformLogEntry> TransformLogger::GetCurrentFrameLog() const {
    std::lock_guard lock(mutex_);
    return currentFrameLog_;
}

std::vector<TransformLogEntry> TransformLogger::GetPreviousFrameLog() const {
    std::lock_guard lock(mutex_);
    return previousFrameLog_;
}

bool TransformLogger::GetCityViewMatrices(D3DMATRIX& outView, D3DMATRIX& outProjection) const {
    std::lock_guard lock(mutex_);
    if (!cityMatricesValid_) return false;
    outView = cityViewMatrix_;
    outProjection = cityProjectionMatrix_;
    return true;
}

bool TransformLogger::HasValidCityMatrices() const {
    std::lock_guard lock(mutex_);
    return cityMatricesValid_;
}

std::vector<MatrixFingerprint> TransformLogger::GetUniqueProjections() const {
    std::lock_guard lock(mutex_);
    return uniqueProjections_;
}

std::vector<MatrixFingerprint> TransformLogger::GetUniqueViews() const {
    std::lock_guard lock(mutex_);
    return uniqueViews_;
}

void TransformLogger::ClearUniqueMatrixHistory() {
    std::lock_guard lock(mutex_);
    uniqueProjections_.clear();
    uniqueViews_.clear();
}

ScreenPoint TransformLogger::WorldToScreen(float worldX, float worldY, float worldZ) const {
    D3DMATRIX view, projection;
    if (!GetCityViewMatrices(view, projection)) {
        return {0, 0, 0, false};
    }

    ImGuiIO& io = ImGui::GetIO();
    return WorldToScreen(worldX, worldY, worldZ, view, projection,
        io.DisplaySize.x, io.DisplaySize.y);
}

ScreenPoint TransformLogger::WorldToScreen(
    float worldX, float worldY, float worldZ,
    const D3DMATRIX& view, const D3DMATRIX& projection,
    float screenWidth, float screenHeight)
{
    ScreenPoint result = {0, 0, 0, false};

    // View transform
    float vx = worldX * view._11 + worldY * view._21 + worldZ * view._31 + view._41;
    float vy = worldX * view._12 + worldY * view._22 + worldZ * view._32 + view._42;
    float vz = worldX * view._13 + worldY * view._23 + worldZ * view._33 + view._43;
    float vw = worldX * view._14 + worldY * view._24 + worldZ * view._34 + view._44;

    // Projection transform
    float cx = vx * projection._11 + vy * projection._21 + vz * projection._31 + vw * projection._41;
    float cy = vx * projection._12 + vy * projection._22 + vz * projection._32 + vw * projection._42;
    float cz = vx * projection._13 + vy * projection._23 + vz * projection._33 + vw * projection._43;
    float cw = vx * projection._14 + vy * projection._24 + vz * projection._34 + vw * projection._44;

    if (cw <= 0.0001f) return result;

    float ndcX = cx / cw;
    float ndcY = cy / cw;
    float ndcZ = cz / cw;

    result.x = (ndcX + 1.0f) * 0.5f * screenWidth;
    result.y = (1.0f - ndcY) * 0.5f * screenHeight;
    result.depth = ndcZ;
    result.visible = (ndcX >= -1.0f && ndcX <= 1.0f &&
        ndcY >= -1.0f && ndcY <= 1.0f &&
        ndcZ >= 0.0f && ndcZ <= 1.0f);

    return result;
}

TransformAnalysis TransformLogger::AnalyzeMatrix(const D3DMATRIX& matrix, D3DTRANSFORMSTATETYPE state) {
    TransformAnalysis a{};
    a.isIdentity = IsIdentityMatrix(matrix);

    if (state == D3DTRANSFORMSTATE_PROJECTION) {
        a.isPerspective = IsPerspectiveProjection(matrix);
        a.isOrthographic = IsOrthographicProjection(matrix);

        if (a.isPerspective && std::abs(matrix._22) > 0.0001f) {
            float tanHalfFov = 1.0f / matrix._22;
            a.estimatedFovDegrees = 2.0f * std::atan(tanHalfFov) * (180.0f / 3.14159265f);

            if (std::abs(matrix._33) > 0.0001f && std::abs(matrix._43) > 0.0001f) {
                a.estimatedNear = matrix._43 / matrix._33;
                a.estimatedFar = matrix._43 / (matrix._33 - 1.0f);
                if (a.estimatedNear > a.estimatedFar) std::swap(a.estimatedNear, a.estimatedFar);
                if (a.estimatedNear < 0) a.estimatedNear = -a.estimatedNear;
            }
        }
    }

    return a;
}

// Relax the perspective detection slightly
bool TransformLogger::IsPerspectiveProjection(const D3DMATRIX& proj) {
    // Standard perspective: _34 is -1 or 1, _44 is 0
    // But some games use variations

    // Log for debugging
    static int checkCount = 0;
    if (checkCount < 10) {
        LOG_INFO("IsPerspectiveProjection check: _34={}, _44={}", proj._34, proj._44);
        checkCount++;
    }

    // Original strict check
    bool strict = (std::abs(proj._34) > 0.5f && std::abs(proj._44) < 0.1f);

    // Looser check - any non-zero _34 with small _44 might be perspective
    bool loose = (std::abs(proj._34) > 0.001f && std::abs(proj._44) < 0.5f);

    return strict;  // Start with strict, can switch to loose if needed
}

bool TransformLogger::IsOrthographicProjection(const D3DMATRIX& proj) {
    return (std::abs(proj._34) < 0.001f && std::abs(proj._44 - 1.0f) < 0.001f);
}

bool TransformLogger::IsIdentityMatrix(const D3DMATRIX& m) {
    constexpr float e = 0.0001f;
    return std::abs(m._11 - 1) < e && std::abs(m._12) < e && std::abs(m._13) < e && std::abs(m._14) < e &&
           std::abs(m._21) < e && std::abs(m._22 - 1) < e && std::abs(m._23) < e && std::abs(m._24) < e &&
           std::abs(m._31) < e && std::abs(m._32) < e && std::abs(m._33 - 1) < e && std::abs(m._34) < e &&
           std::abs(m._41) < e && std::abs(m._42) < e && std::abs(m._43) < e && std::abs(m._44 - 1) < e;
}

bool TransformLogger::MatricesEqual(const D3DMATRIX& a, const D3DMATRIX& b, float threshold) {
    const float* fa = &a._11;
    const float* fb = &b._11;
    for (int i = 0; i < 16; ++i) {
        if (std::abs(fa[i] - fb[i]) > threshold) return false;
    }
    return true;
}

uint32_t TransformLogger::HashMatrix(const D3DMATRIX& m) {
    uint32_t hash = 2166136261u;
    const auto* data = reinterpret_cast<const uint8_t*>(&m);
    for (size_t i = 0; i < sizeof(D3DMATRIX); ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}