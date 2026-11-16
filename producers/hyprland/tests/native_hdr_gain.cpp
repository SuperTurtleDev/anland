// Compiled after actual upstream CM shader strings. No production GLSL changes.
#include "../anland_backend/hyprland/src/anland/ContentGain.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char *why) {
    if (!ok) { std::fprintf(stderr, "%s (GL %x)\n", why, glGetError()); std::exit(1); }
}
static GLuint compile(GLenum kind, const char *source) {
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[8192]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); }
    check(ok, "native CM shader compilation"); return shader;
}
static double pq(double nits) {
    double p = std::pow(nits / 10000., 2610. / 16384.);
    return std::pow((3424. / 4096. + 2413. / 128. * p) / (1. + 2392. / 128. * p), 2523. / 32.);
}
struct Settings {
    std::array<std::array<double, 3>, 3> convertMatrix{{{1,0,0},{0,1,0},{0,0,1}}};
    bool needsTonemap = false;
    float maxLuminance = 1000, dstMaxLuminance = 1000;
    int tonemapMode = 1;
};
int main() {
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    check(platform, "surfaceless EGL available");
    EGLDisplay d = platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    check(eglInitialize(d, nullptr, nullptr), "EGL initialized");
    EGLConfig config; EGLint n;
    EGLint attrs[] = {EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
    check(eglChooseConfig(d,attrs,&config,1,&n) && n, "GLES3 config");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext c = eglCreateContext(d,config,EGL_NO_CONTEXT,ca);
    check(c != EGL_NO_CONTEXT && eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,c), "GLES3 context");
    GLuint program=glCreateProgram();
    glAttachShader(program,compile(GL_VERTEX_SHADER,vertex));
    glAttachShader(program,compile(GL_FRAGMENT_SHADER,fragment));glLinkProgram(program);
    GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);check(ok,"CM shader linked");glUseProgram(program);
    auto uniform=[&](const char *name){return glGetUniformLocation(program,name);};
    GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA32F,1,1);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"real floating scene target");
    glViewport(0,0,1,1);glDisable(GL_DITHER);
    double worst=0;
    for(float gain:{.5f,1.f,2.f,1.f}) {
        // Excludes final output, internal copies, and untagged/SDR surfaces.
        check(Anland::clientHdrGain(true,true,true,false,gain)==1,"final output gain excluded");
        check(Anland::clientHdrGain(true,false,false,false,gain)==1,"converted internal image gain excluded");
        check(Anland::clientHdrGain(true,true,false,true,gain)==1,"internal copy gain excluded");
        for(int tf:{pqTF,scRGBTF,sdrTF}) {
            Settings settings;
            float applied=Anland::clientHdrGain(tf!=sdrTF,true,false,false,gain);
            Anland::applyHdrContentGain(settings,applied);
            GLfloat matrix[9];for(int col=0;col<3;++col)for(int row=0;row<3;++row)matrix[col*3+row]=settings.convertMatrix[row][col];
            glUniformMatrix3fv(uniform("uMatrix"),1,GL_FALSE,matrix);
            glUniform1i(uniform("uTF"),tf);
            glUniform2f(uniform("uRange"),0,tf==pqTF?10000:tf==scRGBTF?80:203);
            glUniform1f(uniform("uMax"),settings.maxLuminance);
            glUniform1f(uniform("uPeak"),settings.dstMaxLuminance);
            glUniform1i(uniform("uMode"),settings.tonemapMode);
            for(double nits:{0.,.005,1.,20.,80.,203.,400.,600.,1000.}) {
                if(tf==sdrTF && nits>203)continue;
                const float input=tf==pqTF?pq(nits):tf==scRGBTF?nits/80.:nits/203.<=.0031308?nits/203.*12.92:1.055*std::pow(nits/203.,1./2.4)-.055;
                glUniform4f(uniform("uPixel"),input,input,input,1);
                glDrawArrays(GL_TRIANGLES,0,3);
                std::array<float,4> actual{};glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,actual.data());
                check(glGetError()==GL_NO_ERROR,"native CM draw/readback");
                const double expected=tf==sdrTF?nits:std::min(nits*gain,1000.);
                const double error=std::abs(actual[0]-expected);worst=std::max(worst,error);
                if(!std::isfinite(actual[0]) || error>std::max(.04,expected*.0002)) {
                    std::fprintf(stderr,"gain %.2f tf %d input %.5f actual %.7f expected %.7f\n",gain,tf,nits,actual[0],expected);
                    check(false,"HDR linear gain/1000-nit cap, SDR unchanged");
                }
            }
        }
    }
    std::printf("Hyprland native CM shader PQ/scRGB .5/1/2/reset, SDR/black/1000-nit cap passed; max error %.5f nit (%s)\n",worst,glGetString(GL_RENDERER));
}
