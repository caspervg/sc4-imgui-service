#pragma once

#include "cIGZImGuiService.h"

// RAII wrapper for automatic texture lifetime management with device generation tracking.
// This class provides automatic cleanup and device loss handling for ImGui textures.
//
// Thread safety: Not thread-safe. Must be used from the render thread only.
//
// Example usage:
//   ImGuiTexture myTexture;
//   myTexture.Create(service, width, height, pixelData);
//   
//   // In render loop:
//   Texture2D tex = myTexture.GetTexture();
//   if (tex.id != 0) {
//       rlImGuiImageSize(&tex, width, height);
//   }
//
class ImGuiTexture
{
public:
    ImGuiTexture() 
        : service_(nullptr)
        , handle_{0, 0}
        , lastKnownGeneration_(0) {}

    ~ImGuiTexture() {
        Release();
    }

    // Non-copyable
    ImGuiTexture(const ImGuiTexture&) = delete;
    ImGuiTexture& operator=(const ImGuiTexture&) = delete;

    // Movable
    ImGuiTexture(ImGuiTexture&& other) noexcept
        : service_(other.service_)
        , handle_(other.handle_)
        , lastKnownGeneration_(other.lastKnownGeneration_) {
        other.service_ = nullptr;
        other.handle_ = {0, 0};
        other.lastKnownGeneration_ = 0;
    }

    ImGuiTexture& operator=(ImGuiTexture&& other) noexcept {
        if (this != &other) {
            Release();
            service_ = other.service_;
            handle_ = other.handle_;
            lastKnownGeneration_ = other.lastKnownGeneration_;
            other.service_ = nullptr;
            other.handle_ = {0, 0};
            other.lastKnownGeneration_ = 0;
        }
        return *this;
    }

    // Creates a texture from RGBA32 pixel data.
    // Returns true on success, false on failure.
    bool Create(cIGZImGuiService* service, uint32_t width, uint32_t height, 
                const void* pixels, bool useSystemMemory = false) {
        if (!service) {
            return false;
        }

        Release();

        ImGuiTextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.pixels = pixels;
        desc.useSystemMemory = useSystemMemory;

        service_ = service;
        handle_ = service_->CreateTexture(desc);
        lastKnownGeneration_ = service_->GetDeviceGeneration();

        return handle_.id != 0;
    }

    // Gets the raylib texture for use with rlImGuiImage* helpers.
    // Returns texture.id == 0 if texture is invalid or device generation changed.
    // Automatically detects device generation changes and returns an invalid texture.
    Texture2D GetTexture() {
        Texture2D invalid{};
        invalid.id = 0;
        if (!service_ || handle_.id == 0) {
            return invalid;
        }

        // Check if device generation changed
        const uint32_t currentGen = service_->GetDeviceGeneration();
        if (currentGen != lastKnownGeneration_) {
            // Device was reset, texture handle is stale
            handle_.generation = 0;  // Invalidate
            lastKnownGeneration_ = currentGen;  // Update to avoid repeated warnings
            return invalid;
        }

        return service_->GetTexture(handle_);
    }

    // Checks if the texture is valid.
    bool IsValid() const {
        if (!service_ || handle_.id == 0) {
            return false;
        }
        return service_->IsTextureValid(handle_);
    }

    // Releases the texture and frees resources.
    void Release() {
        if (service_ && handle_.id != 0) {
            service_->ReleaseTexture(handle_);
        }
        service_ = nullptr;
        handle_ = {0, 0};
        lastKnownGeneration_ = 0;
    }

    // Gets the raw handle (for advanced use cases).
    ImGuiTextureHandle GetHandle() const {
        return handle_;
    }

private:
    cIGZImGuiService* service_;
    ImGuiTextureHandle handle_;
    uint32_t lastKnownGeneration_;
};
