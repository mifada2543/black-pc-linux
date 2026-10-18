// The textured variant of the probe's fragment shader: the vertex colour multiplied by a texture sample.
//
// Separate from probe.frag because of the descriptor argument above: this shader needs set 0, and a Vulkan
// descriptor that has never been written is undefined behaviour -- which on this machine's 32-bit llvmpipe
// is a crash rather than a wrong pixel. The suite therefore runs this path in a forked child and reports
// what happened, instead of finding out the hard way.
#version 450

layout(location = 0) in vec4 v_color;
// Location 0 for the same reason as probe.frag: one colour attachment, so location 0 is the only output
// that reaches a framebuffer. See the note there and NOTES.md.
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

void main()
{
    out_color = v_color * vec4(texture(u_tex, vec2(0.5)).rgb, 1.0);
}
