#version 450

// One fullscreen triangle built from the vertex index alone: the three corners
// overshoot clip space so the viewport is fully covered, and clipping trims the
// rest. No inputs, no outputs beyond the position.
void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
