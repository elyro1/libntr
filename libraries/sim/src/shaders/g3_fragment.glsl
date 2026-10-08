R""(
#version 420 core

uniform sampler2D myTexture;
uniform int useTexture;
uniform int polygonMode;
// 0: all pixels, 1: opaque pixels, 2: translucent pixels.
uniform int alphaPass;
uniform int useFog;
uniform vec4 fogColor;
uniform float fogDepthBoundary[32];
uniform float fogDensity[32];
smooth in vec2 fragTexCoord;
noperspective in vec4 fragInColor;
noperspective in vec4 coords;
layout (location = 0) out vec4 color;
layout (location = 1) out vec4 fogData;

void main()
{
    vec4 shadedColor;
    if( useTexture == 1 ) {
        if(polygonMode == 0) {
            shadedColor.r = texture(myTexture, fragTexCoord).r * fragInColor.r;
            shadedColor.g = texture(myTexture, fragTexCoord).g * fragInColor.g;
            shadedColor.b = texture(myTexture, fragTexCoord).b * fragInColor.b;
            shadedColor.a = texture(myTexture, fragTexCoord).a * fragInColor.a;
        } else {
            shadedColor = texture(myTexture, fragTexCoord);
        }
        if( shadedColor.a <= 0.0) {
            discard;
        }
    } else {
        shadedColor = fragInColor;
    }
    // Alpha textures contain both opaque and translucent pixels. Only opaque
    // pixels write depth by default, even when they share the same polygon.
    if(shadedColor.a <= 0.0 ||
       (alphaPass == 1 && shadedColor.a < 1.0) ||
       (alphaPass == 2 && shadedColor.a >= 1.0)) {
        discard;
    }
    
    // Fog is applied after translucent polygons have been blended. A flashlight
    // disables fog for covered pixels, revealing the already rendered world.
    color = shadedColor;
    fogData = vec4(0.0);
    if(useFog == 1) {
        float fog_maxdist = 1.0;
        float fog_mindist = 0.5;
        float dist = length(coords.xyz);
        float fogFactor = 0.0;
        if(coords.z < fogDepthBoundary[0]) {
            
            fogFactor = fogDensity[0];
        } else {
            int foundIdx = 31;
            for(int i=1; i<32; i++) {
                if(coords.z < fogDepthBoundary[i]) {
                    foundIdx = i;
                    break;
                }
            }
            if(foundIdx == 31) {
                fogFactor = fogDensity[foundIdx];
            } else {
                float interpolationFactor = (fogDepthBoundary[foundIdx-1] - coords.z) / (fogDepthBoundary[foundIdx] - fogDepthBoundary[foundIdx-1]);
                fogFactor = (fogDensity[foundIdx - 1] * (interpolationFactor)) + (fogDensity[foundIdx] * (1.0-interpolationFactor));
            }
        }
        fogData = vec4(fogColor.rgb, fogFactor);
    }
}
)""
