// src/public/TransformLogger.h
#pragma once

#include <atomic>
#include <d3d.h>
#include <mutex>
#include <vector>

struct TransformLogEntry {
    D3DTRANSFORMSTATETYPE state;
    D3DMATRIX matrix;
    uint32_t callIndex;
    uint32_t callerAddress;
    uint32_t frameNumber;
};

struct TransformAnalysis {
    bool isPerspective;
    bool isOrthographic;
    bool isIdentity;
    float estimatedFovDegrees;
    float estimatedNear;
    float estimatedFar;
};

struct MatrixFingerprint {
    uint32_t hash;
    D3DMATRIX matrix;
    D3DTRANSFORMSTATETYPE state;
    uint32_t firstSeenFrame;
    uint32_t lastSeenFrame;
    uint32_t callerAddress;
    uint32_t hitCount;
    TransformAnalysis analysis;
};

struct ScreenPoint {
    float x;
    float y;
    float depth;
    bool visible;
};

// Singleton that hooks D3D7 SetTransform to capture view/projection matrices.
// Use this to implement world-to-screen coordinate conversion for overlays.
//
// Usage:
//   // In your panel's OnInit, after getting cIGZImGuiService:
//   IDirect3DDevice7* d3d; IDirectDraw7* dd;
//   if (service->AcquireD3DInterfaces(&d3d, &dd)) {
//       TransformLogger::Instance().Install(d3d);
//       d3d->Release(); dd->Release();
//   }
//
//   // In your render code:
//   ScreenPoint pt = TransformLogger::Instance().WorldToScreen(worldX, worldY, worldZ);
//   if (pt.visible) {
//       // Draw at (pt.x, pt.y)
//   }
//
class TransformLogger {
public:
    static TransformLogger& Instance();

    TransformLogger(const TransformLogger&) = delete;
    TransformLogger& operator=(const TransformLogger&) = delete;

    // Hook management - call Install once when you have a D3D device
    bool Install(IDirect3DDevice7* device);
    void Uninstall();
    [[nodiscard]] bool IsInstalled() const { return installed_; }

    // Frame events (called automatically by hooks)
    void OnBeginScene();
    void OnEndScene();

    // Access logged transforms from previous frame
    [[nodiscard]] std::vector<TransformLogEntry> GetCurrentFrameLog() const;
    [[nodiscard]] std::vector<TransformLogEntry> GetPreviousFrameLog() const;
    [[nodiscard]] uint32_t GetFrameNumber() const { return frameNumber_.load(std::memory_order_acquire); }

    // Get the captured city view matrices (the ones you need for overlays)
    [[nodiscard]] bool GetCityViewMatrices(D3DMATRIX& outView, D3DMATRIX& outProjection) const;
    [[nodiscard]] bool HasValidCityMatrices() const;

    // Unique matrix tracking (for debugging which matrices SC4 uses)
    [[nodiscard]] std::vector<MatrixFingerprint> GetUniqueProjections() const;
    [[nodiscard]] std::vector<MatrixFingerprint> GetUniqueViews() const;
    void ClearUniqueMatrixHistory();

    // World to screen projection - the main API you'll use
    [[nodiscard]] ScreenPoint WorldToScreen(float worldX, float worldY, float worldZ) const;

    // Static version if you have your own matrices
    [[nodiscard]] static ScreenPoint WorldToScreen(
        float worldX, float worldY, float worldZ,
        const D3DMATRIX& view, const D3DMATRIX& projection,
        float screenWidth, float screenHeight);

    // Matrix analysis utilities
    [[nodiscard]] static TransformAnalysis AnalyzeMatrix(const D3DMATRIX& matrix, D3DTRANSFORMSTATETYPE state);
    [[nodiscard]] static bool IsPerspectiveProjection(const D3DMATRIX& proj);
    [[nodiscard]] static bool IsOrthographicProjection(const D3DMATRIX& proj);
    [[nodiscard]] static bool IsIdentityMatrix(const D3DMATRIX& m);
    [[nodiscard]] static bool MatricesEqual(const D3DMATRIX& a, const D3DMATRIX& b, float threshold);
    [[nodiscard]] static uint32_t HashMatrix(const D3DMATRIX& m);

    // Configuration
    void SetCaptureEnabled(bool enabled) { captureEnabled_ = enabled; }
    [[nodiscard]] bool IsCaptureEnabled() const { return captureEnabled_; }
    void SetTrackUniqueMatrices(bool enabled) { trackUniqueMatrices_ = enabled; }
    [[nodiscard]] bool IsTrackingUniqueMatrices() const { return trackUniqueMatrices_; }
    void SetMatrixSimilarityThreshold(float threshold) { matrixSimilarityThreshold_ = threshold; }
    [[nodiscard]] float GetMatrixSimilarityThreshold() const { return matrixSimilarityThreshold_; }

    // Filter to only record from specific caller (0 = no filter)
    void SetTargetCallerAddress(uint32_t addr) { targetCallerAddress_ = addr; }
    [[nodiscard]] uint32_t GetTargetCallerAddress() const { return targetCallerAddress_; }

private:
    TransformLogger();
    ~TransformLogger() = default;

    static HRESULT STDMETHODCALLTYPE SetTransformHook(
        IDirect3DDevice7* device,
        D3DTRANSFORMSTATETYPE state,
        LPD3DMATRIX matrix);

    static HRESULT STDMETHODCALLTYPE BeginSceneHook(IDirect3DDevice7* device);
    static HRESULT STDMETHODCALLTYPE MultiplyTransformHook(
        IDirect3DDevice7* device,
        D3DTRANSFORMSTATETYPE state,
        LPD3DMATRIX matrix);

    void RecordTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX& matrix, uint32_t callerAddr);
    void UpdateUniqueMatrixTracking(const TransformLogEntry& entry);

private:
    mutable std::mutex mutex_;
    std::vector<TransformLogEntry> currentFrameLog_;
    std::vector<TransformLogEntry> previousFrameLog_;

    std::vector<MatrixFingerprint> uniqueProjections_;
    std::vector<MatrixFingerprint> uniqueViews_;

    D3DMATRIX cityViewMatrix_{};
    D3DMATRIX cityProjectionMatrix_{};
    bool cityMatricesValid_ = false;

    IDirect3DDevice7* device_ = nullptr;
    void** vtable_ = nullptr;

    inline static HRESULT(STDMETHODCALLTYPE* s_OriginalSetTransform)(
        IDirect3DDevice7*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX) = nullptr;
    inline static HRESULT(STDMETHODCALLTYPE* s_OriginalBeginScene)(
        IDirect3DDevice7*) = nullptr;
    inline static HRESULT(STDMETHODCALLTYPE* s_OriginalMultiplyTransform)(
    IDirect3DDevice7*, D3DTRANSFORMSTATETYPE, LPD3DMATRIX) = nullptr;

    uint32_t callIndex_ = 0;
    std::atomic<uint32_t> frameNumber_{0};
    uint32_t targetCallerAddress_ = 0;

    bool installed_ = false;
    bool captureEnabled_ = true;
    bool trackUniqueMatrices_ = true;
    float matrixSimilarityThreshold_ = 0.0001f;

    static constexpr size_t kBeginSceneVTableIndex = 5;
    static constexpr size_t kSetTransformVTableIndex = 11;
    static constexpr size_t kMultiplyTransformVTableIndex = 14;
};