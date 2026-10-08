#version 450

layout(constant_id = 0) const bool alphaToCoverage = false;

layout(set = 0, binding = 0) uniform texture2D uImage;
layout(set = 1, binding = 0) uniform texture2D uSecondImage;
layout(set = 2, binding = 0) uniform sampler uSampler;
layout(set = 3, binding = 0) uniform sampler uSecondSampler;

#define uTexture sampler2D(uImage, uSampler)
#define uSecondTexture sampler2D(uSecondImage, uSecondSampler)

layout(push_constant) uniform PushConstants {
    mat4 transform;
    vec4 params;
    vec4 scale;
    vec4 effectData0;
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec2 vUv2;

layout(location = 0) out vec4 outColor;

vec4 cubicWeights(float f) {
    float g = 1.0 - f;
    return vec4(g * g * g, 3.0 * f * f * f - 6.0 * f * f + 4.0,
                -3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0, f * f * f) / 6.0;
}

vec4 sampleSprite(texture2D image, sampler filtering, vec2 uv) {
    vec4 original = texture(sampler2D(image, filtering), uv);
    if (pc.effectData0.x < 0.5) {
        return original;
    }
    vec2 size = vec2(textureSize(sampler2D(image, filtering), 0));
    float footprint = max(length(dFdx(uv) * size), length(dFdy(uv) * size));
    if (footprint >= 1.0) {
        return original; // Keep existing mipmap/anisotropic minification.
    }
    vec2 at = uv * size - 0.5;
    vec2 cell = floor(at);
    vec4 wx = cubicWeights(fract(at.x));
    vec4 wy = cubicWeights(fract(at.y));
    vec4 filtered = vec4(0.0);
    // Positive B-spline weights avoid ringing. Premultiply each source texel before
    // filtering: four bilinear taps of straight-alpha data cannot do that correctly.
    // Sample texel centres at LOD 0 to respect each sampler's native wrap/clamp modes.
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            vec4 texel = textureLod(sampler2D(image, filtering),
                                   (cell + vec2(x - 1, y - 1) + 0.5) / size, 0.0);
            filtered += vec4(texel.rgb * texel.a, texel.a) * wx[x] * wy[y];
        }
    }
    // Fade to the original filter near 1:1 instead of popping at the LOD boundary.
    float strength = 1.0 - smoothstep(0.5, 1.0, footprint);
    filtered = mix(vec4(original.rgb * original.a, original.a), filtered, strength);
    return vec4(filtered.a > 0.0 ? filtered.rgb / filtered.a : vec3(0.0), filtered.a);
}

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
    vec4 base = sampleSprite(uImage, uSampler, vUv);
    outColor = base * vColor;
    if (pc.scale.z > 0.5) {
        // Keep-alpha alternate-texture mode: the original skin supplies coverage, not colour.
        // The alpha comparison is against K0.a = 2, followed by alternate alpha * vertex alpha.
        bool covered = outColor.a > 2.0 / 255.0;
        float maskAlpha = outColor.a;
        outColor = texture(uSecondTexture, vUv) * vColor;
        if (alphaToCoverage) {
            // Alternate skins change colour, not the original surface's MSAA silhouette.
            outColor.a = maskAlpha;
        }
        if (!covered) {
            discard;
        }
    } else if (pc.scale.z < 0.0) {
        outColor = blendFrames(base, sampleSprite(uSecondImage, uSecondSampler, vUv),
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
