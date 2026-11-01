// What every draw pushes, to both stages: the transform, then the texture coordinate
// offset (xy), the alpha test (z) and the darkening (w), then the coordinate scale (xy),
// the second-stage mode (z: 1 a masked skin, 0 a lightmap, negative a frame blend) and
// the RGB combiner scale (w), then sprite filtering (x) and lightmap mode (y):
// 0 = alpha intensity, 1 = silhouette cut, -1 = imported RGB illumination.
// Immediate effectData0.z is white flash, .w reciprocal fog depth scale;
// effectData1 holds fog RGB and bias. Post shaders reuse these four-vectors separately.
PUSH_CONSTANTS_BEGIN(PushConstants)
    mat4 transform;
    vec4 params;
    vec4 scale;
    vec4 effectData0;
    vec4 effectData1;
PUSH_CONSTANTS_END(PushConstants, pc)
