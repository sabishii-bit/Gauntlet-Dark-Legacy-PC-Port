#ifdef MULTISAMPLED_DEPTH
layout(set = 1, binding = 0) uniform sampler2DMS sceneDepth;
#else
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;
#endif
float sceneDepthAt(vec2 at, vec2 pixel, int samples) {
    ivec2 size = ivec2(1.0 / pixel + 0.5);
    ivec2 coord = clamp(ivec2(at * vec2(size)), ivec2(0), size - 1);
    float depth = 0.0;
#ifdef MULTISAMPLED_DEPTH
    for (int i = 0; i < samples; ++i) {
        depth = max(depth, texelFetch(sceneDepth, coord, i).r);
    }
#else
    depth = texelFetch(sceneDepth, coord, 0).r;
#endif
    return depth;
}
