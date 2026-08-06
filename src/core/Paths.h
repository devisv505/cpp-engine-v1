#pragma once

#include <string>

namespace engine {

// Resolves a runtime data file (scripts, config, shaders).
//
// Development builds prefer the file in the source tree, so editing a script or
// a shader and relaunching picks up the change without rebuilding. Release
// builds always use the copy staged next to the executable.
std::string ResolveDataPath(const std::string& relativePath);

// Reads a whole text file. Returns an empty string and logs on failure.
std::string LoadTextFile(const std::string& path);

} // namespace engine
