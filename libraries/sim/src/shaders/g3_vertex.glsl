R""(
#version 420 core

layout (location = 0) in vec4 position;
layout (location = 1) in vec2 texCoord;
layout (location = 2) in vec4 inColor;
layout (location = 3) in vec3 inDepthPlane;
flat out vec3 depthPlane;

smooth out vec2 fragTexCoord;
noperspective out vec4 fragInColor;
noperspective out vec4 coords;
// SIM_SetWideUI3D: < 1 shows more of the scene on the sides of a wide view
// at native scale (0 means unset: 1).
uniform float clipScaleX;

void main()
{
    // XYZ already contains normalized device coordinates. Restore clip space
    // for perspective-correct UVs while retaining the emulated depth value.
    gl_Position = vec4(position.xyz * position.w, position.w);
    if (clipScaleX > 0.0) gl_Position.x *= clipScaleX;
    coords = vec4(position.x, position.y, position.z, 1.0);
    depthPlane = inDepthPlane;
    fragTexCoord = vec2(texCoord.x, texCoord.y);
    fragInColor = inColor;
}
)""
