R""(
#version 420 core

uniform sampler2D source;
uniform vec2 sourceTexel;
// 0: prefilter the full-resolution emission, 1: downsample, 2: upsample.
uniform int mode;
uniform int taps;
smooth in vec2 uv;
layout (location = 0) out vec4 color;

void main()
{
    vec3 sum = vec3(0.0);
    if (mode == 0) {
        // Box-average this texel's footprint; tap spacing keeps every source
        // texel inside one bilinear sample at any internal resolution.
        vec2 footprint = sourceTexel * float(taps) * 2.0;
        for (int y = 0; y < taps; y++) {
            for (int x = 0; x < taps; x++) {
                vec2 offset = ((vec2(x, y) + 0.5) / float(taps) - 0.5) * footprint;
                sum += texture(source, uv + offset).rgb;
            }
        }
        sum /= float(taps * taps);
    } else if (mode == 1) {
        // Dual-filter downsample.
        vec2 h = sourceTexel;
        sum = texture(source, uv).rgb * 4.0;
        sum += texture(source, uv - h).rgb;
        sum += texture(source, uv + h).rgb;
        sum += texture(source, uv + vec2(h.x, -h.y)).rgb;
        sum += texture(source, uv - vec2(h.x, -h.y)).rgb;
        sum /= 8.0;
    } else {
        // Dual-filter upsample; blending adds it to the finer level.
        vec2 h = sourceTexel * 0.5;
        sum = texture(source, uv + vec2(-2.0 * h.x, 0.0)).rgb;
        sum += texture(source, uv + vec2(-h.x, h.y)).rgb * 2.0;
        sum += texture(source, uv + vec2(0.0, 2.0 * h.y)).rgb;
        sum += texture(source, uv + vec2(h.x, h.y)).rgb * 2.0;
        sum += texture(source, uv + vec2(2.0 * h.x, 0.0)).rgb;
        sum += texture(source, uv + vec2(h.x, -h.y)).rgb * 2.0;
        sum += texture(source, uv + vec2(0.0, -2.0 * h.y)).rgb;
        sum += texture(source, uv + vec2(-h.x, -h.y)).rgb * 2.0;
        sum /= 12.0;
    }
    color = vec4(sum, 1.0);
}
)""
