// Headless regression for fogged BG0 blending against the backdrop or an OBJ.
// From the libntr root:
// g++ -std=c++11 -Iinclude libraries/sim/tests/fog_compositor.cpp \
//     libraries/sim/src/glad.c -lEGL -o /tmp/fog_compositor
// /tmp/fog_compositor
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <simulator/glad/glad.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static const char *fragmentSource =
#include "../src/shaders/g2_fragment.glsl"
;
static const char *vertexSource = R"(#version 420 core
smooth out vec2 fragTexCoord;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    fragTexCoord = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

static GLuint Compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
        std::exit(1);
    }
    return shader;
}

int main() {
    auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!getDisplay) return 2;
    EGLDisplay display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (!eglInitialize(display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API)) return 2;
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
        EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count == 0) return 2;
    EGLint surfaceAttributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION,
        2, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (!eglMakeCurrent(display, surface, surface, context)
        || !gladLoadGLLoader(reinterpret_cast<GLADloadproc>(eglGetProcAddress))) return 2;

    GLuint program = glCreateProgram();
    glAttachShader(program, Compile(GL_VERTEX_SHADER, vertexSource));
    glAttachShader(program, Compile(GL_FRAGMENT_SHADER, fragmentSource));
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) return 3;
    glUseProgram(program);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 8, 8);

    // Texture units: world, fog, weather overlay, transparent, bottom OBJ.
    GLuint textures[5];
    glGenTextures(5, textures);
    unsigned char pixels[5][4] = {
        {32, 128, 32, 255}, {247, 247, 247, 249}, {255, 255, 255, 255},
        {0, 0, 0, 0}, {255, 0, 0, 255}
    };
    for (int i = 0; i < 5; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    auto location = [program](const char *name) { return glGetUniformLocation(program, name); };
    GLint order[] = {1, 0, 2, 3}, priorities[] = {2, 3, 1, 0};
    glUniform1iv(location("bgOrder"), 4, order);
    glUniform1iv(location("bgPriorities"), 4, priorities);
    glUniform1i(location("g3FogTexture"), 1);
    glUniform1i(location("objWindowTexture"), 3);
    glUniform1i(location("bg0as3d"), 1);
    glUniform1f(location("presentationWidth"), 448);
    glUniform1f(location("fieldWidth"), 448);
    glUniform1i(location("bldalpha"), 1801); // Route 210: EVA=9, EVB=7.
    glUniform4f(location("backdropColor"), 0, 0, 0, 1);

    for (int useObj = 0; useObj < 2; useObj++) {
        GLint objects[] = {3, 3, 3, useObj ? 4 : 3, 3};
        glUniform1iv(location("objTexture"), 5, objects);
        for (int weatherOverlay = 0; weatherOverlay < 2; weatherOverlay++) {
            GLint backgrounds[] = {0, 3, weatherOverlay ? 2 : 3, 3};
            glUniform1iv(location("bgTexture"), 4, backgrounds);
            glUniform1i(location("bldcnt"), useObj ? 0x3144 : 0x2144);
            for (int gym = 0; gym < 2; gym++) {
                float fogAlpha = (gym ? 1.f : 31.f) / 32.f;
                unsigned char fog[] = {static_cast<unsigned char>(gym ? 8 : 247),
                    static_cast<unsigned char>(gym ? 8 : 247),
                    static_cast<unsigned char>(gym ? 8 : 247), 219};
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, textures[1]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fog);
                glUniform1f(location("g3FogAlpha"), fogAlpha);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                unsigned char result[4];
                glReadPixels(4, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, result);
                float density = 219.f / 255.f;
                float alpha = 1.f + (fogAlpha - 1.f) * density;
                for (int channel = 0; channel < 3; channel++) {
                    float scene = pixels[0][channel] / 255.f;
                    scene += (fog[channel] / 255.f - scene) * density;
                    float underneath = useObj && channel == 0 ? 1.f : 0.f;
                    float expected = scene * alpha + underneath * (1.f - alpha);
                    if (weatherOverlay) expected = 9.f / 16.f + scene * 7.f / 16.f;
                    if (std::fabs(result[channel] / 255.f - expected) > .01f) {
                        std::fprintf(stderr, "FAIL: OBJ=%d overlay=%d gym=%d channel=%d: got %d expected %.3f\n",
                            useObj, weatherOverlay, gym, channel, result[channel], expected);
                        return 4;
                    }
                }
            }
        }
    }
    if (glGetError() != GL_NO_ERROR) return 5;
    std::puts("PASS: weather and gym fog blend against backdrop/OBJ with and without BG2 overlay");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
