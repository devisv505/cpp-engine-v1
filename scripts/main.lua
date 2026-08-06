-- World configuration. The engine reads these globals after running the file.
-- Lua only DESCRIBES the environment; C++ does all rendering, occlusion
-- and raymarching.
--
--   map              — tile floor: pattern, colors/tiles, size
--   map_environment  — walls (colored quads that also occlude light) and
--                      lights. Positions and sizes are in TILES.
--   editor           — camera pan speed, zoom limits, keys
--
-- Press F5 in the engine to re-run this file.

engine.log("Configuring the world")

-- Dark floor so the light beams read clearly.
map = {
    pattern = "checkerboard",
    colors = {
        { 0.10, 0.10, 0.12, 1.0 },
        { 0.13, 0.13, 0.15, 1.0 }
    },
    width  = 64,
    height = 64,
}

local WALL   = { 0.25, 0.25, 0.28, 1.0 }
local PILLAR = { 0.30, 0.28, 0.26, 1.0 }

-- The room is written room-relative, then offset so its centre lands on the
-- map centre — which is where the camera starts.
local OX, OY = 14.0, 18.0
local function at(x, y) return { x + OX, y + OY } end

map_environment = {
    -- A room with openings, so beams are blocked in some places and stream
    -- through the gaps in others.
    walls = {
        -- Outer frame; the top wall is split to leave a doorway.
        { position = at(0.0,  0.0),  size = { 12.0, 0.6 },  color = WALL },
        { position = at(16.0, 0.0),  size = { 12.0, 0.6 },  color = WALL },
        { position = at(0.0,  21.0), size = { 28.0, 0.6 },  color = WALL },
        { position = at(0.0,  0.0),  size = { 0.6,  21.6 }, color = WALL },
        { position = at(27.4, 0.0),  size = { 0.6,  21.6 }, color = WALL },

        -- Interior pillars that cast visible shadow wedges.
        { position = at(8.0,  7.0),  size = { 1.2, 1.2 }, color = PILLAR },
        { position = at(18.0, 12.0), size = { 1.2, 1.2 }, color = PILLAR },

        -- A divider with a gap the searchlight shines through.
        { position = at(13.0, 4.0),  size = { 0.6, 6.0 }, color = WALL },
        { position = at(13.0, 14.0), size = { 0.6, 6.0 }, color = WALL },

        -- Decorative rail that does NOT occlude, proving the flag works.
        { position = at(3.0, 17.5), size = { 8.0, 0.3 },
          color = { 0.42, 0.34, 0.22, 1.0 }, blocks_light = false, collision = false },
    },

    lights = {
        -- Searchlight aimed right, through the divider gap.
        {
            position      = at(2.5, 10.5),
            direction     = { 1.0, 0.0 },
            mode          = "volumetric-cone",
            color         = { 1.00, 0.82, 0.55, 1.0 },
            intensity     = 2.0,
            distance      = 22.0,
            angle         = 35.0,
            edge_softness = 0.25,
        },
        -- Cool lamp angled down-right onto the first pillar.
        {
            position      = at(6.0, 2.0),
            direction     = { 0.6, 1.0 },
            mode          = "volumetric-cone",
            color         = { 0.45, 0.70, 1.00, 1.0 },
            intensity     = 1.6,
            distance      = 16.0,
            angle         = 45.0,
            edge_softness = 0.45,
        },
        -- Narrow hard-edged beam from the right, blocked by the divider.
        {
            position      = at(26.0, 16.0),
            direction     = { -1.0, -0.35 },
            mode          = "volumetric-cone",
            color         = { 1.00, 0.45, 0.35, 1.0 },
            intensity     = 1.8,
            distance      = 20.0,
            angle         = 18.0,
            edge_softness = 0.05,
        },
        -- The one important light using the screen-space god-ray mode.
--         {
--             position      = at(20.0, 5.0),
--             direction     = { 0.0, 1.0 },
--             mode          = "screen-space",
--             color         = { 1.00, 0.93, 0.70, 1.0 },
--             intensity     = 1.4,
--             distance      = 15.0,
--             angle         = 360.0,
--             edge_softness = 1.0,
--         },
    },
}

editor = {
    pan_speed = 900,
    zoom_min  = 0.125,
    zoom_max  = 8.0,
    keys = { up = "W", down = "S", left = "A", right = "D" },
}
