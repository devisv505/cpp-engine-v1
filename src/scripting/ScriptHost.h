#pragma once

#include <string>

struct lua_State;

namespace engine {

struct Scene2D;
class Window;

// Owns the Lua VM and exposes the engine API to scripts.
//
// A script describes the scene declaratively: it runs once at startup (and
// again on reload), calling into the `engine` table to set the clear color and
// add quads. Script errors are contained — they are logged and the engine
// keeps running with whatever the script managed to build.
class ScriptHost {
public:
    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
    ScriptHost() = default;

    bool Init(Scene2D& scene, const Window& window);
    void Shutdown();

    // Resets the scene and re-runs the script. Returns false if the script
    // failed to load or raised an error.
    bool RunFile(const std::string& path);

    // What the bound engine functions operate on; each one receives a pointer
    // to it as a closure upvalue.
    struct Context {
        Scene2D*      scene  = nullptr;
        const Window* window = nullptr;
    };

private:
    lua_State* m_L = nullptr;
    Context    m_context;
};

} // namespace engine
