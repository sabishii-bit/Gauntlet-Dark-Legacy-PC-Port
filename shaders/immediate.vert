#version 450
#extension GL_GOOGLE_include_directive : require
#include "platform.glsl"
#include "immediate_constants.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inUv2;

VARYING(0) out vec4 vColor;
VARYING(1) out vec2 vUv;
VARYING(2) out vec2 vUv2;
VARYING(3) noperspective out float vFog;

void main() {
    gl_Position = pc.transform * vec4(inPosition, 1.0);
    vFog = gl_Position.w > 0.0
        ? clamp(pc.effectData1.w + pc.effectData0.w / gl_Position.w, 0.0, 1.0) : 0.0;
#ifdef GDL_OPENGL
    // Clip depth runs from 0 to w for Vulkan's range; OpenGL's runs from -w to w.
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;
#endif
    vColor = inColor;
    vUv = inUv * pc.scale.xy + pc.params.xy;
    vUv2 = inUv2;
}
