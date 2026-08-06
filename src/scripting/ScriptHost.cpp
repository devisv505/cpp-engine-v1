#include "scripting/ScriptHost.h"

#include <lua.hpp>

#include "core/Log.h"
#include "core/Scene2D.h"
#include "core/Window.h"

namespace engine {

namespace {

// Every engine function carries its context as an upvalue, so no globals are
// needed and multiple VMs could coexist later.
ScriptHost::Context* GetContext(lua_State* L)
{
    return static_cast<ScriptHost::Context*>(lua_touserdata(L, lua_upvalueindex(1)));
}

float FieldNumber(lua_State* L, int table, const char* name, float fallback)
{
    lua_getfield(L, table, name);
    const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

float IndexNumber(lua_State* L, int table, int index, float fallback)
{
    lua_geti(L, table, index);
    const float value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

// Accepts either {0.9, 0.3, 0.2} or {r = 0.9, g = 0.3, b = 0.2}, alpha optional.
Color ReadColorTable(lua_State* L, int table)
{
    Color color;
    color.r = IndexNumber(L, table, 1, color.r);
    color.g = IndexNumber(L, table, 2, color.g);
    color.b = IndexNumber(L, table, 3, color.b);
    color.a = IndexNumber(L, table, 4, color.a);
    color.r = FieldNumber(L, table, "r", color.r);
    color.g = FieldNumber(L, table, "g", color.g);
    color.b = FieldNumber(L, table, "b", color.b);
    color.a = FieldNumber(L, table, "a", color.a);
    return color;
}

int Lua_Log(lua_State* L)
{
    LOG_INFO("[lua] %s", luaL_checkstring(L, 1));
    return 0;
}

// engine.set_clear_color(r, g, b [, a])
int Lua_SetClearColor(lua_State* L)
{
    Color color;
    color.r = static_cast<float>(luaL_checknumber(L, 1));
    color.g = static_cast<float>(luaL_checknumber(L, 2));
    color.b = static_cast<float>(luaL_checknumber(L, 3));
    color.a = static_cast<float>(luaL_optnumber(L, 4, 1.0));
    GetContext(L)->scene->clearColor = color;
    return 0;
}

// engine.add_quad{ x = , y = , w = , h = , color = {r, g, b} }
int Lua_AddQuad(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    Quad quad;
    quad.x = FieldNumber(L, 1, "x", 0.0f);
    quad.y = FieldNumber(L, 1, "y", 0.0f);
    quad.w = FieldNumber(L, 1, "w", 0.0f);
    quad.h = FieldNumber(L, 1, "h", 0.0f);

    lua_getfield(L, 1, "color");
    if (lua_istable(L, -1)) {
        quad.color = ReadColorTable(L, lua_absindex(L, -1));
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "add_quad: 'color' must be a table like {r, g, b}");
    }
    lua_pop(L, 1);

    if (quad.w <= 0.0f || quad.h <= 0.0f) {
        return luaL_error(L, "add_quad: 'w' and 'h' must be greater than zero");
    }

    GetContext(L)->scene->quads.push_back(quad);
    return 0;
}

// engine.window_size() -> width, height  (framebuffer pixels)
int Lua_WindowSize(lua_State* L)
{
    int width  = 0;
    int height = 0;
    GetContext(L)->window->GetPixelSize(width, height);
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

// Error handler for lua_pcall: turns the raised message into a full traceback.
int Traceback(lua_State* L)
{
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "(non-string error)", 1);
    return 1;
}

} // namespace

ScriptHost::~ScriptHost()
{
    Shutdown();
}

bool ScriptHost::Init(Scene2D& scene, const Window& window)
{
    m_context.scene  = &scene;
    m_context.window = &window;

    m_L = luaL_newstate();
    if (!m_L) {
        LOG_ERROR("[lua] Failed to create Lua state");
        return false;
    }

    // A deliberately limited standard library: no io, os, package or debug, so
    // scripts reach the engine only through the `engine` table.
    static const luaL_Reg kLibraries[] = {
        {LUA_GNAME,      luaopen_base},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
    };
    for (const luaL_Reg& lib : kLibraries) {
        luaL_requiref(m_L, lib.name, lib.func, 1);
        lua_pop(m_L, 1);
    }

    // Filesystem entry points from the base library are not part of the API.
    lua_pushnil(m_L); lua_setglobal(m_L, "dofile");
    lua_pushnil(m_L); lua_setglobal(m_L, "loadfile");

    static const luaL_Reg kEngineApi[] = {
        {"log",             Lua_Log},
        {"set_clear_color", Lua_SetClearColor},
        {"add_quad",        Lua_AddQuad},
        {"window_size",     Lua_WindowSize},
        {nullptr, nullptr},
    };

    lua_newtable(m_L);
    for (const luaL_Reg* entry = kEngineApi; entry->name; ++entry) {
        lua_pushlightuserdata(m_L, &m_context);
        lua_pushcclosure(m_L, entry->func, 1);
        lua_setfield(m_L, -2, entry->name);
    }
    lua_setglobal(m_L, "engine");

    LOG_INFO("[lua] %s initialized", LUA_RELEASE);
    return true;
}

bool ScriptHost::RunFile(const std::string& path)
{
    if (!m_L) {
        return false;
    }

    m_context.scene->Reset();

    lua_pushcfunction(m_L, Traceback);
    const int handler = lua_gettop(m_L);

    if (luaL_loadfile(m_L, path.c_str()) != LUA_OK) {
        LOG_ERROR("[lua] %s", lua_tostring(m_L, -1));
        lua_pop(m_L, 2);
        return false;
    }

    if (lua_pcall(m_L, 0, 0, handler) != LUA_OK) {
        LOG_ERROR("[lua] %s", lua_tostring(m_L, -1));
        lua_pop(m_L, 2);
        return false;
    }

    lua_pop(m_L, 1);
    LOG_INFO("[lua] Ran %s: %zu quad(s) defined",
             path.c_str(), m_context.scene->quads.size());
    return true;
}

void ScriptHost::Shutdown()
{
    if (m_L) {
        lua_close(m_L);
        m_L = nullptr;
    }
}

} // namespace engine
