#include "world/MapIO.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/Log.h"

namespace engine {

namespace {

constexpr uint32_t kMagic   = 0x50414D43u;  // "CMAP" little-endian
constexpr uint32_t kVersion = 1;

template <typename T>
void WritePod(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool ReadPod(std::ifstream& in, T& value)
{
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return in.good();
}

} // namespace

bool SaveMap(const TileMap& map, const TileRegistry& registry, const std::string& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("SaveMap: cannot open '%s' for writing", path.c_str());
        return false;
    }

    WritePod(out, kMagic);
    WritePod(out, kVersion);
    WritePod(out, static_cast<int32_t>(map.Width()));
    WritePod(out, static_cast<int32_t>(map.Height()));

    const uint32_t nameCount = registry.Count();
    WritePod(out, nameCount);
    for (uint32_t id = 0; id < nameCount; ++id) {
        const std::string& name = registry.Get(static_cast<TileId>(id)).name;
        WritePod(out, static_cast<uint32_t>(name.size()));
        out.write(name.data(), static_cast<std::streamsize>(name.size()));
    }

    out.write(reinterpret_cast<const char*>(map.Data()),
              static_cast<std::streamsize>(sizeof(uint16_t)) * map.Width() * map.Height());

    if (!out.good()) {
        LOG_ERROR("SaveMap: write to '%s' failed", path.c_str());
        return false;
    }
    LOG_INFO("Saved map %dx%d to %s", map.Width(), map.Height(), path.c_str());
    return true;
}

bool LoadMap(TileMap& map, const TileRegistry& registry, const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        LOG_ERROR("LoadMap: cannot open '%s'", path.c_str());
        return false;
    }

    uint32_t magic = 0, version = 0;
    int32_t  width = 0, height = 0;
    if (!ReadPod(in, magic) || magic != kMagic) {
        LOG_ERROR("LoadMap: '%s' is not a CMAP file", path.c_str());
        return false;
    }
    if (!ReadPod(in, version) || version != kVersion) {
        LOG_ERROR("LoadMap: '%s' has unsupported version %u", path.c_str(), version);
        return false;
    }
    if (!ReadPod(in, width) || !ReadPod(in, height) ||
        width < 1 || height < 1 ||
        width > TileMap::kMaxDimension || height > TileMap::kMaxDimension) {
        LOG_ERROR("LoadMap: '%s' has invalid dimensions", path.c_str());
        return false;
    }

    uint32_t nameCount = 0;
    if (!ReadPod(in, nameCount) || nameCount > TileRegistry::kMaxTileTypes) {
        LOG_ERROR("LoadMap: '%s' has invalid tile-name table", path.c_str());
        return false;
    }

    // Saved id -> current id, resolved by name.
    std::vector<TileId> remap(nameCount, 0);
    uint32_t            missing = 0;
    for (uint32_t i = 0; i < nameCount; ++i) {
        uint32_t length = 0;
        if (!ReadPod(in, length) || length > 4096) {
            LOG_ERROR("LoadMap: '%s' is truncated in the name table", path.c_str());
            return false;
        }
        std::string name(length, '\0');
        in.read(name.data(), static_cast<std::streamsize>(length));
        if (!in.good()) {
            LOG_ERROR("LoadMap: '%s' is truncated in the name table", path.c_str());
            return false;
        }
        remap[i] = (name == "void") ? 0 : registry.FindByName(name);
        if (remap[i] == 0 && name != "void") {
            ++missing;
            LOG_WARN("LoadMap: tile type '%s' no longer exists, loading as void", name.c_str());
        }
    }

    if (!map.Create(width, height)) {
        return false;
    }
    in.read(reinterpret_cast<char*>(map.MutableData()),
            static_cast<std::streamsize>(sizeof(uint16_t)) * width * height);
    if (!in.good()) {
        LOG_ERROR("LoadMap: '%s' is truncated in tile data", path.c_str());
        map.Create(width, height);  // leave a defined (void-filled) map behind
        return false;
    }

    uint16_t* tiles = map.MutableData();
    const size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        tiles[i] = tiles[i] < nameCount ? remap[tiles[i]] : 0;
    }
    map.MarkAllDirty();

    LOG_INFO("Loaded map %dx%d from %s%s", width, height, path.c_str(),
             missing ? " (some tile types were missing)" : "");
    return true;
}

} // namespace engine
