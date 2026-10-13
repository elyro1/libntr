#ifndef LIBNTR_SIM_3D_TARGET_HPP
#define LIBNTR_SIM_3D_TARGET_HPP

#include <cstdio>
#ifdef SDK_BUILD_NX
#include <glad/glad.h>
#else
#include <simulator/glad/glad.h>
#endif

// Multisample only the scene, its fog attributes and its emission. The existing
// textures remain the resolve target consumed by the DS background compositor.
class SIM3DTarget {
    static constexpr int kColors = 3;
    GLuint framebuffer = 0;
    GLuint buffers[kColors + 1] = { };
    int width = 0, height = 0;
    int sampleCount = 0;

public:
    void Destroy()
    {
        glDeleteRenderbuffers(kColors + 1, buffers);
        glDeleteFramebuffers(1, &framebuffer);
        framebuffer = 0;
        sampleCount = 0;
        for (GLuint &buffer : buffers) {
            buffer = 0;
        }
    }

    GLuint DrawFramebuffer(GLuint fallback) const
    {
        return framebuffer ? framebuffer : fallback;
    }

    int Samples() const { return sampleCount; }

    bool Enabled() const
    {
        return framebuffer != 0;
    }

    void Resize(int w, int h, bool enabled)
    {
        GLint draw, read, renderbuffer;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        // Callers resize between frames, with the presentation target bound.
        Destroy();
        width = w;
        height = h;
        GLint maxSamples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        int samples = maxSamples < 4 ? maxSamples : 4;
        // Avoid multi-gigabyte targets at the 20x resolution setting. The
        // budget was 256 MiB for three attachments; scale it per attachment
        // so the emission buffer does not reduce the available sample count.
        const unsigned long long bytes = (unsigned long long)w * h * 4 * (kColors + 1);
        while (samples >= 2 && bytes * samples * 3 > (kColors + 1) * 256ull * 1024 * 1024) {
            samples /= 2;
        }
        if (enabled && samples >= 2) {
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glGenRenderbuffers(kColors + 1, buffers);
            for (int i = 0; i <= kColors; i++) {
                glBindRenderbuffer(GL_RENDERBUFFER, buffers[i]);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, i < kColors ? GL_RGBA8 : GL_DEPTH24_STENCIL8, width, height);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                    i < kColors ? GL_COLOR_ATTACHMENT0 + i : GL_DEPTH_STENCIL_ATTACHMENT,
                    GL_RENDERBUFFER,
                    buffers[i]);
            }
            const GLenum attachments[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
            glDrawBuffers(kColors, attachments);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                std::fprintf(stderr, "3D multisampling unavailable; using the standard target\n");
                Destroy();
                // Allocation failure is handled by the standard target.
                while (glGetError() != GL_NO_ERROR) { }
            } else {
                sampleCount = samples;
                GLboolean depthMask;
                GLint stencilMask;
                glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
                glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
                GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
                glDisable(GL_SCISSOR_TEST);
                const GLfloat clear[] = { 0, 0, 0, 0 };
                for (int i = 0; i < kColors; i++) {
                    glClearBufferfv(GL_COLOR, i, clear);
                }
                glDepthMask(GL_TRUE);
                glStencilMask(0xff);
                glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);
                glDepthMask(depthMask);
                glStencilMask(stencilMask);
                if (scissor) {
                    glEnable(GL_SCISSOR_TEST);
                }
            }
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    }

    void Resolve(GLuint target) const
    {
        if (!framebuffer) {
            return;
        }
        GLint draw, read;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
        for (int i = 0; i < kColors; i++) {
            glReadBuffer(GL_COLOR_ATTACHMENT0 + i);
            // ES requires attachment i to occupy draw-buffer slot i.
            GLenum destination[kColors] = { GL_NONE, GL_NONE, GL_NONE };
            destination[i] = GL_COLOR_ATTACHMENT0 + i;
            glDrawBuffers(kColors, destination);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        const GLenum attachments[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        glDrawBuffers(kColors, attachments);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        }
    }
};

#endif
