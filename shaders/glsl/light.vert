#version 450

// One fullscreen triangle built from the vertex index alone (same trick as
// tile.vert): every light is a single 3-vertex draw and light.frag does all the
// work per fragment. No inputs, no outputs beyond the position.
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
