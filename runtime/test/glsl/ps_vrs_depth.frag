#version 460
// ps_vrs.frag that also writes depth (SV_Depth). D3D12 shades such a pixel shader per pixel whatever the shading
// rate; d3d12_vrs_test checks that the layer does too.

layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    target = vec4(floor(gl_FragCoord.xy) / 255.0, 0.0, 1.0);
    gl_FragDepth = 0.5;
}
