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
    double sceneError = 0, codeError = 0;
    constexpr double white = 203;
    auto inputCode = [](int kind, double nits) {
        if (kind == 0) return pq(nits);
        if (kind == 1) return nits / 80.;
        if (kind == 2) return std::pow(nits / white, 1. / 2.2);
        const double scene = std::pow(nits / 1000., 1. / 1.2);
        return scene <= 1. / 12. ? std::sqrt(3. * scene) : .17883277 * std::log(12. * scene - .28466892) + .55991073;
    };
    for (const auto &native : nativeCases) {
        const double gain = native.kind == 2 ? 1. : native.percent / 100.;
        for (double nits : {0., .005, 1., 20., 80., 203., 400., 600., 1000.}) {
            if (native.kind == 2 && nits > white) continue;
            for (int mode : {0, 1, 2, 3}) {
                const float alpha = mode == 0 ? 1.f : .5f;
                double code = inputCode(native.kind, mode == 2 ? nits * alpha : nits);
                if (mode == 1) code *= alpha;
                const std::array<float,4> pixel{float(code),float(code),float(code),alpha};
                glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,input);
                glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_FLOAT,pixel.data());
                glBindFramebuffer(GL_FRAMEBUFFER,scene);
                glUniform1i(uniform("convert"),1);
                glUniform1i(uniform("decode"),native.decode);
                glUniform1i(uniform("alpha_mode"),mode);
                glUniform4fv(uniform("decode_params"),1,native.params);
                glUniformMatrix3fv(uniform("to_scene"),1,GL_FALSE,native.matrix);
                glUniform3fv(uniform("luma"),1,native.luma);
                glUniform4fv(uniform("tone"),1,native.tone);
                glUniform1f(uniform("pq_white"),0);
                glUniform1i(uniform("correct_gamut"),0);
                glDrawArrays(GL_TRIANGLES,0,3);
                std::array<float,4> stored{};glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,stored.data());
                require(glGetError()==GL_NO_ERROR,"native gain content draw/readback");
                const double expected = std::pow(std::min(nits*gain,1000.)/white,1./2.2)*alpha;
                // Includes native sampler/decode arithmetic and directed
                // FP16 rounding. Keep the bound below 0.25% of scene signal.
                for (int channel=0;channel<3;++channel) {
                    const double error=std::abs(stored[channel]-expected);
                    sceneError=std::max(sceneError,error);
                    if (error > std::max(std::abs(expected)*.0025,.000003)) {
                        std::fprintf(stderr,"gain %d kind %d alpha mode %d nits %.5f actual %.8f expected %.8f\n",native.percent,native.kind,mode,nits,stored[channel],expected);
                        require(false,"actual Rust SurfaceColor uniforms changed alpha/SDR/gain unexpectedly");
                    }
                }
                require(std::abs(stored[3]-alpha)<.00001,"HDR gain preserves alpha");
                if (mode != 0) continue;
                // Final PQ conversion stays unchanged; measure its error against
                // the real native FP16 scene, including native quantization.
                glBindTexture(GL_TEXTURE_2D,sceneTexture);
                glBindFramebuffer(GL_FRAMEBUFFER,output);
                glUniform1i(uniform("convert"),0);
                glUniform1i(uniform("alpha_mode"),0);
                glUniform1f(uniform("pq_white"),white);
                glUniformMatrix3fv(uniform("gamut"),1,GL_FALSE,to2020);
                glDrawArrays(GL_TRIANGLES,0,3);
                GLuint packed=0;glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_INT_2_10_10_10_REV,&packed);
                require(glGetError()==GL_NO_ERROR,"native gain PQ readback");
                for (int channel=0;channel<3;++channel) {
                    double light=0;
                    for(int i=0;i<3;++i)light+=to2020[i*3+channel]*signedPower(stored[i],2.2)*white;
                    const double encoded=pq(std::clamp(light,0.,10000.));
                    const double actual=((packed>>(channel*10))&1023)/1023.;
                    codeError=std::max(codeError,std::abs(actual-encoded)*1023);
                    require(std::abs(actual-encoded)*1023<2,"unchanged PQ encoder remains accurate");
                    require(unpq(actual)<1000.+8.,"HDR gain retains native 1000nit ceiling within RGB10/FP16 quantization");
                }
            }
        }
    }
    std::printf("Denial real SurfaceColor uniforms + native GLES PQ/scRGB/HLG .5/1/2/reset, SDR invariant and all alpha modes passed; FP16 max %.6f scene units, PQ %.3f codes (%s)\n",sceneError,codeError,glGetString(GL_RENDERER));
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context);eglTerminate(display);
}
