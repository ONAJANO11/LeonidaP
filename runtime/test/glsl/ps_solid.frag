#version 460
// A pixel shader written to the translator's runtime contract, standing in for a translated one in
// d3d12_depth_bias_test: a solid red fragment, so the depth test's verdict is the only thing the render target
// shows.

layout(location = 0) out vec4 target;

layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    target = vec4(1.0, 0.0, 0.0, 1.0);
}
