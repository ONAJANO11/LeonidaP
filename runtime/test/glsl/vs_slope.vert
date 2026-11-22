#version 460
// A vertex shader written to the translator's runtime contract (docs/design.md, "Vertex fetch" and "User data"),
// standing in for a translated one in d3d12_depth_bias_test. POSITION0 is a three-component float at input register
// 0; the triangle's z rises linearly along x, so a depth bias moves the depth-test pass line across the columns.

layout(location = 0) in vec4 position;  // POSITION0, input register 0

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    gl_Position = position;
}
