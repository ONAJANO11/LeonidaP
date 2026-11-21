#version 460
// A vertex shader for d3d12_vrs_test, written to the translator's runtime contract: one triangle that covers the
// whole viewport, from SV_VertexID alone (no input layout, no vertex buffer).

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    const vec2 corner = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);
    gl_Position = vec4(corner, 0.0, 1.0);
}
