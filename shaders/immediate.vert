#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;

layout(push_constant) uniform PushConstants {
    mat4 transform;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

void main() {
    gl_Position = pc.transform * vec4(inPosition, 1.0);
    vColor = inColor;
    vUv = inUv;
}
