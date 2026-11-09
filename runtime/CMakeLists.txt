cmake_minimum_required(VERSION 3.20)
project(shrouded_engine_tweaks VERSION 0.2.0 LANGUAGES C CXX)
enable_language(ASM_MASM)

# The version, for the code (RUNTIME_VERSION, GetVersion() in Lua) and powrprof.dll's Properties.
configure_file(version.h.in version.h @ONLY)
configure_file(version.rc.in version.rc @ONLY)

# Static CRT: the proxy must not depend on a runtime the game folder lacks.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# AddressSanitizer, for running the self-tests only (docs/building.md). Never ship this DLL.
option(SET_ASAN "AddressSanitizer build for the self-tests" OFF)
if(SET_ASAN)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address> /Zi)  # MASM has no sanitizer
    add_link_options(/DEBUG /INCREMENTAL:NO)
endif()

# Lua is compiled as C++ so its errors are C++ exceptions: they unwind our C++ frames properly.
file(GLOB LUA_SRC CONFIGURE_DEPENDS ${TP}/lua/*.c)
set_source_files_properties(${LUA_SRC} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${TP}/lua)

add_library(minhook STATIC ${TP}/minhook/src/buffer.c ${TP}/minhook/src/hook.c ${TP}/minhook/src/trampoline.c
            ${TP}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

# Vulkan headers only: no vulkan-1.lib. A layer must call down its chain, never the loader's exports.
add_library(vulkan_headers INTERFACE)
target_include_directories(vulkan_headers INTERFACE ${TP}/vulkan/include)
target_compile_definitions(vulkan_headers INTERFACE VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)

add_library(imgui STATIC ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
            ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_demo.cpp ${TP}/imgui/backends/imgui_impl_win32.cpp
            ${TP}/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui ${TP}/imgui/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_link_libraries(imgui PUBLIC vulkan_headers)

# Game access and the Lua API, shared by the DLL and the offline self-test.
add_library(core STATIC game.cpp lua_api.cpp lua_imgui.cpp lua_json.cpp hooks.cpp hooks_x64.asm rules.cpp contracts.cpp
            report.cpp writelog.cpp modsettings.cpp inspect.cpp)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_BINARY_DIR})  # version.h
target_link_libraries(core PUBLIC lua imgui minhook)
target_compile_features(core PUBLIC cxx_std_20)
target_compile_options(core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->)

add_library(powrprof SHARED main.cpp overlay_vk.cpp overlay_ui.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_link_libraries(powrprof PRIVATE core minhook imgui version dxgi)
target_compile_options(powrprof PRIVATE /W4 /permissive-)

add_executable(runtime_selftest selftest.cpp hooks_test.cpp hooks_test_x64.asm)
target_link_libraries(runtime_selftest PRIVATE core)
target_compile_options(runtime_selftest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->)

# Drives a real swapchain through the system Vulkan loader with the layer enabled. Needs a GPU, not the game.
add_executable(overlay_selftest overlay_test.cpp)
target_link_libraries(overlay_selftest PRIVATE vulkan_headers)
target_compile_features(overlay_selftest PRIVATE cxx_std_20)
target_compile_options(overlay_selftest PRIVATE /W4 /permissive-)
