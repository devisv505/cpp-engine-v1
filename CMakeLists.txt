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

project(CppEngine VERSION 0.1.0 LANGUAGES C CXX)

# OBJCXX must be enabled before SDL is added as a subdirectory (FetchContent
# path): SDL enables OBJC in its own scope, and enabling OBJCXX afterwards
# fails at generate time with missing CMAKE_OBJCXX_* internal variables.
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL3: prefer an installed package, fall back to building it from source.
# ENGINE_FETCH_SDL=ON skips the system package entirely; CI and release builds
# use it so binaries always link a known SDL built statically (self-contained,
# no dylib/dll/so dependency for people downloading a release).
option(ENGINE_FETCH_SDL "Always build SDL3 from source instead of using an installed package" OFF)

if(NOT ENGINE_FETCH_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  message(STATUS "Building SDL 3.4.12 from source (static)")
  include(FetchContent)
  set(SDL_TEST_LIBRARY OFF)
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.12
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(SDL3)
endif()

# ---- Lua 5.4.8 (vendored source, built as a static library) ----
# The vendored copy is frozen, so globbing it cannot miss newly added files.
file(GLOB ENGINE_LUA_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/lua/*.c")
add_library(lua STATIC ${ENGINE_LUA_SOURCES})
target_include_directories(lua PUBLIC "${CMAKE_SOURCE_DIR}/third_party/lua")
set_target_properties(lua PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
  target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
  target_link_libraries(lua PRIVATE dl m)
endif()

add_executable(engine
        src/main.cpp
        src/core/Config.cpp
        src/core/Window.cpp
        src/core/Application.cpp
        src/scripting/ScriptHost.cpp
        src/world/TileRegistry.cpp
        src/world/TileMap.cpp
        src/world/MapPatterns.cpp
        src/world/MapIO.cpp
        src/world/TileAtlas.cpp
        src/editor/EditorCamera.cpp
        src/renderer/RendererFactory.cpp
        src/core/input/Input.h
        src/core/math/Vector2.h
        src/core/math/Vector2Int.h)

target_include_directories(engine PRIVATE src third_party)
target_link_libraries(engine PRIVATE SDL3::SDL3 lua)

# Exactly one renderer backend is compiled per platform.
# APPLE must be checked before UNIX: macOS matches both.
if(WIN32)
  target_sources(engine PRIVATE src/renderer/dx12/D3D12Renderer.cpp)
  target_compile_definitions(engine PRIVATE ENGINE_BACKEND_D3D12=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  # d3dcompiler provides D3DCompile, used to build the quad shaders at runtime.
  target_link_libraries(engine PRIVATE d3d12 dxgi dxguid d3dcompiler)
elseif(APPLE)
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

  # Vulkan needs SPIR-V, so the GLSL sources are compiled at build time and
  # loaded from disk at runtime. Metal and D3D12 compile their shaders in-process
  # and need no build step.
  find_program(ENGINE_GLSL_COMPILER NAMES glslc glslangValidator
               HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
  if(NOT ENGINE_GLSL_COMPILER)
    message(FATAL_ERROR
            "No GLSL compiler found. Install glslang-tools (glslangValidator) "
            "or the Vulkan SDK (glslc).")
  endif()
  if(ENGINE_GLSL_COMPILER MATCHES "glslc")
    set(ENGINE_GLSL_ARGS "")   # glslc infers the stage from the file extension
  else()
    set(ENGINE_GLSL_ARGS -V)
  endif()

  add_custom_target(engine_shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:engine>/shaders"
    COMMAND ${ENGINE_GLSL_COMPILER} ${ENGINE_GLSL_ARGS}
            "${CMAKE_SOURCE_DIR}/shaders/quad.vert"
            -o "$<TARGET_FILE_DIR:engine>/shaders/quad.vert.spv"
    COMMAND ${ENGINE_GLSL_COMPILER} ${ENGINE_GLSL_ARGS}
            "${CMAKE_SOURCE_DIR}/shaders/quad.frag"
            -o "$<TARGET_FILE_DIR:engine>/shaders/quad.frag.spv"
    COMMAND ${ENGINE_GLSL_COMPILER} ${ENGINE_GLSL_ARGS}
            "${CMAKE_SOURCE_DIR}/shaders/tile.vert"
            -o "$<TARGET_FILE_DIR:engine>/shaders/tile.vert.spv"
    COMMAND ${ENGINE_GLSL_COMPILER} ${ENGINE_GLSL_ARGS}
            "${CMAKE_SOURCE_DIR}/shaders/tile.frag"
            -o "$<TARGET_FILE_DIR:engine>/shaders/tile.frag.spv"
    COMMENT "Compiling GLSL shaders to SPIR-V")
  add_dependencies(engine engine_shaders)
endif()

# Ship runtime data next to the binary; the app resolves it through
# SDL_GetBasePath() so it works from any launch directory. The engine target
# DEPENDS on this (not the other way around) so that building or running
# `engine` from an IDE always restages the data — and it runs on every build,
# so editing a script reaches the build directory without relinking.
add_custom_target(engine_data
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:engine>/config"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/config/window.json"
          "$<TARGET_FILE_DIR:engine>/config/window.json"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/scripts"
          "$<TARGET_FILE_DIR:engine>/scripts"
  COMMENT "Staging runtime data next to executable")
add_dependencies(engine engine_data)
