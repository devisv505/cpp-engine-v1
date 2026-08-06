cmake_minimum_required(VERSION 3.24)

# An x86_64 CMake running under Rosetta makes clang default to x86_64, which
# then fails to link against arm64 Homebrew libraries. Pin the target to the
# real hardware architecture unless the caller overrides it. Must run before
# project() to affect compiler detection.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
  execute_process(COMMAND sysctl -n hw.optional.arm64
                  OUTPUT_VARIABLE _host_is_arm64
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  if(_host_is_arm64 STREQUAL "1")
    set(CMAKE_OSX_ARCHITECTURES arm64)
  endif()
endif()

project(CppEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL3: prefer an installed package, fall back to building it from source.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  message(STATUS "System SDL3 not found; fetching SDL 3.4.12 from source")
  include(FetchContent)
  set(SDL_TEST_LIBRARY OFF)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.12
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(SDL3)
endif()

add_executable(engine
  src/main.cpp
  src/core/Config.cpp
  src/core/Window.cpp
  src/core/Application.cpp
  src/renderer/RendererFactory.cpp)

target_include_directories(engine PRIVATE src third_party)
target_link_libraries(engine PRIVATE SDL3::SDL3)

# Exactly one renderer backend is compiled per platform.
# APPLE must be checked before UNIX: macOS matches both.
if(WIN32)
  target_sources(engine PRIVATE src/renderer/dx12/D3D12Renderer.cpp)
  target_compile_definitions(engine PRIVATE ENGINE_BACKEND_D3D12=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(engine PRIVATE d3d12 dxgi dxguid)
elseif(APPLE)
  enable_language(OBJCXX)
  target_sources(engine PRIVATE src/renderer/metal/MetalRenderer.mm)
  set_source_files_properties(src/renderer/metal/MetalRenderer.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(engine PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
  target_compile_definitions(engine PRIVATE ENGINE_BACKEND_METAL=1)
  find_library(FW_METAL Metal REQUIRED)
  find_library(FW_QUARTZCORE QuartzCore REQUIRED)
  find_library(FW_FOUNDATION Foundation REQUIRED)
  target_link_libraries(engine PRIVATE ${FW_METAL} ${FW_QUARTZCORE} ${FW_FOUNDATION})
elseif(UNIX)
  find_package(Vulkan REQUIRED)
  target_sources(engine PRIVATE src/renderer/vulkan/VulkanRenderer.cpp)
  target_compile_definitions(engine PRIVATE ENGINE_BACKEND_VULKAN=1)
  target_link_libraries(engine PRIVATE Vulkan::Vulkan)
endif()

# Ship the runtime config next to the binary; the app resolves it through
# SDL_GetBasePath() so it works from any launch directory.
add_custom_command(TARGET engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:engine>/config"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/config/window.json"
          "$<TARGET_FILE_DIR:engine>/config/window.json"
  COMMENT "Copying config/window.json next to executable")
