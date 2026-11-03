#ifndef LIBNTR_SIM_FIELD_COVERAGE_HPP
#define LIBNTR_SIM_FIELD_COVERAGE_HPP

#include <cstdio>
#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

// Repair once per native scene texel, before the display-resolution compositor.
// Keeping the neighborhood search out of that shader avoids penalizing
// ordinary composition, even when repair is disabled.
class SIMFieldCoverage {
    GLuint program = 0, vertexArray = 0, framebuffer = 0, textures[2] = {};
    int width = 0, height = 0;

public:
    bool Ready() const { return framebuffer != 0; }
    GLuint Color() const { return textures[0]; }
    GLuint Fog() const { return textures[1]; }

    void Destroy()
    {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(2, textures);
        glDeleteVertexArrays(1, &vertexArray);
        glDeleteProgram(program);
        framebuffer = program = vertexArray = textures[0] = textures[1] = 0;
    }

    void Init(const char *vertexSource, const char *fragmentSource, int w, int h)
    {
        width = w;
        height = h;
        GLuint shaders[] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
        const char *sources[] = { vertexSource, fragmentSource };
        for (int i = 0; i < 2; ++i) {
            glShaderSource(shaders[i], 1, &sources[i], nullptr);
            glCompileShader(shaders[i]);
        }
        program = glCreateProgram();
        for (GLuint shader : shaders) {
            glAttachShader(program, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(program);
        GLint status;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status != GL_TRUE) {
            char log[2048];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "Field coverage repair unavailable: %s\n", log);
            Destroy();
            return;
        }
        glProgramUniform1i(program, glGetUniformLocation(program, "sceneTexture"), 0);
        glProgramUniform1i(program, glGetUniformLocation(program, "fogTexture"), 1);
        GLint previousFramebuffer, previousTexture;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGenVertexArrays(1, &vertexArray);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glGenTextures(2, textures);
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, i == 0 ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, textures[i], 0);
        }
        const GLenum attachments[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, attachments);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "Field coverage repair unavailable: framebuffer incomplete\n");
            Destroy();
        }
        glBindTexture(GL_TEXTURE_2D, previousTexture);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFramebuffer);
    }

    void Run(GLuint scene, GLuint fog)
    {
        if (!Ready()) return;
        GLint previousFramebuffer, previousProgram, previousVertexArray, previousActive, previousTextures[2], viewport[4];
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
        glGetIntegerv(GL_VIEWPORT, viewport);
        const GLenum capabilities[] = { GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
        GLboolean enabled[4], blend[2], masks[2][4];
        for (int i = 0; i < 4; ++i) {
            enabled[i] = glIsEnabled(capabilities[i]);
            glDisable(capabilities[i]);
        }
        const GLuint inputs[] = { scene, fog };
        for (int i = 0; i < 2; ++i) {
            blend[i] = glIsEnabledi(GL_BLEND, i);
            glGetBooleani_v(GL_COLOR_WRITEMASK, i, masks[i]);
            glDisablei(GL_BLEND, i);
            glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTextures[i]);
            glBindTexture(GL_TEXTURE_2D, inputs[i]);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, width, height);
        glUseProgram(program);
        glBindVertexArray(vertexArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int i = 0; i < 2; ++i) {
            if (blend[i]) glEnablei(GL_BLEND, i);
            glColorMaski(i, masks[i][0], masks[i][1], masks[i][2], masks[i][3]);
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, previousTextures[i]);
        }
        for (int i = 0; i < 4; ++i) if (enabled[i]) glEnable(capabilities[i]);
        glActiveTexture(previousActive);
        glBindVertexArray(previousVertexArray);
        glUseProgram(previousProgram);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousFramebuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    }
};

#endif
