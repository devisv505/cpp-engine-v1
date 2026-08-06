-- World configuration. The engine reads these globals after running the file:
--
--   tiles  — tile type definitions:
--            { name = "grass", color = {r,g,b,a} }                  solid color
--            { name = "sand",  texture = "textures/sand.png" }      texture
--            { name = "moss",  texture = "...", tint = {r,g,b,a} }  texture x tint
--            walkable = false  marks a tile as blocking (for future collision)
--   map    — generation: pattern ("checkerboard" | "random" | "solid"),
--            width/height (tiles), cell_size, seed, weights,
--            tiles = {"name", ...} or colors = {{r,g,b,a}, ...} shorthand
--            (colors auto-define tile types named color_1, color_2, ...)
--   editor — camera: pan_speed, zoom_min, zoom_max,
--            keys = { up = "W", down = "S", left = "A", right = "D" }
--
-- Map generation and rendering run in C++; this file only configures them.
-- Press F5 in the engine to re-run it (regenerates the map).

engine.log("Configuring the world")

-- The map: a two-color dark-grey checkerboard.
map = {
    pattern = "checkerboard",

    colors = {
        { 0.16, 0.16, 0.16, 1.0 },
        { 0.25, 0.25, 0.25, 1.0 }
    },

    width  = 256,
    height = 256,
}

editor = {
    pan_speed = 900,
    zoom_min  = 0.125,
    zoom_max  = 8.0,
    keys = { up = "W", down = "S", left = "A", right = "D" },
}
