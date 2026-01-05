#pragma once

#include "cIGZUnknown.h"

struct ImGuiPanelDesc
{
    uint32_t id;
    int32_t order;
    bool visible;
    void (*on_init)(void* data);
    void (*on_render)(void* data);
    void (*on_update)(void* data);
    void (*on_visible_changed)(void* data, bool visible);
    void (*on_shutdown)(void* data);
    void (*on_unregister)(void* data);
    void* data;
};

// ReSharper disable once CppPolymorphicClassWithNonVirtualPublicDestructor
class cIGZImGuiService : public cIGZUnknown
{
public:
    [[nodiscard]] virtual uint32_t GetServiceID() const = 0;
    [[nodiscard]] virtual uint32_t GetApiVersion() const = 0;
    [[nodiscard]] virtual void* GetContext() const = 0;

    virtual bool RegisterPanel(const ImGuiPanelDesc& desc) = 0;
    virtual bool UnregisterPanel(uint32_t panelId) = 0;
    virtual bool SetPanelVisible(uint32_t panelId, bool visible) = 0;
};
