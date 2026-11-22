#version 460
// A pixel shader for d3d12_vrs_test: it writes the pixel it ran at, floor(SV_Position.xy), as unorm8 (k / 255 is
// stored as k exactly). A coarse pixel runs once, at the centre of its area, so every pixel it covers gets the same
// value: d3d12_vrs_test reads the shading rate back from where the values repeat.

layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    target = vec4(floor(gl_FragCoord.xy) / 255.0, 0.0, 1.0);
}
