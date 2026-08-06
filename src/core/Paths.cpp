#include "core/Paths.h"

#include <fstream>
#include <sstream>

#include <SDL3/SDL_filesystem.h>

#include "core/Log.h"

namespace engine {

namespace {

bool FileExists(const std::string& path)
{
    std::ifstream file(path);
    return file.good();
}

} // namespace

std::string ResolveDataPath(const std::string& relativePath)
{
#if !defined(NDEBUG) && defined(ENGINE_SOURCE_DIR)
    // Debug builds read straight from the repository when the file is there.
    const std::string sourcePath = std::string(ENGINE_SOURCE_DIR) + "/" + relativePath;
    if (FileExists(sourcePath)) {
        return sourcePath;
    }
#endif
    if (const char* basePath = SDL_GetBasePath()) {
        return std::string(basePath) + relativePath;
    }
    return relativePath;
}

std::string LoadTextFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Cannot open '%s'", path.c_str());
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace engine
