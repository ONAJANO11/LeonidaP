#version 460
// A vertex shader written to the translator's runtime contract, standing in for a translated one that writes
// SV_ViewportArrayIndex (a pos1 misc export; docs/design.md, "Inputs and outputs"), for d3d12_viewport_array_test.
// Inputs as vs_draw.vert: COLOR at location 0, POSITION at location 1. Instance i goes to viewport i, and its
// colour comes from a palette, so every viewport shows which instance landed in it.
#extension GL_ARB_shader_viewport_layer_array : require

layout(location = 0) in vec4 color;     // COLOR0, input register 0 (unused here)
layout(location = 1) in vec4 position;  // POSITION0, input register 1

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    // ps_draw.frag writes (red * tint.x, tint.y, blue * tint.z): with tint (1, 0, 1), red and blue survive.
    const vec4 palette[4] = vec4[4](vec4(1, 0, 0, 1), vec4(0, 0, 1, 1), vec4(1, 0, 1, 1), vec4(0.5, 0, 0.5, 1));
    gl_Position = vec4(position.xy, 0.0, 1.0);
    gl_ViewportIndex = gl_InstanceIndex;
    out_color = palette[gl_InstanceIndex & 3] + 0.0 * color;
}
