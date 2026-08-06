#include "world/TileAtlas.h"

#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb/stb_image.h>

#include "core/Log.h"
#include "core/Paths.h"

namespace engine {

namespace {

struct LoadedImage {
    TileId               id = 0;
    int                  width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA8
};

} // namespace

TileRenderData BuildTileRenderData(TileRegistry& registry)
{
    TileRenderData data;

    // Load every referenced image up front; failures degrade to solid color.
    std::vector<LoadedImage> images;
    for (TileId id = 0; id < registry.Count(); ++id) {
        TilePrototype& tile = registry.GetMutable(id);
        if (tile.texturePath.empty()) {
            continue;
        }
        const std::string path = ResolveDataPath(tile.texturePath);

        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!pixels) {
            LOG_WARN("Tile '%s': cannot load texture '%s' (%s), using solid color",
                     tile.name.c_str(), path.c_str(), stbi_failure_reason());
            tile.texturePath.clear();
            continue;
        }

        LoadedImage image;
        image.id     = id;
        image.width  = width;
        image.height = height;
        image.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        images.push_back(std::move(image));
    }

    if (images.empty()) {
        // Single white pixel: shaders can sample unconditionally.
        data.atlasPixels = {255, 255, 255, 255};
        data.atlasWidth  = 1;
        data.atlasHeight = 1;
    } else {
        // Shelf packing: rows of images sorted by height, atlas width fixed
        // to the smallest power of two that fits the widest image or 1024.
        std::sort(images.begin(), images.end(),
                  [](const LoadedImage& a, const LoadedImage& b) { return a.height > b.height; });

        int atlasWidth = 1024;
        for (const LoadedImage& image : images) {
            while (atlasWidth < image.width) {
                atlasWidth *= 2;
            }
        }

        int penX = 0, penY = 0, rowHeight = 0;
        struct Placement { TileId id; int x, y, w, h; };
        std::vector<Placement> placements;
        for (const LoadedImage& image : images) {
            if (penX + image.width > atlasWidth) {
                penX = 0;
                penY += rowHeight;
                rowHeight = 0;
            }
            placements.push_back({image.id, penX, penY, image.width, image.height});
            penX += image.width;
            rowHeight = std::max(rowHeight, image.height);
        }
        const int atlasHeight = penY + rowHeight;

        data.atlasWidth  = atlasWidth;
        data.atlasHeight = atlasHeight;
        data.atlasPixels.assign(static_cast<size_t>(atlasWidth) * atlasHeight * 4, 0);

        for (size_t i = 0; i < images.size(); ++i) {
            const LoadedImage& image = images[i];
            const Placement&   at    = placements[i];
            for (int row = 0; row < image.height; ++row) {
                std::copy_n(&image.pixels[static_cast<size_t>(row) * image.width * 4],
                            static_cast<size_t>(image.width) * 4,
                            &data.atlasPixels[(static_cast<size_t>(at.y + row) * atlasWidth +
                                               at.x) * 4]);
            }

            TilePrototype& tile = registry.GetMutable(image.id);
            tile.u0 = static_cast<float>(at.x) / atlasWidth;
            tile.v0 = static_cast<float>(at.y) / atlasHeight;
            tile.u1 = static_cast<float>(at.x + at.w) / atlasWidth;
            tile.v1 = static_cast<float>(at.y + at.h) / atlasHeight;
            tile.hasTexture = true;
        }
        LOG_INFO("Tile atlas packed: %d texture(s) into %dx%d",
                 static_cast<int>(images.size()), atlasWidth, atlasHeight);
    }

    // Palette rows: see TileRenderData docs for the layout contract.
    data.palettePixels.assign(static_cast<size_t>(TileRegistry::kMaxTileTypes) * 2 * 4, 0.0f);
    for (TileId id = 0; id < registry.Count(); ++id) {
        const TilePrototype& tile = registry.Get(id);
        float* colorRow = &data.palettePixels[static_cast<size_t>(id) * 4];
        colorRow[0] = tile.color.r;
        colorRow[1] = tile.color.g;
        colorRow[2] = tile.color.b;
        colorRow[3] = tile.hasTexture ? 1.0f : 0.0f;

        float* uvRow = &data.palettePixels[(static_cast<size_t>(TileRegistry::kMaxTileTypes) + id) * 4];
        uvRow[0] = tile.u0;
        uvRow[1] = tile.v0;
        uvRow[2] = tile.u1;
        uvRow[3] = tile.v1;
    }

    return data;
}

} // namespace engine
