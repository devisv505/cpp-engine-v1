#include "core/Config.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "core/Log.h"

namespace engine {

WindowConfig LoadWindowConfig(const std::string& path)
{
    WindowConfig cfg;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Config file '%s' not found, using defaults", path.c_str());
        return cfg;
    }

    const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        LOG_WARN("Config file '%s' is not valid JSON, using defaults", path.c_str());
        return cfg;
    }

    try {
        const auto window = doc.value("window", nlohmann::json::object());
        cfg.title      = window.value("title", cfg.title);
        cfg.width      = window.value("width", cfg.width);
        cfg.height     = window.value("height", cfg.height);
        cfg.fullscreen = window.value("fullscreen", cfg.fullscreen);
        cfg.resizable  = window.value("resizable", cfg.resizable);
        cfg.vsync      = window.value("vsync", cfg.vsync);
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("Config file '%s' has wrong-typed values (%s), keeping defaults for those",
                 path.c_str(), e.what());
    }

    if (cfg.width < 1 || cfg.height < 1) {
        LOG_WARN("Config window size %dx%d is invalid, clamping to at least 1x1",
                 cfg.width, cfg.height);
        cfg.width  = std::max(cfg.width, 1);
        cfg.height = std::max(cfg.height, 1);
    }

    LOG_INFO("Window config from %s: \"%s\" %dx%d fullscreen=%s resizable=%s vsync=%s",
             path.c_str(), cfg.title.c_str(), cfg.width, cfg.height,
             cfg.fullscreen ? "true" : "false",
             cfg.resizable ? "true" : "false",
             cfg.vsync ? "true" : "false");
    return cfg;
}

} // namespace engine
