#version 450

layout(push_constant) uniform PushConstants {
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    mat4 viewProjection;
    vec4 openness;
} pc;

layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUv;
layout(location=3) in vec4 inBaseColor;
layout(location=4) in vec4 inMaterial;
layout(location=5) in vec3 indexPositionDelta;
layout(location=6) in vec3 gripPositionDelta;
layout(location=7) in vec3 thumbPositionDelta;
layout(location=8) in vec3 indexNormalDelta;
layout(location=9) in vec3 gripNormalDelta;
layout(location=10) in vec3 thumbNormalDelta;

layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
layout(location=2) out vec2 uv;
layout(location=3) out vec4 baseColor;
layout(location=4) out vec4 material;

void main() {
    vec3 position = inPosition + indexPositionDelta * pc.openness.x
        + gripPositionDelta * pc.openness.y + thumbPositionDelta * pc.openness.z;
    vec3 normal = inNormal + indexNormalDelta * pc.openness.x
        + gripNormalDelta * pc.openness.y + thumbNormalDelta * pc.openness.z;
    vec4 local = vec4(position, 1.0);
    vec4 world = vec4(dot(pc.modelRow0, local), dot(pc.modelRow1, local),
        dot(pc.modelRow2, local), 1.0);
    gl_Position = pc.viewProjection * world;
    worldPosition = world.xyz;
    worldNormal = normalize(vec3(dot(pc.modelRow0.xyz, normal),
        dot(pc.modelRow1.xyz, normal), dot(pc.modelRow2.xyz, normal)));
    uv = inUv;
    baseColor = inBaseColor;
    material = inMaterial;
}
