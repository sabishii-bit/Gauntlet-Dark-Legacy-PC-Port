#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inUv2;

// The transform, then the texture coordinate offset (xy) and the alpha test (z), then the
// texture coordinate scale (xy).
layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 params;
    vec4 scale;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec2 vUv2;

void main() {
    gl_Position = pc.transform * vec4(inPosition, 1.0);
    vColor = inColor;
    vUv = inUv * pc.scale.xy + pc.params.xy;
    vUv2 = inUv2;
}
