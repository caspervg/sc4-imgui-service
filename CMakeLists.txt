cmake_minimum_required(VERSION 4.0)

# Force 32-bit build for SimCity 4 compatibility BEFORE project() call
# Set the platform regardless of current setting to ensure 32-bit
set(CMAKE_GENERATOR_PLATFORM "Win32")
message(STATUS "Forcing 32-bit build for SC4 compatibility")
#
## Force release runtime BEFORE project() call - this is critical
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_CXX_FLAGS_DEBUG "/MD /Zi /Ob0 /Od")
set(CMAKE_C_FLAGS_DEBUG "/MD /Zi /Ob0 /Od")

# Additional 32-bit enforcement
if(WIN32)
    set(CMAKE_SIZEOF_VOID_P 4)
    set(CMAKE_C_SIZEOF_DATA_PTR 4)
    set(CMAKE_CXX_SIZEOF_DATA_PTR 4)
endif()

project(SC4ImguiBackend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform specific settings
if(WIN32)
    add_definitions(-DWIN32 -D_WINDOWS -DUNICODE -D_UNICODE)
endif()

# MSVC specific settings
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # Force release runtime for all configurations
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

    # Override debug flags to use release runtime
    set(CMAKE_CXX_FLAGS_DEBUG "/MD /Zi /Ob0 /Od /RTC1")
    set(CMAKE_C_FLAGS_DEBUG "/MD /Zi /Ob0 /Od /RTC1")

    # Enable debugging in release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()

add_subdirectory(vendor/spdlog)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

find_package(raylib QUIET)
if (NOT raylib_FOUND)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 5.5
            GIT_SHALLOW 1
    )
    FetchContent_MakeAvailable(raylib)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()
set(RAYLIB_INCLUDE_DIRS "")
if (TARGET raylib)
    get_target_property(RAYLIB_INCLUDE_DIRS raylib INTERFACE_INCLUDE_DIRECTORIES)
endif()
if (NOT RAYLIB_INCLUDE_DIRS AND DEFINED raylib_SOURCE_DIR)
    set(RAYLIB_INCLUDE_DIRS ${raylib_SOURCE_DIR}/src)
endif()

