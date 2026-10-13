#ifndef LIBNTR_SIM_BLOOM_HPP
#define LIBNTR_SIM_BLOOM_HPP

#include <cmath>
#include <cstdio>
#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

// Blurs the 3D emission attachment into a small chain of half-resolution
// levels. The chain starts at one texel per DS pixel regardless of the
// internal resolution, so the halo radius and the cost of every pass after
// the first stay fixed. Level 0 holds the accumulated result.
class SIMBloom {
public:
    static constexpr int kLevels = 6;
    // Each coarser level contributes this much more than the finer one, which
    // widens the halo without blurring the source itself.
    static constexpr float kSpread = 1.6f;
    // Brightness of the halo at strength 1, relative to the emission.
    static constexpr float kGain = 3.0f;
    // Scales level 0 back to the emission's own range.
    static float Normalization()
    {
        float sum = 0, weight = 1;
        for (int i = 0; i < kLevels; i++, weight *= kSpread) sum += weight;
        return kGain / sum;
    }

private:
    GLuint program = 0, vertexArray = 0;
    GLuint textures[kLevels] = { }, framebuffers[kLevels] = { };
    int widths[kLevels] = { }, heights[kLevels] = { };
    GLint modeLocation = -1, texelLocation = -1, tapsLocation = -1;

    void Destroy()
    {
        glDeleteFramebuffers(kLevels, framebuffers);
        glDeleteTextures(kLevels, textures);
        for (int i = 0; i < kLevels; i++) {
            framebuffers[i] = textures[i] = 0;
            widths[i] = heights[i] = 0;
        }
    }

    void Pass(int mode, GLuint source, int sourceWidth, int sourceHeight, int target, int taps)
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[target]);
        glViewport(0, 0, widths[target], heights[target]);
        glBindTexture(GL_TEXTURE_2D, source);
        glProgramUniform1i(program, modeLocation, mode);
        glProgramUniform2f(program, texelLocation, 1.0f / sourceWidth, 1.0f / sourceHeight);
        glProgramUniform1i(program, tapsLocation, taps);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

public:
    bool Ready() const { return program != 0 && textures[0] != 0; }
    GLuint Texture() const { return textures[0]; }

    void Init(const char *vertexSource, const char *fragmentSource)
    {
        GLuint shaders[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
        const char *sources[2] = { vertexSource, fragmentSource };
        GLint status = GL_FALSE;
        for (int i = 0; i < 2; i++) {
            glShaderSource(shaders[i], 1, &sources[i], nullptr);
            glCompileShader(shaders[i]);
            glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
            if (status != GL_TRUE) {
                char log[1024];
                glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
                std::fprintf(stderr, "Bloom shader unavailable:\n%s\n", log);
            }
        }
        program = glCreateProgram();
        glAttachShader(program, shaders[0]);
        glAttachShader(program, shaders[1]);
        glLinkProgram(program);
        glDeleteShader(shaders[0]);
        glDeleteShader(shaders[1]);
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status != GL_TRUE) {
            std::fprintf(stderr, "Bloom disabled: shader link failed\n");
            glDeleteProgram(program);
            program = 0;
            return;
        }
        modeLocation = glGetUniformLocation(program, "mode");
        texelLocation = glGetUniformLocation(program, "sourceTexel");
        tapsLocation = glGetUniformLocation(program, "taps");
        glProgramUniform1i(program, glGetUniformLocation(program, "source"), 0);
        // Core profiles require a bound vertex array even without attributes.
        glGenVertexArrays(1, &vertexArray);
    }

    // nativeWidth/nativeHeight cover the whole 3D target in DS pixels.
    void Resize(int nativeWidth, int nativeHeight, int scale)
    {
        Destroy();
        if (!program) {
            return;
        }
        GLint texture, framebuffer;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        int w = nativeWidth, h = nativeHeight;
        glGenTextures(kLevels, textures);
        glGenFramebuffers(kLevels, framebuffers);
        for (int i = 0; i < kLevels; i++) {
            widths[i] = w > 1 ? w : 1;
            heights[i] = h > 1 ? h : 1;
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, widths[i], heights[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[i]);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
            const GLfloat clear[] = { 0, 0, 0, 0 };
            glClearBufferfv(GL_COLOR, 0, clear);
            w /= 2;
            h /= 2;
        }
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "Bloom disabled: half-float targets unavailable\n");
            Destroy();
        }
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    }

    // Uses texture unit 0 and restores every state it changes.
    void Run(GLuint emission, int emissionWidth, int emissionHeight)
    {
        if (!Ready()) {
            return;
        }
        GLint drawFramebuffer, program0, vertexArray0, activeTexture, texture, viewport[4];
        GLint srcRGB, dstRGB, srcAlpha, dstAlpha;
        GLfloat blendColor[4];
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program0);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray0);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegeri_v(GL_BLEND_SRC_RGB, 0, &srcRGB);
        glGetIntegeri_v(GL_BLEND_DST_RGB, 0, &dstRGB);
        glGetIntegeri_v(GL_BLEND_SRC_ALPHA, 0, &srcAlpha);
        glGetIntegeri_v(GL_BLEND_DST_ALPHA, 0, &dstAlpha);
        glGetFloatv(GL_BLEND_COLOR, blendColor);
        const GLboolean blend = glIsEnabledi(GL_BLEND, 0);
        const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean cull = glIsEnabled(GL_CULL_FACE);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean stencil = glIsEnabled(GL_STENCIL_TEST);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisablei(GL_BLEND, 0);
        glUseProgram(program);
        glBindVertexArray(vertexArray);

        // Average the whole footprint of each level-0 texel. Bilinear taps
        // each cover 2x2 source texels, so ratio/2 taps per axis suffice.
        float ratio = (float)emissionWidth / widths[0];
        int taps = (int)std::ceil(ratio / 2);
        Pass(0, emission, emissionWidth, emissionHeight, 0, taps < 1 ? 1 : taps);
        for (int i = 1; i < kLevels; i++) {
            Pass(1, textures[i - 1], widths[i - 1], heights[i - 1], i, 1);
        }
        glEnablei(GL_BLEND, 0);
        glBlendColor(kSpread, kSpread, kSpread, kSpread);
        glBlendFunci(0, GL_CONSTANT_COLOR, GL_ONE);
        for (int i = kLevels - 1; i > 0; i--) {
            Pass(2, textures[i], widths[i], heights[i], i - 1, 1);
        }

        glBlendColor(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
        glBlendFuncSeparatei(0, srcRGB, dstRGB, srcAlpha, dstAlpha);
        if (!blend) glDisablei(GL_BLEND, 0);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (cull) glEnable(GL_CULL_FACE);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (stencil) glEnable(GL_STENCIL_TEST);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(activeTexture);
        glBindVertexArray(vertexArray0);
        glUseProgram(program0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};

#endif
