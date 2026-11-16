// Included after shader strings extracted from the actual pinned renderer.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "%s (GL %#x)\n", message, glGetError()); std::exit(1); }
}
static GLuint compile(GLenum kind, const char *source)
{
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); }
    require(ok, "native shader compilation failed");
    return shader;
}
static GLuint texture(GLenum format)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, format, 1, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}
static GLuint framebuffer(GLuint texture, int minimum, GLenum type)
{
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "incomplete real framebuffer");
    for (GLenum channel : {GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE}) {
        GLint bits = 0;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, channel, &bits);
        require(bits >= minimum, "driver lost target precision");
    }
    GLint actual = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &actual);
    require(GLenum(actual) == type, "driver changed framebuffer component type");
    return fbo;
}
static double pq(double nits)
{
    const double x = std::pow(nits / 10000.0, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * x) / (1.0 + 2392.0 / 128.0 * x), 2523.0 / 32.0);
}
static double unpq(double code)
{
    const double x = std::pow(code, 32.0 / 2523.0);
    return 10000.0 * std::pow(std::fmax(x - 3424.0 / 4096.0, 0.0) / (2413.0 / 128.0 - 2392.0 / 128.0 * x), 16384.0 / 2610.0);
}
static double signedPower(double value, double exponent)
{
    return std::copysign(std::pow(std::abs(value), exponent), value);
}
int main()
{
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    require(platform != nullptr, "surfaceless EGL is unavailable");
    EGLDisplay display = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    require(eglInitialize(display, nullptr, nullptr), "EGL initialization failed");
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    require(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "GLES3 config unavailable");
    require(eglBindAPI(EGL_OPENGL_ES_API), "GLES API unavailable");
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    require(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "GLES3 context failed");
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "native shader link failed");
    glUseProgram(program);
    auto uniform = [program](const char *name) { return glGetUniformLocation(program, name); };
    glUniform1i(uniform("source_texture"), 0);
    glUniform1i(uniform("look"), 1);
    glUniform1i(uniform("grade"), 0);
    glUniform1i(uniform("alpha_mode"), 0);
    // Preserve the source independently of the native FP16 scene, so this
    // test can attribute quantization to the actual scene attachment.
    GLuint input = texture(GL_RGBA32F);
    GLuint sceneTexture = texture(GL_RGBA16F);
    GLuint scene = framebuffer(sceneTexture, 16, GL_FLOAT);
    GLuint outputTexture = texture(GL_RGB10_A2);
    GLuint output = framebuffer(outputTexture, 10, GL_UNSIGNED_NORMALIZED);
    glViewport(0, 0, 1, 1);
    glDisable(GL_DITHER);
    // Column-major native linear sRGB <-> BT.2020 primaries, D65.
    const GLfloat to2020[] = {0.6274039f, 0.0690973f, 0.0163914f, 0.3292830f, 0.9195404f, 0.0880133f, 0.0433131f, 0.0113623f, 0.8955953f};
    const GLfloat toSrgb[] = {1.6604910f, -0.1245505f, -0.0181508f, -0.5876411f, 1.1328999f, -0.1005789f, -0.0728499f, -0.0083494f, 1.1187297f};
    GLfloat scaled[9];
    for (int i = 0; i < 9; ++i) scaled[i] = toSrgb[i] * (10000.0f / 203.0f);
    double maximumError = 0.0;
    double idealMaximumError = 0.0;
    double maximumBlackLeakNits = 0.0;
    auto check = [&](std::array<float, 4> pixel, bool hdrClient, std::array<double, 3> expected, double white) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
        if (hdrClient) {
            glBindFramebuffer(GL_FRAMEBUFFER, scene);
            glUniform1i(uniform("convert"), 1);
            glUniform1i(uniform("decode"), 3);
            glUniformMatrix3fv(uniform("to_scene"), 1, GL_FALSE, scaled);
            glUniform4f(uniform("tone"), 10000.0f / 203.0f, float(1000.0 / white), float(1000.0 / white), 1.0f);
            glUniform1f(uniform("pq_white"), 0.0f);
            glUniform1i(uniform("correct_gamut"), 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::array<float, 4> stored{};
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, stored.data());
            require(glGetError() == GL_NO_ERROR, "native FP16 scene readback failed");
            std::array<double, 3> idealLight{};
            for (int channel = 0; channel < 3; ++channel) {
                double linear = 0.0;
                for (int inputChannel = 0; inputChannel < 3; ++inputChannel)
                    linear += toSrgb[inputChannel * 3 + channel] * unpq(pixel[inputChannel]) / 203.0;
                idealLight[channel] = std::fmin(linear, 1000.0 / white);
                const double idealScene = signedPower(idealLight[channel], 1.0 / 2.2);
                // GLES permits directed half-float rounding. Bound one FP16
                // ULP plus shader arithmetic, without assuming CPU RNE.
                require(std::abs(stored[channel] - idealScene) <= std::fmax(std::abs(idealScene) * 0.0011, 0.000002),
                    "native content conversion lost HDR/negative scene precision");
            }
            // The final PQ encoder must faithfully encode the REAL native
            // FP16 scene, including its quantization. Signed gamma sRGB half
            // storage can produce small near-black cross-talk after the gamut
            // inverse; measure that separately against the ideal input below.
            for (int channel = 0; channel < 3; ++channel) {
                double linear = 0.0;
                for (int inputChannel = 0; inputChannel < 3; ++inputChannel)
                    linear += to2020[inputChannel * 3 + channel] * signedPower(stored[inputChannel], 2.2);
                const double nativeExpected = pq(std::clamp(linear * white, 0.0, 10000.0));
                expected[channel] = nativeExpected;
                double ideal = 0.0;
                for (int sourceChannel = 0; sourceChannel < 3; ++sourceChannel)
                    ideal += to2020[sourceChannel * 3 + channel] * idealLight[sourceChannel];
                // Native HDR content is reference-relative: changing desktop
                // white rescales it and clips at the unchanged 1000-nit peak.
                pixel[channel] = float(pq(std::clamp(ideal * white, 0.0, 10000.0)));
            }
            glBindTexture(GL_TEXTURE_2D, sceneTexture);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, output);
        glUniform1i(uniform("convert"), 0);
        glUniform1f(uniform("pq_white"), float(white));
        glUniformMatrix3fv(uniform("gamut"), 1, GL_FALSE, to2020);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::array<GLuint, 4> actual{};
        // GLES normalized RGB10 supports the packed 2:10:10:10 read format.
        GLuint packed = 0;
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &packed);
        require(glGetError() == GL_NO_ERROR, "native HDR draw/readback failed");
        actual = {packed & 1023u, (packed >> 10) & 1023u, (packed >> 20) & 1023u, packed >> 30};
        for (int channel = 0; channel < 3; ++channel) {
            const double error = std::abs(double(actual[channel]) - expected[channel] * 1023.0);
            maximumError = std::fmax(maximumError, error);
            if (error > 2.0) std::fprintf(stderr, "client=%d channel=%d actual=%u expected=%f error=%f\n", hdrClient, channel, actual[channel], expected[channel] * 1023.0, error);
            require(error <= 2.0, "native HDR pixel differs by more than two 10-bit codes");
            if (hdrClient) {
                const double idealCode = pixel[channel];
                idealMaximumError = std::fmax(idealMaximumError, std::abs(double(actual[channel]) - idealCode * 1023.0));
                if (idealCode < 0.000001)
                    maximumBlackLeakNits = std::fmax(maximumBlackLeakNits, unpq(double(actual[channel]) / 1023.0));
            }
        }
    };
    // Retune the same native FP16 scene and RGB10 scanout targets repeatedly;
    // no shader rebuild or storage replacement is involved in a live update.
    for (double white : {80.0, 203.0, 300.0, 400.0, 500.0, 203.0}) {
        for (double gray : {0.0, 0.001, 0.1, 0.5, 1.0}) {
            const double expected = pq(std::pow(gray, 2.2) * white);
            check({float(gray), float(gray), float(gray), 1.0f}, false, {expected, expected, expected}, white);
        }
        for (double nits : {0.0, 0.005, 1.0, 80.0, 203.0, 1000.0}) {
            const double code = pq(nits);
            check({float(code), float(code), float(code), 1.0f}, true, {code, code, code}, white);
        }
        // Negative sRGB scene components retain the native wide gamut. The
        // oracle includes headroom clipping at the newly selected reference.
        for (int primary = 0; primary < 3; ++primary)
            for (double nits : {0.005, 1.0, 80.0, 203.0, 500.0}) {
                std::array<float, 4> pixel{0, 0, 0, 1};
                std::array<double, 3> expected{};
                pixel[primary] = float(pq(nits));
                expected[primary] = pq(nits);
                check(pixel, true, expected, white);
            }
    }
    std::printf("Native Denial HDR GLES shader passed: FP16 content conversion + real RGB10 PQ encoder, SDR white 80/203/300/400/500/203 nit; encoder error %.3f codes (%s)\n", maximumError, glGetString(GL_RENDERER));
    std::printf("Native signed-gamma sRGB FP16 limitation: ideal input round-trip max %.3f codes; zero-channel gamut cross-talk %.6f nit\n", idealMaximumError, maximumBlackLeakNits);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
}
