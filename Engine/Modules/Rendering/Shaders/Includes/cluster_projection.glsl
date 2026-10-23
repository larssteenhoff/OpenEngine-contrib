#ifndef GE_CLUSTER_PROJECTION_GLSL
#define GE_CLUSTER_PROJECTION_GLSL

// View-space Z from reverse-Z depth, for perspective and orthographic
// projections. The four values are inverse-projection elements [2,2], [3,2],
// [2,3], [3,3] ([column,row]).
float GE_ClusterViewZ(float depth, float zDepth, float zConstant,
                     float wDepth, float wConstant)
{
    return (zDepth * depth + zConstant) /
           max(wDepth * depth + wConstant, 1e-30f);
}

// A cluster side plane must have an offset for orthographic views. Building
// every plane through the camera origin makes parallel tiles into wedges.
vec4 GE_ClusterInwardPlane(vec3 a, vec3 b, vec3 c, vec3 inside)
{
    vec3 n = normalize(cross(b - a, c - a));
    if (dot(n, inside - a) < 0.0f) n = n * -1.0f;
    return vec4(n, -dot(n, a));
}

float GE_ClusterPlaneDistance(vec4 plane, vec3 point)
{
    return dot(vec3(plane.x, plane.y, plane.z), point) + plane.w;
}
#endif
