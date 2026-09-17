#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;
layout(set = 1, binding = 0) uniform sampler2D uLightmap; // white when a draw has none

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 params;
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec2 vUv2;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture, vUv) * vColor;
    // Texels under the alpha test neither show nor write depth, like the console's compare.
    if (outColor.a < pc.params.z) {
        discard;
    }
    // The lightmap's alpha carries its intensity, like the console's second texture stage.
    outColor.rgb *= texture(uLightmap, vUv2).a;
}
