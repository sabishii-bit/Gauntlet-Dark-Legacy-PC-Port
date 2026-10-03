#include "scene_depth.glsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Occlusion {
    mat4 clipToView;
    vec4 ao; // world radius, surface bias, maximum darkening, MSAA samples
    vec4 pixel;
} settings;
vec3 positionAt(vec2 at) {
    float depth = sceneDepthAt(at, settings.pixel.xy, int(settings.ao.w));
    vec4 p = settings.clipToView * vec4(at * 2.0 - 1.0, depth, 1.0);
    return p.xyz / max(abs(p.w), 0.000001);
}
void main() {
    vec4 original = texture(scene, uv);
    float depth = sceneDepthAt(uv, settings.pixel.xy, int(settings.ao.w));
    if (depth <= 0.0) { color = original; return; }
    vec3 p = positionAt(uv);
    vec3 left = p - positionAt(uv - vec2(settings.pixel.x, 0));
    vec3 right = positionAt(uv + vec2(settings.pixel.x, 0)) - p;
    vec3 up = p - positionAt(uv - vec2(0, settings.pixel.y));
    vec3 down = positionAt(uv + vec2(0, settings.pixel.y)) - p;
    // Choose the continuous side at silhouettes, not a derivative across a wall.
    vec3 dx = dot(left, left) < dot(right, right) ? left : right;
    vec3 dy = dot(up, up) < dot(down, down) ? up : down;
    vec3 normal = cross(dx, dy);
    if (dot(normal, normal) < 0.00000001) { color = original; return; }
    normal = normalize(normal);
    if (dot(normal, -p) < 0.0) normal = -normal;
    vec4 q = settings.clipToView * vec4((uv + settings.pixel.xy) * 2.0 - 1.0, depth, 1.0);
    vec2 worldPixel = max(abs((q.xyz / max(abs(q.w), 0.000001) - p).xy), vec2(0.0001));
    vec2 radius = min(vec2(settings.ao.x) / worldPixel, vec2(48.0)) * settings.pixel.xy;
    const vec2 disk[12] = vec2[](
        vec2(0.25, 0), vec2(-0.32, 0.29), vec2(0.05, -0.55),
        vec2(0.40, 0.52), vec2(-0.74, -0.13), vec2(0.70, -0.45),
        vec2(-0.24, 0.89), vec2(-0.45, -0.82), vec2(0.93, 0.32),
        vec2(-0.90, 0.42), vec2(0.42, -0.90), vec2(0.27, 0.95));
    float occlusion = 0.0;
    for (int i = 0; i < 12; ++i) {
        vec2 at = uv + disk[i] * radius;
        if (any(lessThan(at, vec2(0))) || any(greaterThan(at, vec2(1)))) continue;
        if (sceneDepthAt(at, settings.pixel.xy, int(settings.ao.w)) <= 0.0) continue;
        vec3 delta = positionAt(at) - p;
        float distance = length(delta);
        float horizon = max(dot(normal, delta) - settings.ao.y, 0.0) / max(distance, 0.001);
        occlusion += horizon * (1.0 - smoothstep(0.0, settings.ao.x, distance));
    }
    color = vec4(original.rgb * (1.0 - settings.ao.z * clamp(occlusion / 6.0, 0.0, 1.0)), original.a);
}
