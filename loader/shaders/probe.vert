// The vertex shader of the headless probe: a position in clip space and a colour per vertex, which is the
// smallest thing that proves a pipeline draws and that what came out is the triangle that went in.
#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 v_color;

void main()
{
    v_color = in_color;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
