layout(set = 0, binding = 0) uniform sampler2D scene;
#ifdef MULTISAMPLED_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS sceneDepth;
#else
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;
#endif
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Blur {
    mat4 clipToView;
    vec4 focus; // end of sharp range, transition length, radius in pixels, sample count
    vec4 pixel; // reciprocal framebuffer size
} settings;

float viewDistance(vec2 at) {
    ivec2 size = ivec2(1.0 / settings.pixel.xy + 0.5);
    ivec2 coord = clamp(ivec2(at * vec2(size)), ivec2(0), size - 1);
    float depth = 0.0;
#ifdef MULTISAMPLED_DEPTH
    for (int i = 0; i < int(settings.focus.w); ++i) {
        // Reversed Z: preserve the nearest silhouette at antialiased edges.
        depth = max(depth, texelFetch(sceneDepth, coord, i).r);
    }
#else
    depth = texelFetch(sceneDepth, coord, 0).r;
#endif
    vec4 p = settings.clipToView * vec4(at * 2.0 - 1.0, depth, 1.0);
    return p.z / max(abs(p.w), 0.000001);
}

void main() {
    vec4 original = texture(scene, uv);
    float distance = viewDistance(uv);
    float amount = smoothstep(settings.focus.x, settings.focus.x + settings.focus.y, distance);
    float radius = settings.focus.z * amount;
    if (radius < 0.25) {
        color = original;
        return;
    }
    // Small deterministic disk, no temporal noise or animation tied to render frequency.
    const vec2 disk[12] = vec2[](
        vec2(0.25, 0.0), vec2(-0.32, 0.29), vec2(0.05, -0.55),
        vec2(0.40, 0.52), vec2(-0.74, -0.13), vec2(0.70, -0.45),
        vec2(-0.24, 0.89), vec2(-0.45, -0.82), vec2(0.93, 0.32),
        vec2(-0.90, 0.42), vec2(0.42, -0.90), vec2(0.27, 0.95));
    vec3 sum = original.rgb;
    float weight = 1.0;
    for (int i = 0; i < 12; ++i) {
        vec2 at = clamp(uv + disk[i] * radius * settings.pixel.xy,
                        settings.pixel.xy * 0.5, 1.0 - settings.pixel.xy * 0.5);
        float neighbor = viewDistance(at);
        // Do not smear a sharp foreground character into the out-of-focus background.
        float accept = smoothstep(settings.focus.x, settings.focus.x + settings.focus.y, neighbor);
        accept *= step(distance - max(0.5, distance * 0.025), neighbor);
        sum += texture(scene, at).rgb * accept;
        weight += accept;
    }
    color = vec4(sum / weight, original.a);
}
