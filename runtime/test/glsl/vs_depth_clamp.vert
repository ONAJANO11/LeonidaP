#version 460
// A vertex shader with no inputs, written to the translator's runtime contract (docs/design.md, "User data"),
// standing in for a translated one in d3d12_depth_clamp_test. One triangle covers the whole viewport at clip-space
// depth s0 (a float; w is 1, so s0 is also z/w).

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    // (-1,-1), (-1,3), (3,-1): the triangle of vs_fullscreen.vert.
    const vec2 corner = vec2(float(gl_VertexIndex & 2), float((gl_VertexIndex << 1) & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, uintBitsToFloat(pc.s[0]), 1.0);
}
