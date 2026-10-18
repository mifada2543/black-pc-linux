// The fragment shader of the headless probe: the colour the vertex shader passed through.
//
// No sampler here on purpose. A Vulkan shader that samples has to have its descriptors bound, and on this
// machine a 32-bit process cannot bind any: vkAllocateDescriptorSets segfaults inside llvmpipe, the Intel
// driver segfaults on submit, and the 32-bit loader does not expose push descriptors (all three measured
// from programs written from scratch, not from the loader). So the always-testable shader is the one that
// needs none of that, and the textured variant lives in probe_tex.frag, which the suite runs in a child
// process so that a driver that dies takes the child with it instead of the suite.
#version 450

layout(location = 0) in vec4 v_color;
// Location 0, and it has to be: the pipeline this shader is used with declares one colour attachment
// (cb.attachmentCount = 1 in gfx_vulkan.cpp), so location 0 is the only output that has somewhere to go.
// At location 1 every fragment was written to an attachment that does not exist: the clear survived, the
// draw reported success, no validation error fired, and not one pixel was ever produced. See NOTES.md.
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color;
}
