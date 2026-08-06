-- Scene definition. The engine runs this once at startup; press F5 while the
-- engine is running to re-run it and see changes immediately.
--
-- Available API:
--   engine.log(message)
--   engine.set_clear_color(r, g, b [, a])
--   engine.add_quad{ x = , y = , w = , h = , color = {r, g, b [, a]} }
--   engine.window_size() -> width, height   (framebuffer pixels)
--
-- Coordinates are pixels with the origin at the top-left corner.

engine.log("Building the scene")

engine.set_clear_color(0.09, 0.10, 0.13)

local width, height = engine.window_size()

-- Title bar across the top.
engine.add_quad{
    x = 0, y = 0, w = width, h = height * 0.12,
    color = { 0.16, 0.18, 0.24 },
}

-- A row of swatches, evenly spaced and centred vertically.
local palette = {
    { 0.91, 0.30, 0.24 },
    { 0.95, 0.61, 0.07 },
    { 0.18, 0.80, 0.44 },
    { 0.20, 0.60, 0.86 },
    { 0.61, 0.35, 0.71 },
}

local margin = width * 0.06
local gap = width * 0.02
local count = #palette
local quadWidth = (width - 2 * margin - gap * (count - 1)) / count
local quadHeight = height * 0.34

for i, color in ipairs(palette) do
    engine.add_quad{
        x = margin + (i - 1) * (quadWidth + gap),
        y = (height - quadHeight) * 0.55,
        w = quadWidth,
        h = quadHeight,
        color = color,
    }
end

engine.log(string.format("Scene ready: %d swatches at %dx%d", count, width, height))