set(IMGUI_VERSION 1.92.4-docking)
set(IMGUI_TAG e7d2d636affeeac1fc7175fb13472ca02d1bc0d1)
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG ${IMGUI_TAG}
)
FetchContent_MakeAvailable(imgui)
add_library(imgui SHARED
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
add_library(imgui::imgui ALIAS imgui)
target_include_directories(imgui PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
if(WIN32)
    target_compile_definitions(imgui PRIVATE IMGUI_EXPORTS)
    target_link_libraries(imgui PRIVATE user32 gdi32)
endif()
target_compile_definitions(imgui PRIVATE IMGUI_USER_CONFIG=\"imconfig.h\")
set_target_properties(imgui PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

set(RLIMGUI_TAG 4d8a61842903978bc42adf3347cd34f4e6524efc)
if (NOT TARGET rlimgui)
    FetchContent_Declare(
            rlimgui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG ${RLIMGUI_TAG}
    )
    FetchContent_MakeAvailable(rlimgui)
    add_library(rlimgui SHARED ${rlimgui_SOURCE_DIR}/rlImGui.cpp)
    target_include_directories(rlimgui PUBLIC
            ${rlimgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${RAYLIB_INCLUDE_DIRS}
            ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(rlimgui PUBLIC RLIMGUI_ALWAYS_TRACK_MOUSE BUILD_LIBTYPE_SHARED)
    target_link_libraries(rlimgui PRIVATE imgui raylib)
    if (WIN32)
        target_compile_definitions(rlimgui PRIVATE IMGUI_IMPORTS)
    endif()
    target_compile_definitions(rlimgui PRIVATE IMGUI_USER_CONFIG=\"imconfig.h\")
    set_target_properties(rlimgui PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ImGui library (docking)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
set(IMGUI_BACKENDS_DIR ${imgui_SOURCE_DIR}/backends)

# GZCOM-DLL (vendor/gzcom-dll has sources, build local static lib)
set(GZCOM_DIR ${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll)

# Collect all public headers and sources
file(GLOB_RECURSE GZCOM_HEADERS
        ${GZCOM_DIR}/include/*.h
        ${GZCOM_DIR}/include/*.hpp
)
file(GLOB_RECURSE GZCOM_SOURCES
        ${GZCOM_DIR}/src/*.cpp
)

# Exclude vendor app entry and example directors from gzcom sources
list(REMOVE_ITEM GZCOM_SOURCES
        ${GZCOM_DIR}/src/main.cpp
        ${GZCOM_DIR}/src/cGZExampleDllDirector.cpp
        ${GZCOM_DIR}/src/cGZExtraCheatsDllDirector.cpp
        ${GZCOM_DIR}/src/cGZExtraExtraCheatsDllDirector.cpp
        ${GZCOM_DIR}/src/cGZCityHallUpgradeDllDirector.cpp
)

# Build gzcom2 as a static library to match existing linkage
add_library(gzcom2 STATIC ${GZCOM_SOURCES} ${GZCOM_HEADERS})

# Expose include path to dependents
target_include_directories(gzcom2 PUBLIC
        ${GZCOM_DIR}/include
)

# Include directories
include_directories(
        ${CMAKE_SOURCE_DIR}/src
)

# ImGui service DLL
set(IMGUI_SERVICE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/service/ImGuiService.cpp
        ${CMAKE_SOURCE_DIR}/src/service/ImGuiServiceDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
        ${CMAKE_SOURCE_DIR}/src/DX7InterfaceHook.cpp
)

add_library(SC4ImGuiService SHARED ${IMGUI_SERVICE_SOURCES} SC4ImGuiService.def)

target_include_directories(SC4ImGuiService PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${RAYLIB_INCLUDE_DIRS}
)

target_link_libraries(SC4ImGuiService PRIVATE
        rlimgui
        imgui
        raylib
        gzcom2
        spdlog
)

if(WIN32)
    target_link_libraries(SC4ImGuiService PRIVATE kernel32 user32 Version ddraw dxguid)
endif()

set_target_properties(SC4ImGuiService PROPERTIES
        OUTPUT_NAME "SC4ImGuiService"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiService PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()
if (WIN32)
    target_compile_definitions(SC4ImGuiService PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            MMNOSOUND
            IMGUI_IMPORTS
            IMGUI_USER_CONFIG=\"imconfig.h\"
    )
    target_link_options(SC4ImGuiService PRIVATE
            /FORCE:MULTIPLE
            /IGNORE:4006,4221
    )
endif()

# ImGui sample DLL
set(IMGUI_SAMPLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSample SHARED ${IMGUI_SAMPLE_SOURCES} SC4ImGuiSample.def)

target_include_directories(SC4ImGuiSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${RAYLIB_INCLUDE_DIRS}
)

target_link_libraries(SC4ImGuiSample PRIVATE
        rlimgui
        imgui
        raylib
        gzcom2
        spdlog
)

set_target_properties(SC4ImGuiSample PROPERTIES
        OUTPUT_NAME "SC4ImGuiSample"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiSample PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()
if (WIN32)
    target_compile_definitions(SC4ImGuiSample PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            NOGDI
            MMNOSOUND
            IMGUI_IMPORTS
            IMGUI_USER_CONFIG=\"imconfig.h\"
    )
    target_link_options(SC4ImGuiSample PRIVATE
            /FORCE:MULTIPLE
            /IGNORE:4006,4221
    )
endif()

# ImGui city-only sample DLL
set(IMGUI_SAMPLE_CITY_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleCityDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleCity SHARED ${IMGUI_SAMPLE_CITY_SOURCES} SC4ImGuiSampleCity.def)

target_include_directories(SC4ImGuiSampleCity PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${RAYLIB_INCLUDE_DIRS}
)

target_link_libraries(SC4ImGuiSampleCity PRIVATE
        rlimgui
        imgui
        raylib
        gzcom2
        spdlog
)

set_target_properties(SC4ImGuiSampleCity PROPERTIES
        OUTPUT_NAME "SC4ImGuiSampleCity"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiSampleCity PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()
if (WIN32)
    target_compile_definitions(SC4ImGuiSampleCity PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            NOGDI
            MMNOSOUND
            IMGUI_IMPORTS
            IMGUI_USER_CONFIG=\"imconfig.h\"
    )
    target_link_options(SC4ImGuiSampleCity PRIVATE
            /FORCE:MULTIPLE
            /IGNORE:4006,4221
    )
endif()

# ImGui demo sample DLL
set(IMGUI_SAMPLE_DEMO_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiSampleDemoDirector.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiSampleDemo SHARED ${IMGUI_SAMPLE_DEMO_SOURCES} SC4ImGuiSampleDemo.def)

target_include_directories(SC4ImGuiSampleDemo PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${RAYLIB_INCLUDE_DIRS}
)

target_link_libraries(SC4ImGuiSampleDemo PRIVATE
        rlimgui
        imgui
        raylib
        gzcom2
        spdlog
)

set_target_properties(SC4ImGuiSampleDemo PROPERTIES
        OUTPUT_NAME "SC4ImGuiSampleDemo"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiSampleDemo PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()
if (WIN32)
    target_compile_definitions(SC4ImGuiSampleDemo PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            NOGDI
            MMNOSOUND
            IMGUI_IMPORTS
            IMGUI_USER_CONFIG=\"imconfig.h\"
    )
    target_link_options(SC4ImGuiSampleDemo PRIVATE
            /FORCE:MULTIPLE
            /IGNORE:4006,4221
    )
endif()

# ImGui texture sample DLL
set(IMGUI_TEXTURE_SAMPLE_SOURCES
        ${CMAKE_SOURCE_DIR}/src/sample/ImGuiTextureSample.cpp
        ${CMAKE_SOURCE_DIR}/src/utils/Logger.cpp
)

add_library(SC4ImGuiTextureSample SHARED ${IMGUI_TEXTURE_SAMPLE_SOURCES} SC4ImGuiTextureSample.def)

target_include_directories(SC4ImGuiTextureSample PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/service
        ${IMGUI_DIR}
        ${RAYLIB_INCLUDE_DIRS}
)

target_link_libraries(SC4ImGuiTextureSample PRIVATE
        rlimgui
        imgui
        raylib
        gzcom2
        spdlog
)

set_target_properties(SC4ImGuiTextureSample PROPERTIES
        OUTPUT_NAME "SC4ImGuiTextureSample"
        SUFFIX ".dll"
        PREFIX ""
)

if(MSVC)
    set_target_properties(SC4ImGuiTextureSample PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
endif()
if (WIN32)
    target_compile_definitions(SC4ImGuiTextureSample PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            NOGDI
            MMNOSOUND
            IMGUI_IMPORTS
            IMGUI_USER_CONFIG=\"imconfig.h\"
    )
    target_link_options(SC4ImGuiTextureSample PRIVATE
            /FORCE:MULTIPLE
            /IGNORE:4006,4221
    )
endif()

add_custom_target(SC4ImGuiAllTargets
        DEPENDS rlimgui SC4ImGuiService SC4ImGuiSample SC4ImGuiSampleCity SC4ImGuiSampleDemo SC4ImGuiTextureSample
)

# Automatic deployment to SC4 directories
if(WIN32 AND NOT DEFINED ENV{CI})
    # Try to find SC4 installation directory
    set(SC4_INSTALL_PATHS
            "C:/Program Files (x86)/SimCity 4 Deluxe Edition"
            "C:/Program Files (x86)/Maxis/SimCity 4 Deluxe"
            "C:/Program Files/Maxis/SimCity 4 Deluxe"
            "C:/Program Files (x86)/Steam/steamapps/common/SimCity 4 Deluxe"
            "C:/Program Files/Steam/steamapps/common/SimCity 4 Deluxe"
    )

    foreach(SC4_PATH ${SC4_INSTALL_PATHS})
        if(EXISTS "${SC4_PATH}/Apps/SimCity 4.exe")
            set(SC4_INSTALL_DIR "${SC4_PATH}")
            break()
        endif()
    endforeach()

    # Get user's Documents folder
    set(USERPROFILE_DIR "$ENV{USERPROFILE}")
    set(SC4_PLUGINS_DIR "${USERPROFILE_DIR}/Documents/SimCity 4/Plugins")

    if(SC4_INSTALL_DIR)
        message(STATUS "Found SC4 installation at: ${SC4_INSTALL_DIR}")

        add_custom_command(TARGET rlimgui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_INSTALL_DIR}/Apps"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rlimgui>
                "${SC4_INSTALL_DIR}/Apps/"
                COMMENT "Deploying rlimgui.dll to Apps folder"
        )
        if (TARGET raylib)
            set(RAYLIB_TARGET_NAME raylib)
        elseif (TARGET raylib::raylib)
            set(RAYLIB_TARGET_NAME raylib::raylib)
        endif()
        if (RAYLIB_TARGET_NAME)
            add_custom_command(TARGET SC4ImGuiService POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_INSTALL_DIR}/Apps"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${RAYLIB_TARGET_NAME}>
                    "${SC4_INSTALL_DIR}/Apps/"
                    COMMENT "Deploying raylib.dll to Apps folder"
            )
        endif()
        add_custom_command(TARGET imgui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_INSTALL_DIR}/Apps"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:imgui>
                "${SC4_INSTALL_DIR}/Apps/"
                COMMENT "Deploying imgui.dll to Apps folder"
        )

        add_custom_command(TARGET SC4ImGuiService POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiService>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiService.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSample.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiSampleCity POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSampleCity>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSampleCity.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiSampleDemo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiSampleDemo>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiSampleDemo.dll to Plugins folder"
        )

        add_custom_command(TARGET SC4ImGuiTextureSample POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SC4ImGuiTextureSample>
                "${SC4_PLUGINS_DIR}/"
                COMMENT "Deploying SC4ImGuiTextureSample.dll to Plugins folder"
        )

        message(STATUS "Deployment targets configured:")
        message(STATUS "  - rlimgui.dll -> ${SC4_INSTALL_DIR}/Apps/")
        message(STATUS "  - raylib.dll -> ${SC4_INSTALL_DIR}/Apps/")
        message(STATUS "  - imgui.dll -> ${SC4_INSTALL_DIR}/Apps/")
        message(STATUS "  - SC4ImGuiService.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSample.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleCity.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiSampleDemo.dll -> ${SC4_PLUGINS_DIR}/")
        message(STATUS "  - SC4ImGuiTextureSample.dll -> ${SC4_PLUGINS_DIR}/")
    else()
        message(WARNING "SC4 installation not found. Manual deployment required.")
    endif()
endif()

# Install targets
