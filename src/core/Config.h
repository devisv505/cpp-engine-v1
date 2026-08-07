#pragma once

#include <string>

namespace engine {

struct WindowConfig {
    std::string title      = "Engine505";
    int         width      = 1280;
    int         height     = 720;
    bool        fullscreen = false;
    bool        resizable  = true;
    bool        vsync      = true;  // consumed at swapchain creation, once rendering exists
};

// Loads window settings from a JSON file. Never throws: a missing file,
// a parse error, or a wrong-typed value falls back to the defaults above.
WindowConfig LoadWindowConfig(const std::string& path);

} // namespace engine
