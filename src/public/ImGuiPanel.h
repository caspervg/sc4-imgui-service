#pragma once

struct ImGuiPanel
{
    virtual ~ImGuiPanel() = default;
    virtual void OnInit() {}
    virtual void OnRender() = 0;
    virtual void OnUpdate() {}
    virtual void OnVisibleChanged(bool) {}
    virtual void OnShutdown() {}
    virtual void OnUnregister() {}
};
