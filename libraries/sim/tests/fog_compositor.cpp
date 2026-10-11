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
#include <vector>

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
    GLuint textures[7];
    glGenTextures(7, textures);
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
    // The compositor reads window masks and blending registers per scanline.
    // Enable every layer and color effects for the whole fixture.
    std::vector<unsigned char> masks(448 * 384 * 2, 63);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, textures[5]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8UI, 448, 384, 0, GL_RG_INTEGER, GL_UNSIGNED_BYTE, masks.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(location("windowMaskTexture"), 5);
    std::vector<unsigned short> blend(384 * 4);
    for (int row = 0; row < 384; row++) {
        blend[row * 4 + 1] = 1801; // Route 210: EVA=9, EVB=7.
        blend[row * 4 + 3] = 2 | (3 << 2) | (1 << 4); // BG priorities.
    }
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, textures[6]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, 1, 384, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, blend.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(location("scanlineBlendTexture"), 6);
    glUniform1i(location("g3FogTexture"), 1);
    glUniform1i(location("fieldParticlesTexture"), 7);
    glUniform1i(location("objWindowTexture"), 3);
    glUniform1i(location("bg0as3d"), 1);
    glUniform1f(location("presentationWidth"), 448);
    glUniform1f(location("fieldWidth"), 448);
    glUniform4f(location("backdropColor"), 0, 0, 0, 1);

    for (int useObj = 0; useObj < 2; useObj++) {
        GLint objects[] = {3, 3, 3, useObj ? 4 : 3, 3};
        glUniform1iv(location("objTexture"), 5, objects);
        for (int weatherOverlay = 0; weatherOverlay < 2; weatherOverlay++) {
            GLint backgrounds[] = {0, 3, weatherOverlay ? 2 : 3, 3};
            glUniform1iv(location("bgTexture"), 4, backgrounds);
            for (int row = 0; row < 384; row++) blend[row * 4] = useObj ? 0x3144 : 0x2144;
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, textures[6]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 384, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, blend.data());
            // Weather, original gym, fully dark gym, weather again (leaving
            // the gym), then a fog-free flashlight pixel in the dark gym.
            for (int fogCase = 0; fogCase < 5; fogCase++) {
                bool gym = fogCase == 1 || fogCase == 2 || fogCase == 4;
                bool darkGym = fogCase == 2 || fogCase == 4;
                float fogAlpha = (gym ? 1.f : 31.f) / 32.f;
                unsigned char fogColor = darkGym ? 0 : (gym ? 8 : 247);
                unsigned char fogDensity = fogCase == 4 ? 0 : (darkGym ? 255 : 219);
                unsigned char fog[] = {fogColor, fogColor, fogColor, fogDensity};
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, textures[1]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fog);
                glUniform1f(location("g3FogAlpha"), fogAlpha);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                unsigned char result[4];
                glReadPixels(4, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, result);
                float density = fogDensity / 255.f;
                float alpha = 1.f + (fogAlpha - 1.f) * density;
                for (int channel = 0; channel < 3; channel++) {
                    float scene = pixels[0][channel] / 255.f;
                    scene += (fog[channel] / 255.f - scene) * density;
                    float underneath = useObj && channel == 0 ? 1.f : 0.f;
                    float expected = scene * alpha + underneath * (1.f - alpha);
                    if (weatherOverlay) expected = 9.f / 16.f + scene * 7.f / 16.f;
                    if (std::fabs(result[channel] / 255.f - expected) > .01f) {
                        std::fprintf(stderr, "FAIL: OBJ=%d overlay=%d fogCase=%d channel=%d: got %d expected %.3f\n",
                            useObj, weatherOverlay, fogCase, channel, result[channel], expected);
                        return 4;
                    }
                }
            }
        }
    }
    if (glGetError() != GL_NO_ERROR) return 5;
    std::puts("PASS: route fog, dark gym, flashlight reveal, and gym-to-weather transition with backdrop/OBJ and BG2 overlay");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
