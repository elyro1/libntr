R""(
#version 420 core

uniform sampler2D myTexture;
uniform int useTexture;
uniform int polygonMode;
uniform int useWBuffer;
uniform vec4 depthViewport;
flat in vec3 depthPlane;
// 0: all pixels, 1: opaque pixels, 2: translucent pixels.
uniform int alphaPass;
uniform int useFog;
uniform vec4 fogColor;
uniform float fogDepthBoundary[32];
uniform float fogDensity[32];
// Field color grade: exposure, saturation and contrast, then a split tone
// tinting shadows and highlights separately. Identity outside the field.
uniform vec3 gradeShadows;
uniform vec3 gradeHighlights;
uniform vec3 gradeParams; // exposure, saturation, contrast
uniform vec4 atmosphereFog;
uniform vec2 atmosphereRange;
uniform int fieldEffectMode;
uniform int fieldEmissionOnly;
uniform float fieldEffectPhase;
// Per-material weight of the light this pixel adds to the bloom pass.
uniform float fieldEmissionWeight;
uniform int fieldSurface;
uniform int fieldWorld;
uniform float fieldTime;
uniform float fieldWetness;
uniform vec4 groundMist;
uniform vec2 groundMistLayer; // Base height and depth.
uniform mat3 worldFromClip;
// Lights mirrored in water: where on the water (x, z), the light's height
// above it and its strength, and color.
uniform vec4 emitterPosition[48];
uniform vec3 emitterColor[48];
uniform int emitterCount;
// Down the screen along the ground, the way reflections stretch.
uniform vec2 reflectionDown;
uniform vec3 worldFromClipOffset;
uniform vec3 cameraWorld;
smooth in vec2 fragTexCoord;
noperspective in vec4 fragInColor;
noperspective in vec4 coords;
layout (location = 0) out vec4 color;
layout (location = 1) out vec4 fogData;
// Light emitted by this pixel, blended like color so nearer surfaces occlude it.
layout (location = 2) out vec4 emission;

vec3 Grade(vec3 c)
{
    c *= gradeParams.x;
    float luma = dot(c, vec3(0.25,0.5,0.25));
    c = mix(vec3(luma), c, gradeParams.y);
    // Contrast pivots around a mid-dark value so night scenes keep detail.
    c = max(vec3(0.0), 0.35 + (c - 0.35) * gradeParams.z);
    float tone = smoothstep(0.05, 0.65, dot(c, vec3(0.25,0.5,0.25)));
    return c * mix(gradeShadows, gradeHighlights, tone);
}

uint HashCell(ivec2 cell)
{
    uint h = uint(cell.x) * 1597334677u ^ uint(cell.y) * 3812015801u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
// Smooth value noise in 0..1 on a grid of the given cell size.
float ValueNoise(vec2 p, float size)
{
    vec2 g = p / size, f = fract(g);
    ivec2 c = ivec2(floor(g));
    f = f * f * (3.0 - 2.0 * f);
    float a = float(HashCell(c) & 1023u), b = float(HashCell(c + ivec2(1, 0)) & 1023u);
    float d = float(HashCell(c + ivec2(0, 1)) & 1023u), e = float(HashCell(c + ivec2(1, 1)) & 1023u);
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y) / 1023.0;
}
// Rain drops landing on a surface, on the DS pixel grid (world units or
// texture tiles * 16): each 7x7 pixel cell holds one expanding ring at a
// random spot and time. Returns its brightness. reach 1 also draws rings
// from neighboring cells that spill over; 0 only this cell's, which is
// cheaper and enough for faint drops.
float RainRings(vec2 pixelPosition, float time, int reach)
{
    ivec2 pixel = ivec2(floor(pixelPosition));
    ivec2 cell = ivec2(floor(vec2(pixel) / 7.0));
    float brightness = 0.0;
    for (int dy = -reach; dy <= reach; dy++)
        for (int dx = -reach; dx <= reach; dx++) {
            ivec2 c = cell + ivec2(dx, dy);
            uint h = HashCell(c + ivec2(31, 77));
            vec2 center = vec2(c * 7) + vec2(float((h & 7u) % 7u), float(((h >> 3) & 7u) % 7u)) + 0.5;
            float life = fract(time * 0.9 + float((h >> 6) & 1023u) / 1023.0);
            float radius = 0.5 + life * 3.5;
            float d = length(vec2(pixel) + 0.5 - center);
            // Half of the cells are dry at any time, so drops stay sparse.
            float ringActive = ((h >> 20) & 1u) == 1u ? 1.0 : 0.0;
            brightness = max(brightness, (1.0 - smoothstep(0.35, 0.75, abs(d - radius))) * (1.0 - life) * ringActive);
        }
    return brightness;
}

// Ground painted under a tree stump: anything but the stump's reds and browns.
bool StumpDisc(vec4 c)
{
    return c.a > 0.0 && c.r < max(c.g, c.b) - 0.02;
}

void main()
{
    // Evaluate a common plane at the pixel position so coplanar triangles
    // agree on depth. Z stays affine; W is recovered from reciprocal clip W.
    float depth = gl_FragCoord.z;
    float reciprocalW = gl_FragCoord.w;
    if(any(notEqual(depthPlane, vec3(0.0)))) {
        vec2 ndc = (gl_FragCoord.xy - depthViewport.xy) * 2.0 / depthViewport.zw - 1.0;
        float planeDepth = dot(depthPlane, vec3(ndc, 1.0));
        if(useWBuffer == 1) reciprocalW = planeDepth;
        else depth = clamp(planeDepth * 0.5 + 0.5, 0.0, 1.0);
    }
    if(useWBuffer == 1) {
        depth = clamp((4096.0 / 16777215.0) / reciprocalW, 0.0, 1.0);
    }
    gl_FragDepth = depth;
    // The pixel's world position, when the field provides its camera.
    bool hasWorld = fieldWorld == 1 && reciprocalW > 0.0;
    vec3 world = vec3(0.0);
    if (hasWorld) {
        vec2 ndc = (gl_FragCoord.xy - depthViewport.xy) * 2.0 / depthViewport.zw - 1.0;
        float w = 1.0 / reciprocalW;
        world = worldFromClip * vec3(ndc * w, w) + worldFromClipOffset;
    }
    float fogDepth = depth * 2.0 - 1.0;
    vec4 shadedColor;
    vec4 texel = vec4(1.0);
    if( useTexture == 1 ) {
        texel = texture(myTexture, fragTexCoord);
        shadedColor = polygonMode == 0 ? texel * fragInColor : texel;
        // Cast shadows blur past the sprite's outline; see mode 23.
        if( shadedColor.a <= 0.0 && fieldEffectMode != 23) {
            discard;
        }
    } else {
        shadedColor = fragInColor;
    }
    vec3 emitted = vec3(0.0);
    vec3 waterGlow = vec3(0.0);
    float waterLight = 0.0;
    if ((fieldEffectMode > 0 && fieldEffectMode <= 7) || fieldEffectMode == 17 || fieldEffectMode == 24) {
        vec2 p = fragTexCoord * 2.0 - 1.0;
        float radius = length(p);
        float opacity = 0.0;
        if (fieldEffectMode == 1) {
            // Soft, compact contact shadow with a dense center and feathered edge.
            opacity = (1.0 - smoothstep(0.42, 1.0, radius)) * fieldEffectPhase;
        } else if (fieldEffectMode == 2) {
            float ring = 1.0 - smoothstep(0.045, 0.12, abs(radius - 0.70));
            opacity = ring * 0.55 * pow(1.0 - clamp(fieldEffectPhase,0.0,1.0), 2.0);
        } else if (fieldEffectMode == 3) {
            // Open water. Texture coordinates are world positions in tiles;
            // work on the DS pixel grid so the light matches the pixel art.
            ivec2 pixel = ivec2(floor(fragTexCoord * 16.0));
            vec2 w = (vec2(pixel) + 0.5) / 16.0;
            float t = fieldEffectPhase;
            // Slow, soft light bands drifting across the surface.
            float bands = sin(w.x * 1.9 + t * 0.55 + 1.3 * sin(w.y * 1.1 + t * 0.35))
                * sin(w.y * 2.3 - t * 0.45 + 1.1 * sin(w.x * 0.9 - t * 0.30));
            float shimmer = smoothstep(0.30, 0.95, bands) * 0.12;
            // Sparse pixel sparkles: each 8x8 pixel cell may hold one, at a
            // random spot, twinkling in and fading out at its own time.
            ivec2 cell = pixel >> 3;
            uint h = uint(cell.x) * 1597334677u ^ uint(cell.y) * 3812015801u;
            h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
            ivec2 center = cell * 8 + 1 + ivec2(int(h & 7u) % 6, int((h >> 3) & 7u) % 6);
            ivec2 d = abs(pixel - center);
            float life = fract(t * 0.3 + float((h >> 6) & 1023u) / 1023.0 * 7.0);
            float twinkle = smoothstep(0.0, 0.08, life) * (1.0 - smoothstep(0.08, 0.30, life));
            float arm = twinkle > 0.6 ? 2.0 : 1.0;
            float star = d == ivec2(0) ? 1.0
                : ((d.x == 0 && float(d.y) <= arm) || (d.y == 0 && float(d.x) <= arm)) ? 0.55 : 0.0;
            float sparkle = ((h >> 16) & 255u) < 90u ? star * twinkle : 0.0;
            opacity = shimmer + sparkle * 0.85;
            // Rain drops on water and puddles.
            if (fieldWetness > 0.0)
                opacity += RainRings(fragTexCoord * 16.0, fieldTime + 0.37, 1) * 0.55 * clamp(fieldWetness, 0.0, 1.0);
        } else if (fieldEffectMode == 7) {
            opacity = exp(-dot(p,p) * 3.5) * fieldEffectPhase;
        } else if (fieldEffectMode == 17) {
            // Lighthouse beam: s runs from the lantern outwards, t across it.
            float along = (p.x + 1.0) * 0.5;
            opacity = pow(1.0 - along, 1.5) * exp(-p.y * p.y * 3.0) * smoothstep(0.0, 0.08, along) * fieldEffectPhase;
        } else if (fieldEffectMode == 24) {
            // Street light cone: s runs from the lamp head down to the road,
            // t across. It stays visible most of the way down, then dissolves
            // into the light pool it lands on.
            float along = (p.x + 1.0) * 0.5;
            opacity = (1.0 - 0.55 * along) * exp(-p.y * p.y * 2.5) * smoothstep(0.0, 0.06, along)
                * (1.0 - smoothstep(0.7, 1.0, along)) * fieldEffectPhase;
        } else {
            float life = clamp(min(fieldEffectPhase * 5.0, (1.0 - fieldEffectPhase) * 5.0), 0.0, 1.0);
            if (fieldEffectMode == 5) {
                float leaf = abs(p.x) + abs(p.y) * 1.5;
                opacity = (1.0 - smoothstep(0.60,1.0,leaf)) * life * 0.70;
            } else if (fieldEffectMode == 6) {
                opacity = exp(-dot(p,p) * 4.0) * life * 0.85;
            } else {
                opacity = (1.0 - smoothstep(0.15,1.0,radius)) * life * 0.55;
            }
        }
        shadedColor = vec4(fragInColor.rgb, fragInColor.a * opacity);
        if (fieldEffectMode == 17 || fieldEffectMode == 24) emitted = fragInColor.rgb;
    }
    if (fieldEffectMode == 8) shadedColor.a *= fieldEffectPhase;
    // A character's cast shadow: its sprite drawn flat on the ground as a
    // dark silhouette of the given opacity. The outline softens with distance
    // from the feet (the sprite's bottom), like a penumbra: the sprite's
    // alpha is averaged over two rings of texels around each point.
    if (fieldEffectMode == 23) {
        float coverage = texel.a;
        if (useTexture == 1) {
            vec2 texelSize = 1.0 / vec2(textureSize(myTexture, 0));
            float spread = mix(0.6, 2.6, clamp(1.0 - fragTexCoord.y, 0.0, 1.0));
            coverage = texel.a;
            for (int i = 0; i < 8; i++) {
                float angle = float(i) * 0.785398 + 0.39;
                vec2 direction = vec2(cos(angle), sin(angle)) * texelSize * spread;
                coverage += texture(myTexture, clamp(fragTexCoord + direction, vec2(0.0), vec2(1.0))).a;
                coverage += texture(myTexture, clamp(fragTexCoord + direction * 0.5, vec2(0.0), vec2(1.0))).a;
            }
            coverage /= 17.0;
        }
        if (coverage <= 0.0)
            discard;
        shadedColor = vec4(0.10, 0.13, 0.16, fieldEffectPhase * coverage);
    }
    if (fieldEffectMode == 22 && useTexture == 1) {
        // Tree atlases paint the tree's shadow as a disc around the stump in
        // 32x32 stump cells. A cell is one when its center is stump with disc
        // to the left and right; disc texels inside the disc's ellipse fade
        // with the shadow, showing the ground underneath. Canopy cells and
        // canopy edges reaching into a stump cell stay opaque.
        ivec2 size = textureSize(myTexture, 0);
        ivec2 t = ivec2(floor(fragTexCoord * vec2(size))) & (size - 1);
        ivec2 cell = t & ~31;
        vec4 stump = texelFetch(myTexture, cell + ivec2(16, 12), 0);
        if (cell.x + 32 <= size.x && cell.y + 32 <= size.y && stump.a > 0.0 && stump.r >= max(stump.g, stump.b)
            && StumpDisc(texelFetch(myTexture, cell + ivec2(3, 16), 0))
            && StumpDisc(texelFetch(myTexture, cell + ivec2(28, 16), 0))) {
            vec2 d = (vec2(t - cell) + 0.5 - 16.5) / vec2(15.0, 13.0);
            if (dot(d, d) < 1.0 && StumpDisc(texel))
                shadedColor.a *= fieldEffectPhase;
        }
    }
    if (fieldEffectMode == 21 && useTexture == 1) {
        // Tree shadows painted into an opaque ground tile as a darker disc.
        // Uniform corners hold the lit ground and a uniform, darker center
        // marks the disc; textures without one (plain grass) stay as they are.
        // Shadow texels lift towards the lit ground as the shadow fades.
        ivec2 size = textureSize(myTexture, 0);
        ivec2 c = size / 2;
        vec3 corners[4] = vec3[4](texelFetch(myTexture, ivec2(1, 1), 0).rgb, texelFetch(myTexture, ivec2(size.x - 2, 1), 0).rgb,
            texelFetch(myTexture, ivec2(1, size.y - 2), 0).rgb, texelFetch(myTexture, size - 2, 0).rgb);
        vec3 cores[4] = vec3[4](texelFetch(myTexture, c - 1, 0).rgb, texelFetch(myTexture, ivec2(c.x, c.y - 1), 0).rgb,
            texelFetch(myTexture, ivec2(c.x - 1, c.y), 0).rgb, texelFetch(myTexture, c, 0).rgb);
        vec3 lit = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25;
        vec3 core = (cores[0] + cores[1] + cores[2] + cores[3]) * 0.25;
        const vec3 luma = vec3(0.25, 0.5, 0.25);
        float spread = 0.0;
        for (int i = 0; i < 4; i++)
            spread = max(spread, max(abs(dot(corners[i] - lit, luma)), abs(dot(cores[i] - core, luma))));
        if (dot(lit - core, luma) > 0.04 && spread < 0.02) {
            float shade = smoothstep(0.005, 0.03, dot(lit - texel.rgb, luma));
            vec3 ground = mix(texel.rgb, lit, shade * (1.0 - clamp(fieldEffectPhase, 0.0, 1.0)));
            shadedColor.rgb = polygonMode == 0 ? ground * fragInColor.rgb : ground;
        }
    }
    if (fieldEffectMode == 9) {
        // Glass/light materials retain texture detail, but emission bypasses
        // daylight tint. No heuristic selects dark pixels from other textures.
        float detail = useTexture == 1 ? dot(texel.rgb, vec3(0.25,0.5,0.25)) : dot(shadedColor.rgb,vec3(0.25,0.5,0.25));
        vec3 warm = vec3(1.0,0.78,0.38) * sqrt(max(0.0,detail));
        shadedColor.rgb = mix(Grade(shadedColor.rgb), warm, clamp(fieldEffectPhase,0.0,1.0));
        emitted = warm * clamp(fieldEffectPhase,0.0,1.0);
    }
    if (fieldEffectMode == 10) {
        vec3 source = useTexture == 1 ? texel.rgb : shadedColor.rgb;
        shadedColor.rgb = mix(Grade(shadedColor.rgb), source, clamp(fieldEffectPhase,0.0,1.0));
        emitted = source * clamp(fieldEffectPhase,0.0,1.0);
    }
    if (fieldEffectMode == 13 || fieldEffectMode == 18) {
        // Lamp meshes share one texture between post and glass. Only the
        // near-white glass emits; the post keeps ordinary lighting. 18 keeps
        // only the very brightest texels, for light bars inside white trim.
        vec3 source = useTexture == 1 ? texel.rgb : shadedColor.rgb;
        float luma = dot(source, vec3(0.25,0.5,0.25));
        float glass = fieldEffectMode == 13 ? smoothstep(0.70, 0.90, luma) : smoothstep(0.90, 0.97, luma);
        float lit = clamp(fieldEffectPhase,0.0,1.0) * glass;
        shadedColor.rgb = mix(Grade(shadedColor.rgb), source, lit);
        emitted = source * lit;
    }
    if (fieldEffectMode == 14 || fieldEffectMode == 16) {
        // Buildings whose lights share a texture with plain walls: only
        // strongly colored texels light up. 14 keeps their own color (neon
        // bulbs), 16 turns them into warm lit windows.
        // Saturation of the shaded color covers both colored textures and
        // gray textures tinted by their material color.
        float peak = max(shadedColor.r, max(shadedColor.g, shadedColor.b));
        float saturation = (peak - min(shadedColor.r, min(shadedColor.g, shadedColor.b))) / max(peak, 0.001);
        float key = fieldEffectMode == 14 ? smoothstep(0.40, 0.60, saturation) : smoothstep(0.08, 0.16, saturation);
        float lit = clamp(fieldEffectPhase,0.0,1.0) * key;
        vec3 source = useTexture == 1 ? texel.rgb : vec3(1.0);
        float brightness = max(source.r, max(source.g, source.b));
        vec3 glow = fieldEffectMode == 14 ? shadedColor.rgb / max(peak, 0.001) * brightness
            : vec3(1.0,0.78,0.38) * sqrt(brightness);
        shadedColor.rgb = mix(Grade(shadedColor.rgb), glow, lit);
        emitted = glow * lit;
    }
    if (fieldEffectMode == 20) {
        // An emblem sharing its texture with stone and roof: only yellow
        // (not orange) texels emit, at full brightness in their own hue.
        vec3 source = useTexture == 1 ? texel.rgb : shadedColor.rgb;
        float peak = max(source.r, max(source.g, source.b));
        float yellow = (min(source.r, source.g) - source.b) / max(peak, 0.001);
        float key = smoothstep(0.25, 0.40, yellow) * smoothstep(0.65, 0.80, source.g / max(source.r, 0.001));
        vec3 glow = min(source / max(peak, 0.001), vec3(1.0));
        float lit = clamp(fieldEffectPhase,0.0,1.0) * key;
        shadedColor.rgb = mix(Grade(shadedColor.rgb), glow, lit);
        emitted = glow * lit;
    }
    // Ground pools and emissive materials apply the grade themselves.
    bool selfTinted = fieldEffectMode == 7 || (fieldEffectMode >= 9 && fieldEffectMode <= 10)
        || fieldEffectMode == 13 || fieldEffectMode == 18 || fieldEffectMode == 20 || fieldEffectMode == 14 || fieldEffectMode == 16 || fieldEffectMode == 17 || fieldEffectMode == 24;
    // Alpha textures contain both opaque and translucent pixels. Only opaque
    // pixels write depth by default, even when they share the same polygon.
    if(shadedColor.a <= 0.0 ||
       (alphaPass == 1 && shadedColor.a < 1.0) ||
       (alphaPass == 2 && shadedColor.a >= 1.0)) {
        discard;
    }
    
    // Fog is applied after translucent polygons have been blended. A flashlight
    // disables fog for covered pixels, revealing the already rendered world.
    // Lights mirrored in a water surface (mode 27; libntr places each
    // reflection on the water in front of its light). Ripples break the
    // mirror image into a column of dashes reaching down the screen, on the
    // DS pixel grid like the art: each two-pixel row shifts sideways and
    // changes length a few times a second, and the light catches mostly on
    // the crests of the water's own painted waves. The column tapers and
    // fades away from the light, its start glows nearly white, and a faint
    // haze of the light's color surrounds it.
    if (fieldEffectMode == 27 && emitterCount > 0 && hasWorld) {
        vec2 p = floor(world.xz) + 0.5;
        float tick = floor(fieldTime * 6.0);
        vec3 waterTexel = useTexture == 1 ? texel.rgb : shadedColor.rgb;
        float crest = 0.55 + 0.45 * smoothstep(0.50, 0.62, dot(waterTexel, vec3(0.25, 0.5, 0.25)));
        for (int i = 0; i < emitterCount; i++) {
            vec2 spot = emitterPosition[i].xy;
            float h = emitterPosition[i].z;
            vec2 down = reflectionDown;
            vec2 d = p - spot;
            float along = dot(d, down);
            float len = min(22.0 + h * 1.0, 54.0), width = min(5.0 + h * 0.1, 8.0);
            if (along < -6.0 || along > len) continue;
            float u = clamp(along / len, 0.0, 1.0);
            float row = floor(along / 2.0);
            uint rh = HashCell(ivec2(int(row), int(tick)) + ivec2(i * 131, 17));
            float shift = (float(rh & 15u) / 15.0 - 0.5) * 3.0 * (0.5 + u);
            float dash = width * (0.8 + 0.5 * float((rh >> 4) & 7u) / 7.0) * mix(1.0, 0.55, u);
            float across = abs(dot(d, vec2(-down.y, down.x)) + shift);
            float core = (1.0 - step(dash * 0.5, across)) * crest;
            // Some rows drop out entirely further from the light.
            core *= ((rh >> 8) & 7u) < uint(u * 4.0) ? 0.0 : 1.0;
            float fade = along >= 0.0 ? 1.0 - u * u : 1.0 - smoothstep(0.0, 6.0, -along);
            float haze = exp(-across * across / (width * width * 1.8)) * 0.22;
            float amount = emitterPosition[i].w * fade * max(core, haze);
            vec3 hue = mix(emitterColor[i], vec3(1.0), 0.3 * core * (1.0 - smoothstep(0.0, 0.25, u)));
            waterGlow += hue * amount;
            waterLight += amount;
        }
    }
    // Rain on world surfaces. The world position's screen-space derivatives
    // give the surface normal.
    // Everything darkens and gets richer; up-facing paths and sand (mode 26)
    // collect puddles that mirror the sky and catch rain drops.
    if (fieldSurface == 1 && fieldWetness > 0.0 && hasWorld
        && (fieldEffectMode == 0 || fieldEffectMode == 8 || fieldEffectMode == 21 || fieldEffectMode == 22 || fieldEffectMode == 26)) {
        vec3 n = normalize(cross(dFdx(world), dFdy(world)));
        if (dot(n, cameraWorld - world) < 0.0) n = -n;
        float wet = clamp(fieldWetness, 0.0, 1.0);
        vec3 c = shadedColor.rgb;
        float luma = dot(c, vec3(0.25, 0.5, 0.25));
        c = mix(c, max(mix(vec3(luma), c, 1.35) * 0.86, vec3(0.0)), wet);
        // Puddles sit on the DS pixel grid, like the art.
        vec2 pixel = floor(world.xz) + 0.5;
        float up = smoothstep(0.80, 0.95, n.y);
        // A puddle sits inside one 16-unit tile, so it never runs into a
        // neighboring tile of another material: a quarter of the tiles hold
        // one, at most 7.3 units from the tile's center, with a ragged edge.
        // Only up-facing puddle ground (mode 26) works this out.
        float puddle = 0.0, rim = 0.0;
        if (fieldEffectMode == 26 && up > 0.0) {
            ivec2 tile = ivec2(floor(world.xz / 16.0));
            uint th = HashCell(tile + ivec2(53, 11));
            if ((th & 3u) == 0u) {
                vec2 center = vec2(tile) * 16.0 + 8.0 + (vec2(float(th & 7u), float((th >> 3) & 7u)) / 7.0 - 0.5) * 3.0;
                float size = 3.5 + float((th >> 6) & 3u) * 0.5;
                float aspect = 1.0 + float((th >> 8) & 3u) * 0.15;
                vec2 offset = pixel - center;
                float d = length(vec2(offset.x, offset.y * aspect));
                // Only near the edge does the ragged outline matter.
                if (d < size + 1.0)
                    d += (ValueNoise(pixel, 3.0) - 0.5) * 1.5;
                puddle = up * step(d, size) * wet;
                rim = step(size - 1.0, d);
            }
        }
        if (puddle > 0.0) {
            // Like the game's own puddles: light sky blue over the ground, more
            // opaque along the edge, with rain drops landing in it.
            float rings = RainRings(pixel, fieldTime, 1);
            vec3 water = mix(c * 0.85, vec3(0.60, 0.84, 0.97), mix(0.43, 0.68, rim)) + rings * 0.18;
            c = mix(c, water, puddle);
        } else if (up > 0.0) {
            // Faint drops on all other wet ground.
            c += RainRings(pixel, fieldTime, 0) * 0.03 * up * wet;
        }
        shadedColor.rgb = c;
    }
    // A character near a warm light source at night: its own colors, lit
    // warm, take over from the night grade as far as the light reaches.
    vec3 warmLit = shadedColor.rgb * vec3(1.05, 0.86, 0.62);
    if (!selfTinted) shadedColor.rgb = Grade(shadedColor.rgb);
    // Mirrored light takes over the water's color in its own hue, like the
    // glow it comes from, and brightens it a little.
    if (waterLight > 0.0) {
        vec3 hue = waterGlow / waterLight;
        float cover = clamp(waterLight * 1.6, 0.0, 0.9);
        shadedColor.rgb = min(mix(shadedColor.rgb, hue, cover) + hue * cover * 0.25, vec3(1.0));
    }
    if (fieldEffectMode == 25) shadedColor.rgb = mix(shadedColor.rgb, min(warmLit, vec3(1.0)), clamp(fieldEffectPhase, 0.0, 1.0));
    // Drifting mist low over the ground, on the DS pixel grid. It covers
    // surfaces and sprites in its own graded color and thins overlays such
    // as shadows and water shimmer rather than brightening them.
    if (hasWorld && groundMist.a > 0.0) {
        vec2 p = floor(world.xz) + 0.5;
        // Pixels above the layer skip the drifting noise.
        float low = 1.0 - smoothstep(0.0, groundMistLayer.y, world.y - groundMistLayer.x);
        float mist = 0.0;
        if (low > 0.0) {
            float wisps = ValueNoise(p + vec2(7.0, 2.0) * fieldTime, 90.0) * 0.6 + ValueNoise(p + vec2(-4.0, 5.0) * fieldTime, 31.0) * 0.4;
            mist = clamp(groundMist.a * low * smoothstep(0.25, 0.75, wisps), 0.0, 1.0);
        }
        bool overlay = (fieldEffectMode >= 1 && fieldEffectMode <= 7) || fieldEffectMode == 17 || fieldEffectMode == 23 || fieldEffectMode == 24;
        if (overlay) shadedColor.a *= 1.0 - mist;
        else shadedColor.rgb = mix(shadedColor.rgb, Grade(groundMist.rgb), mist);
    }
    // Perspective clip W gives distance along the camera's view axis. Keep
    // near objects clear, and never replace the game's authored fog state.
    if (useFog == 0 && atmosphereFog.a > 0.0) {
        float viewDistance = 1.0 / max(reciprocalW, 0.000001);
        float haze = smoothstep(atmosphereRange.x, atmosphereRange.y, viewDistance) * atmosphereFog.a;
        shadedColor.rgb = mix(shadedColor.rgb, Grade(atmosphereFog.rgb), haze);
    }
    // Debug view of the emission buffer, plus the ground pools.
    if (fieldEmissionOnly != 0 && fieldEffectMode != 7) shadedColor.rgb = emitted;
    color = shadedColor;
    // Ground pools and other translucent glows neither emit nor occlude light.
    emission = vec4(emitted * fieldEmissionWeight, fieldEffectMode == 7 ? 0.0 : shadedColor.a);
    fogData = vec4(0.0);
    if(useFog == 1) {
        float fog_maxdist = 1.0;
        float fog_mindist = 0.5;
        float dist = length(coords.xyz);
        float fogFactor = 0.0;
        if(fogDepth < fogDepthBoundary[0]) {
            
            fogFactor = fogDensity[0];
        } else {
            int foundIdx = 31;
            for(int i=1; i<32; i++) {
                if(fogDepth < fogDepthBoundary[i]) {
                    foundIdx = i;
                    break;
                }
            }
            if(foundIdx == 31) {
                fogFactor = fogDensity[foundIdx];
            } else {
                float interpolationFactor = (fogDepthBoundary[foundIdx-1] - fogDepth) / (fogDepthBoundary[foundIdx] - fogDepthBoundary[foundIdx-1]);
                fogFactor = (fogDensity[foundIdx - 1] * (interpolationFactor)) + (fogDensity[foundIdx] * (1.0-interpolationFactor));
            }
        }
        fogData = vec4(fogColor.rgb, fogFactor);
    }
}
)""

