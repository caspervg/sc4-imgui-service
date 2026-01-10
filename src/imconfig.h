// ImGui build configuration for shared usage via rlimgui.dll.
#pragma once

#if defined(_WIN32)
#  if defined(IMGUI_EXPORTS)
#    define IMGUI_API __declspec(dllexport)
#  elif defined(IMGUI_IMPORTS)
#    define IMGUI_API __declspec(dllimport)
#  endif
#endif

