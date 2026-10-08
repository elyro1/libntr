R""(
#version 420 core

layout (location = 0) in vec4 position;
layout (location = 1) in vec2 texCoord;
layout (location = 2) in vec4 inColor;

smooth out vec2 fragTexCoord;
noperspective out vec4 fragInColor;
noperspective out vec4 coords;

void main()
{
    // XYZ already contains normalized device coordinates. Restore clip space
    // for perspective-correct UVs while retaining the emulated depth value.
    gl_Position = vec4(position.xyz * position.w, position.w);
    coords = vec4(position.x, position.y, position.z, 1.0);
    fragTexCoord = vec2(texCoord.x, texCoord.y);
    fragInColor = inColor;
}
)""
