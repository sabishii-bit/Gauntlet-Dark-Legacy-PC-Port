#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;
layout(set = 1, binding = 0) uniform sampler2D uSecondTexture; // white with no second stage

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 params;
    vec4 scale;
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec2 vUv2;

layout(location = 0) out vec4 outColor;

vec4 blendFrames(vec4 current, vec4 next, float fraction) {
    if (fraction >= 1.0) {
        return next;
    }
    // Interpolate coverage-weighted colour to avoid dark halos or hidden RGB bleeding
    // from transparent texels. The normal blend pipeline expects straight alpha.
    float alpha = mix(current.a, next.a, fraction);
    vec3 premultiplied = mix(current.rgb * current.a, next.rgb * next.a, fraction);
    return vec4(alpha > 0.0 ? premultiplied / alpha : vec3(0.0), alpha);
}

void main() {
    outColor = texture(uTexture, vUv) * vColor;
    if (pc.scale.z > 0.5) {
        // Keep-alpha alternate-texture mode: the original skin supplies coverage, not colour.
        // The alpha comparison is against K0.a = 2, followed by alternate alpha * vertex alpha.
        bool covered = outColor.a > 2.0 / 255.0;
        outColor = texture(uSecondTexture, vUv) * vColor;
        if (!covered) {
            discard;
        }
    } else if (pc.scale.z < 0.0) {
        outColor = blendFrames(texture(uTexture, vUv), texture(uSecondTexture, vUv),
                               -pc.scale.z) * vColor;
    } else {
        // The lightmap's alpha carries its intensity, like the console's second texture stage.
        outColor.rgb *= texture(uSecondTexture, vUv2).a;
    }
    // Texels under the alpha test neither show nor write depth, like the console's compare.
    if (outColor.a < pc.params.z) {
        discard;
    }
    // What a draw is darkened by, as when the level's ambient light is pulled down.
    outColor.rgb = clamp(outColor.rgb * pc.scale.w, 0.0, 1.0);
    outColor.rgb *= 1.0 - pc.params.w;
}
