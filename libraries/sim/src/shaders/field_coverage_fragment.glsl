R""(
#version 420 core

uniform sampler2D sceneTexture;
uniform sampler2D fogTexture;
layout (location = 0) out vec4 color;
layout (location = 1) out vec4 fog;

// T-junctions in the field mesh can leave a single uncovered raster sample
// after the GPU rounds edge endpoints to its subpixel grid. At native
// resolution that sample becomes a black DS pixel. Repair only an empty
// sample completely enclosed by opaque samples; silhouettes, larger openings
// and translucent effects retain their coverage. Pick the least-changing
// opposite pair to continue the surface's texture, including its fog state.
ivec2 sourcePixel(ivec2 p, vec4 scene) {
    if (scene.a != 0.0) return p;
    ivec2 size = textureSize(sceneTexture, 0);
    if (p.x < 1 || p.y < 1 || p.x >= size.x - 1 || p.y >= size.y - 1) return p;
    const ivec2 offsets[8] = ivec2[8](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
        ivec2(-1, -1), ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1));
    vec4 neighbors[8];
    for (int i = 0; i < 8; i++) {
        neighbors[i] = texelFetch(sceneTexture, p + offsets[i], 0);
        if (neighbors[i].a < 1.0) return p;
    }
    int best = 0;
    float difference = 4.0;
    for (int i = 0; i < 8; i += 2) {
        vec3 delta = neighbors[i].rgb - neighbors[i + 1].rgb;
        float d = dot(delta, delta);
        if (d < difference) { difference = d; best = i; }
    }
    return p + offsets[best];
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 scene = texelFetch(sceneTexture, p, 0);
    ivec2 source = sourcePixel(p, scene);
    color = source == p ? scene : texelFetch(sceneTexture, source, 0);
    fog = texelFetch(fogTexture, source, 0);
}
)""
