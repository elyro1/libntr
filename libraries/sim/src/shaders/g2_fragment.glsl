R""(
#version 420 core

uniform sampler2D myTexture;
uniform sampler2D g3FogTexture;
uniform float g3FogAlpha;
uniform bool g3FogAlphaOnly;
uniform sampler2D bgTexture[4];
uniform sampler2D objTexture[5]; //The extra element is for OBJs drawn on top of all BGs
uniform sampler2D objWindowTexture;
int bgOrder[4];
int bgPriorities[4];
uniform usampler2D windowMaskTexture;
uniform usampler2D scanlineBlendTexture;
int bldcnt;
int bldalpha;
int bldy;
int windowMask;
uniform int topScreen;
uniform int dispcnt;
uniform vec4 backdropColor;
uniform int bg0as3d;

smooth in vec2 fragTexCoord;
//uniform float inAlpha;
uniform float inMasterBrightFactor;
uniform int inMasterBrightMode;
out vec4 color;
uniform float presentationWidth;
uniform float borderWidth;
uniform float fieldWidth;
uniform bool fieldOverlayTiled;
uniform bool fieldDarknessMask;
uniform bool fieldMoveCutIn;
uniform bool screenFadeWindows;
uniform vec4 fieldMoveWindow;
uniform sampler2DArray fieldParticlesTexture;
uniform bool fieldParticlesEnabled;
uniform bool singleScreen;
uniform sampler2D sideAverageTexture;
uniform bool sideAverageValid;
uniform bool averagePass;
vec2 uiCoord;
vec2 worldCoord;

bool insideFieldMoveWindow() {
    // Scale only the horizontal reveal. The band keeps its native height.
    float x = fragTexCoord.x * 256.0;
    float y = ((topScreen == 1 ? 1.0 : 0.5) - uiCoord.y) * 384.0;
    // The DS exclusive right edge is 255; cover the final widescreen column.
    float right = fieldMoveWindow.z == 255.0 ? 256.0 : fieldMoveWindow.z;
    return (dispcnt & (1 << 13)) != 0 && x >= fieldMoveWindow.x && x < right
        && y >= fieldMoveWindow.y && y < fieldMoveWindow.w;
}

vec4 sampleBg(int layer) {
    if (fieldMoveCutIn && layer == 3) {
        if (!insideFieldMoveWindow()) return vec4(0.0);
        // Continue the moving pattern at its original pixel scale.
        return texture(bgTexture[layer], uiCoord);
    }
    if ((windowMask & (1 << layer)) == 0) return vec4(0.0);
    if (layer == 0 && bg0as3d != 0) {
        vec4 scene = texture(bgTexture[layer], worldCoord);
        // Rendering into transparent black stores premultiplied RGB. Recover
        // the source color before applying fog and DS background blending.
        if (scene.a > 0.0) scene.rgb /= scene.a;
        vec4 fog = texture(g3FogTexture, worldCoord);
        if (!g3FogAlphaOnly) scene.rgb = mix(scene.rgb, fog.rgb, fog.a);
        scene.a = mix(scene.a, g3FogAlpha, fog.a);
        return scene;
    }
    // Field weather/shading overlays blend BG2 over the 3D world. Repeat
    // seamless textures; extend cave mask edges without repeating the opening.
    bool fieldOverlay = fieldWidth > 256.0 && bg0as3d != 0 && layer == 2
        && (fieldDarknessMask || fieldOverlayTiled || (((bldcnt >> 6) & 3) == 1
            && (bldcnt & (1 << 2)) != 0 && (bldcnt & (1 << 8)) != 0));
    if (fieldOverlay) {
        if (fieldOverlayTiled) {
            return texture(bgTexture[layer], uiCoord);
        }
        vec2 overlayCoord = vec2(clamp(uiCoord.x, 0.5 / 256.0, 255.5 / 256.0), uiCoord.y);
        return texture(bgTexture[layer], overlayCoord);
    }
    if (uiCoord.x < 0.0 || uiCoord.x >= 1.0) return vec4(0.0);
    return texture(bgTexture[layer], uiCoord);
}
vec4 sampleObj(int layer) {
    if (!fieldMoveCutIn && (windowMask & 16) == 0) return vec4(0.0);
    if (uiCoord.x < 0.0 || uiCoord.x >= 1.0) {
        if (fieldParticlesEnabled && fieldWidth > 256.0 && bg0as3d != 0 && layer < 4) {
            if (fieldMoveCutIn && !insideFieldMoveWindow()) return vec4(0.0);
            return texture(fieldParticlesTexture, vec3(worldCoord, float(layer)));
        }
        return vec4(0.0);
    }
    if ((windowMask & 16) == 0) return vec4(0.0);
    return texture(objTexture[layer], uiCoord);
}
vec4 sampleWindow() {
    if (uiCoord.x < 0.0 || uiCoord.x >= 1.0) return vec4(0.0);
    return texture(objWindowTexture, uiCoord);
}
vec4 sideBackground() {
    // A single reduced screen color prevents UI shapes from leaking into the sides.
    vec3 average = sideAverageValid
        ? textureLod(sideAverageTexture, vec2(0.5), 8.0).rgb
        : backdropColor.rgb;
    float x = abs(fragTexCoord.x - 0.5) * presentationWidth;
    float distance = clamp((x - 128.0) / max((borderWidth - 256.0) * 0.5, 1.0), 0.0, 1.0);
    float shade = mix(0.55, 0.20, smoothstep(0.0, 1.0, distance));
    return vec4(average * shade, 1.0);
}

void main()
{
    uiCoord = vec2(0.5 + (fragTexCoord.x - 0.5) * presentationWidth / 256.0, fragTexCoord.y);
    worldCoord = vec2(0.5 + (fragTexCoord.x - 0.5) * presentationWidth / 448.0, fragTexCoord.y);
    int oamsDrawn = 0;
    int currentBgDrawn = 5; // Backdrop
    if(topScreen == 1) {
        if(fragTexCoord.t < 0.5){
            discard;
        }
    } else {
        if(fragTexCoord.t >= 0.5){
            discard;
        }
    }
    int row = clamp(int(floor(fragTexCoord.y * 384.0)), 0, 383);
    // Fade windows describe a whole-screen reveal, independently of native UI.
    // Sample their native mask across the viewport without stretching the scene.
    float maskX = screenFadeWindows ? fragTexCoord.x : uiCoord.x;
    int maskPixel = int(floor(maskX * 256.0 + (screenFadeWindows ? 0.0001 : 0.0)));
    // Fade assets use 255 as their full-width exclusive edge. Include the
    // outermost column when widening that edge, as the HM cut-in does.
    if (screenFadeWindows && presentationWidth > 256.0) maskPixel = min(maskPixel, 254);
    int column = clamp(maskPixel + 96, 0, 447);
    uvec2 masks = texelFetch(windowMaskTexture, ivec2(column, row), 0).rg;
    windowMask = int(masks.r);
    if ((windowMask & 128) != 0 && sampleWindow().a > 0.1)
        windowMask = int(masks.g);
    bool effectsEnabled = (windowMask & 32) != 0;
    uvec4 blend = texelFetch(scanlineBlendTexture, ivec2(0, row), 0);
    bldcnt = int(blend.r);
    bldalpha = int(blend.g);
    bldy = int(blend.b);
    for (int bg = 0; bg < 4; bg++) {
        bgPriorities[bg] = (int(blend.a) >> (bg * 2)) & 3;
        bgOrder[bg] = bg;
    }
    // Back to front; lower BG number wins when priorities match.
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 4; j++) {
        int a = bgOrder[i], b = bgOrder[j];
        if (bgPriorities[a] < bgPriorities[b]
            || (bgPriorities[a] == bgPriorities[b] && a < b)) {
            bgOrder[i] = b;
            bgOrder[j] = a;
        }
    }
    bool filler = singleScreen && fieldWidth == 256.0 && (uiCoord.x < 0.0 || uiCoord.x >= 1.0);
    if (filler) {
        color = sideBackground();
    } else {
    for(int i=0; i<4; i++) {
        int bgNum = bgOrder[i];
        int curPrio = bgPriorities[bgNum];
        // Draw the OBJs that are at or below the current BG priority
        for(int j=3-oamsDrawn; j>curPrio; j--) {
            if(sampleObj(j).a > 0.1) {
                color = sampleObj(j);
                currentBgDrawn = 4;
            }
            oamsDrawn++;
        }
        
        // Draw the current bg
        if(sampleBg(bgNum).a > 0.0) {
            // 3D BG0 uses per-pixel alpha even when it is not selected as a
            // first target in BLDCNT, as in the default battle animation setup.
            bool is3D = bgNum == 0 && bg0as3d != 0;
            bool secondTarget = ((bldcnt >> (8 + currentBgDrawn)) & 1) != 0;
            // BG0's 3D alpha blending also applies when the window disables
            // ordinary color effects (the evolution scene uses that setup).
            if ((is3D && secondTarget) || (effectsEnabled
                && (((bldcnt >> 6) & 0x3) == 1) && ((bldcnt >> bgNum & 1) == 1))) {
                // Alpha Blending Mode. bgNum is Target 1.
                float alphaA = min(16.0, float(bldalpha & 31)) / 16.0;
                float alphaB = min(16.0, float((bldalpha >> 8) & 31)) / 16.0;
                if(bgNum == 0 && bg0as3d != 0) {
                    alphaA = sampleBg(0).a;
                    alphaB = 1.0 - alphaA;
                }
                if((currentBgDrawn < 4) && (((bldcnt >> (8+currentBgDrawn)) & 1) == 1) ) {
                    color.r = min(1.0, ((sampleBg(bgNum).r *alphaA) + (sampleBg(currentBgDrawn).r * alphaB)));
                    color.g = min(1.0, ((sampleBg(bgNum).g *alphaA) + (sampleBg(currentBgDrawn).g * alphaB)));
                    color.b = min(1.0, ((sampleBg(bgNum).b *alphaA) + (sampleBg(currentBgDrawn).b * alphaB)));
                    color.a = 1.0;
                    currentBgDrawn = bgNum;
                } else if( (currentBgDrawn == 4) && ( ( (bldcnt >> (8+currentBgDrawn)) & 1) == 1) ){
                    // Alpha blending where the highest OBJ pixel is the target
                    color.r = min(1.0, ( (sampleBg(bgNum).r *alphaA) + color.r * alphaB) );
                    color.g = min(1.0, ( (sampleBg(bgNum).g *alphaA) + color.g * alphaB) );
                    color.b = min(1.0, ( (sampleBg(bgNum).b *alphaA) + color.b * alphaB) );
                    color.a = 1.0;
                } else if((currentBgDrawn == 5) && (((bldcnt >> (8+currentBgDrawn)) & 1) == 1)) {
                    // Alpha blending where the backdrop is the target
                    color.r = min(1.0, ((sampleBg(bgNum).r *alphaA) + backdropColor.r * alphaB));
                    color.g = min(1.0, ((sampleBg(bgNum).g *alphaA) + backdropColor.g * alphaB));
                    color.b = min(1.0, ((sampleBg(bgNum).b *alphaA) + backdropColor.b * alphaB));
                    color.a = 1.0;
                } else {
                    color = sampleBg(bgNum);
                    currentBgDrawn = bgNum;
                }
                // The blended background becomes the top layer even when its
                // second target was an OBJ or the backdrop. Later layers must
                // blend against it, and the final fallback must not erase it.
                currentBgDrawn = bgNum;
            } else if(effectsEnabled && (((bldcnt >> 6) & 0x3) == 2) && ((bldcnt >> bgNum & 1) == 1)) {
                // Brightness Increase Mode. bgNum is Target 1.
                float brightFactor = min(16.0, float(bldy & 31)) / 16.0;
                //color.r = min(1.0, ( sampleBg(bgNum).r + ((1.0 - sampleBg(bgNum).r)*brightFactor)) );
                //color.g = min(1.0, ( sampleBg(bgNum).g + ((1.0 - sampleBg(bgNum).g)*brightFactor)) );
                //color.b = min(1.0, ( sampleBg(bgNum).b + ((1.0 - sampleBg(bgNum).b)*brightFactor)) );
                color.r = sampleBg(bgNum).r + (1.0 - sampleBg(bgNum).r)*brightFactor;
                color.g = sampleBg(bgNum).g + (1.0 - sampleBg(bgNum).g)*brightFactor;
                color.b = sampleBg(bgNum).b + (1.0 - sampleBg(bgNum).b)*brightFactor;
                color.a = 1.0;
                currentBgDrawn = bgNum;
            } else if(effectsEnabled && (((bldcnt >> 6) & 0x3) == 3) && ((bldcnt >> bgNum & 1) == 1)) {
                // Brightness Decrease Mode. bgNum is Target 1.
                float brightFactor = min(16.0, float(bldy & 31)) / 16.0;
                color.r = max(0.0, ( sampleBg(bgNum).r - ((sampleBg(bgNum).r)*brightFactor)) );
                color.g = max(0.0, ( sampleBg(bgNum).g - ((sampleBg(bgNum).g)*brightFactor)) );
                color.b = max(0.0, ( sampleBg(bgNum).b - ((sampleBg(bgNum).b)*brightFactor)) );
                color.a = 1.0;
                currentBgDrawn = bgNum;
            } else {
                color.rgb = sampleBg(bgNum).rgb;
                color.a = 1.0;
                currentBgDrawn = bgNum;
            }
        }
    }
    //Draw the remaining objs
    for(int i=3-oamsDrawn; i>=0; i--) {
        if(sampleObj(i).a > 0.1) {
            color = sampleObj(i);
            currentBgDrawn = 4;
        }
    }
    if(currentBgDrawn == 5) {
        color = backdropColor;
    }
    
    } // End scene composition; master brightness also affects the filler.
    // Reduce the pre-fade color; presentation applies master brightness once.
    if (averagePass) {
        color.a = 1.0;
        return;
    }
    if(inMasterBrightMode == 1) {
        // brightness up
        color.r = color.r + ((1.0 - color.r) * (inMasterBrightFactor/16.0));
        color.g = color.g + ((1.0 - color.g) * (inMasterBrightFactor/16.0));
        color.b = color.b + ((1.0 - color.b) * (inMasterBrightFactor/16.0));
    } else if(inMasterBrightMode == 2) {
        // brightness down
        color.r = color.r - (color.r * (inMasterBrightFactor / 16.0));
        color.g = color.g - (color.g * (inMasterBrightFactor / 16.0));
        color.b = color.b - (color.b * (inMasterBrightFactor / 16.0));
    }
    if (filler && presentationWidth > borderWidth) {
        // Match the capped gradient at the join, then reach black with zero
        // slope at both ends. Apply after master brightness to keep the outer
        // window edge black during white transitions too.
        float x = abs(fragTexCoord.x - 0.5) * presentationWidth;
        float fade = smoothstep(borderWidth * 0.5, presentationWidth * 0.5, x);
        color.rgb *= 1.0 - fade;
    }
}
)""
