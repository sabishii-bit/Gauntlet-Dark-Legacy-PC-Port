#version 450

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Bloom {
    mat4 unused;
    vec4 glow; // soft threshold start/end, strength, sample spacing in pixels
    vec4 pixel; // reciprocal framebuffer size
} settings;

void main() {
    vec4 original = texture(scene, uv);
    // A normalized binomial kernel keeps the result stable without temporal noise/history.
    const float weights[5] = float[](1.0, 4.0, 6.0, 4.0, 1.0);
    vec3 bloom = vec3(0.0);
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 at = uv + vec2(x, y) * settings.glow.w * settings.pixel.xy;
            vec3 sampleColor = texture(scene, at).rgb;
            // Peak channel preserves saturated magic/fire, unlike a luminance-only cutoff.
            float peak = max(sampleColor.r, max(sampleColor.g, sampleColor.b));
            float bright = smoothstep(settings.glow.x, settings.glow.y, peak);
            bloom += sampleColor * bright * weights[x + 2] * weights[y + 2];
        }
    }
    bloom *= settings.glow.z / 256.0;
    // Screen blend preserves detail in the bounded scene buffer instead of clipping highlights.
    color = vec4(original.rgb + bloom * (1.0 - original.rgb), original.a);
}
