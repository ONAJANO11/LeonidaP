#version 460
// A full-screen vertex shader that reads a constant from its own program, as translated shaders read code-embedded
// data through s_getpc_b64 (docs/design.md, "Code-embedded data"): its code base is in push-constant dwords 34-35,
// and the float at byte 12 of its FSSM blob becomes the red channel. Stands in for a translated VS in
// d3d12_draw_test, next to ps_code.frag, which reads its own code base from dwords 38-39 (SHADER-67).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Floats {
    float f[];
};

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

uvec2 add64(uvec2 a, uvec2 b) {
    uint carry;
    const uint low = uaddCarry(a.x, b.x, carry);
    return uvec2(low, a.y + b.y + carry);
}

void main() {
    const vec2 corner = vec2(float(gl_VertexIndex & 2), float((gl_VertexIndex << 1) & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
    Floats code = Floats(add64(pc.heap_base, pc.code_base));
    out_color = vec4(code.f[3], 0.0, 0.0, 1.0);
}
