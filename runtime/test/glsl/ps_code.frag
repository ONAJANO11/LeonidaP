#version 460
// A pixel shader that reads a constant from its own program (docs/design.md, "Code-embedded data"). A pixel
// shader's code base is in push-constant dwords 38-39, not 34-35 (SHADER-67), so the block is 160 bytes. The float
// at byte 16 of its FSSM blob becomes the green channel; red comes from vs_code.vert. Stands in for a translated PS
// in d3d12_draw_test.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Floats {
    float f[];
};

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;      // the VS's
    uvec2 mesh_output;    // --mesh-compute's output address
    uvec2 ps_code_base;
} pc;

uvec2 add64(uvec2 a, uvec2 b) {
    uint carry;
    const uint low = uaddCarry(a.x, b.x, carry);
    return uvec2(low, a.y + b.y + carry);
}

void main() {
    Floats code = Floats(add64(pc.heap_base, pc.ps_code_base));
    target = vec4(color.r, code.f[4], 0.0, 1.0);
}
