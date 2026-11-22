#version 460
// A pixel shader that writes SV_Depth, written to the translator's runtime contract (docs/design.md, "User data"),
// standing in for a translated one in d3d12_depth_clamp_test. The depth is s1, a float in the root constants.

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    gl_FragDepth = uintBitsToFloat(pc.s[1]);
}
