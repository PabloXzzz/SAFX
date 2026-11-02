// SAFX64 - Fase 1: god rays solares (post-proceso) para GTA SA Android 2.10 (arm64)
// Se carga con AML desde la carpeta mods/ como libSAFX64.so

#include <mod/amlmod.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <string.h>
#include <math.h>

MYMOD(net.safx.visuals, SAFX Visuals, 0.1, SAFX)

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "SAFX", __VA_ARGS__)

// ------------------------- Ajustes (edita y recompila) -------------------------
static const float kRayIntensity = 0.55f;   // fuerza de los rayos
static const float kRayDensity   = 0.95f;   // largo del barrido hacia el sol (0..1)
static const float kThreshold    = 0.60f;   // brillo minimo que genera rayos
static const float kTint[3]      = { 1.00f, 0.92f, 0.78f }; // color calido
static const int   kHalfDiv      = 2;       // resolucion del buffer de rayos (2 = mitad)
// -------------------------------------------------------------------------------

static int*   g_curStored  = nullptr; // CTimeCycle::m_CurrentStoredValue
static float* g_vecToSun   = nullptr; // CTimeCycle::m_VectorToSun (CVector[16])
static float* g_sunScrX    = nullptr; // CCoronas::SunScreenX
static float* g_sunScrY    = nullptr; // CCoronas::SunScreenY
static bool*  g_sunBlocked = nullptr; // CCoronas::SunBlockedByClouds
static void (*RQ_Swap_orig)(char*&) = nullptr;

static bool   g_glReady = false, g_glFailed = false;
static GLuint g_progRays = 0, g_progComp = 0;
static GLuint g_sceneTex = 0, g_halfTex = 0, g_halfFbo = 0;
static int    g_w = 0, g_h = 0, g_hw = 0, g_hh = 0;
static GLint  uR_scene, uR_sun, uR_dens, uR_thr, uC_tex, uC_int, uC_tint;

static const GLfloat kTri[6] = { -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f };

static const char* kVS =
"attribute vec2 aPos; varying vec2 vUV;\n"
"void main(){ vUV = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }\n";

static const char* kFSRays =
"precision mediump float;\n"
"uniform sampler2D uScene; uniform vec2 uSun; uniform float uDens; uniform float uThr;\n"
"varying vec2 vUV;\n"
"void main(){\n"
"  vec2 delta = (uSun - vUV) * uDens / 24.0;\n"
"  vec2 uv = vUV; float w = 1.0; vec3 acc = vec3(0.0);\n"
"  for (int i = 0; i < 24; i++) {\n"
"    uv += delta;\n"
"    vec3 c = texture2D(uScene, uv).rgb;\n"
"    float l = dot(c, vec3(0.299,0.587,0.114));\n"
"    acc += c * max(l - uThr, 0.0) * w;\n"
"    w *= 0.95;\n"
"  }\n"
"  gl_FragColor = vec4(acc, 1.0);\n"
"}\n";

static const char* kFSComp =
"precision mediump float;\n"
"uniform sampler2D uTex; uniform float uInt; uniform vec3 uTint; varying vec2 vUV;\n"
"void main(){ gl_FragColor = vec4(texture2D(uTex, vUV).rgb * uTint * uInt, 1.0); }\n";

static GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(s, sizeof b, nullptr, b); LOG("shader error: %s", b); return 0; }
    return s;
}
static GLuint MakeProgram(const char* vs, const char* fs) {
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char b[512]; glGetProgramInfoLog(p, sizeof b, nullptr, b); LOG("link error: %s", b); return 0; }
    return p;
}

static void MakeTex(GLuint& t, GLenum fmt, int w, int h) {
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool InitGL(int w, int h) {
    if (!g_progRays) {
        g_progRays = MakeProgram(kVS, kFSRays);
        g_progComp = MakeProgram(kVS, kFSComp);
        if (!g_progRays || !g_progComp) return false;
        uR_scene = glGetUniformLocation(g_progRays, "uScene");
        uR_sun   = glGetUniformLocation(g_progRays, "uSun");
        uR_dens  = glGetUniformLocation(g_progRays, "uDens");
        uR_thr   = glGetUniformLocation(g_progRays, "uThr");
        uC_tex   = glGetUniformLocation(g_progComp, "uTex");
        uC_int   = glGetUniformLocation(g_progComp, "uInt");
        uC_tint  = glGetUniformLocation(g_progComp, "uTint");
        glGenFramebuffers(1, &g_halfFbo);
    }
    g_w = w; g_h = h; g_hw = w / kHalfDiv; g_hh = h / kHalfDiv;
    MakeTex(g_sceneTex, GL_RGB,  g_w,  g_h);
    MakeTex(g_halfTex,  GL_RGBA, g_hw, g_hh);
    glBindFramebuffer(GL_FRAMEBUFFER, g_halfFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_halfTex, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) LOG("FBO incompleto");
    return ok;
}

struct GLState {
    GLint prog, fbo, tex, activeTex, arrayBuf, vp[4];
    GLint a0en, a0size, a0type, a0norm, a0stride, a0buf; void* a0ptr;
    GLint blendSrcRGB, blendDstRGB, blendSrcA, blendDstA;
    GLboolean blend, depth, cull, scissor, stencil, depthMask, colorMask[4];
};
static void Save(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.a0en);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s.a0size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s.a0type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s.a0norm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s.a0stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s.a0buf);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s.a0ptr);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRGB);   glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcA);   glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstA);
    s.blend = glIsEnabled(GL_BLEND); s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE); s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask); glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
}
static void Restore(const GLState& s) {
    glBindBuffer(GL_ARRAY_BUFFER, s.a0buf);
    glVertexAttribPointer(0, s.a0size, s.a0type, s.a0norm, s.a0stride, s.a0ptr);
    if (s.a0en) glEnableVertexAttribArray(0); else glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuf);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glUseProgram(s.prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s.tex);
    glActiveTexture(s.activeTex);
    glBlendFuncSeparate(s.blendSrcRGB, s.blendDstRGB, s.blendSrcA, s.blendDstA);
    auto set = [](GLenum cap, GLboolean on){ if (on) glEnable(cap); else glDisable(cap); };
    set(GL_BLEND, s.blend); set(GL_DEPTH_TEST, s.depth); set(GL_CULL_FACE, s.cull);
    set(GL_SCISSOR_TEST, s.scissor); set(GL_STENCIL_TEST, s.stencil);
    glDepthMask(s.depthMask); glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
}

static void DrawTri() {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kTri);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void GetScreenSize(int& w, int& h) {
    EGLDisplay d = eglGetCurrentDisplay(); EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    w = h = 0;
    if (d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) { eglQuerySurface(d, s, EGL_WIDTH, &w); eglQuerySurface(d, s, EGL_HEIGHT, &h); }
    if (w <= 0 || h <= 0) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); w = vp[2]; h = vp[3]; }
}

static void RenderRays() {
    int w, h; GetScreenSize(w, h);
    if (w < 64 || h < 64) return;

    const float* v = g_vecToSun + (*g_curStored & 15) * 3;
    float sunUp = fminf(fmaxf(v[2] * 3.0f, 0.0f), 1.0f);
    float blocked = *g_sunBlocked ? 0.0f : 1.0f;

    float u = *g_sunScrX / (float)w, vv = 1.0f - (*g_sunScrY / (float)h);

    static int frame = 0;
    if ((frame++ % 300) == 0)
        LOG("sun scr=(%.1f,%.1f) uv=(%.2f,%.2f) vec=(%.2f,%.2f,%.2f) idx=%d blocked=%d screen=%dx%d",
            *g_sunScrX, *g_sunScrY, u, vv, v[0], v[1], v[2], *g_curStored, (int)*g_sunBlocked, w, h);

    float ex = fmaxf(fmaxf(-u, u - 1.0f), 0.0f), ey = fmaxf(fmaxf(-vv, vv - 1.0f), 0.0f);
    float edge = 1.0f - fminf(fmaxf((ex > ey ? ex : ey) / 0.3f, 0.0f), 1.0f);
    float k = kRayIntensity * sunUp * blocked * edge;
    if (k < 0.01f) return;

    if (!g_glReady || w != g_w || h != g_h) {
        GLState st0; Save(st0);
        g_glReady = InitGL(w, h);
        Restore(st0);
        if (!g_glReady) { g_glFailed = true; return; }
    }

    GLState st; Save(st);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_sceneTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glDepthMask(GL_FALSE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glBindFramebuffer(GL_FRAMEBUFFER, g_halfFbo);
    glViewport(0, 0, g_hw, g_hh);
    glUseProgram(g_progRays);
    glUniform1i(uR_scene, 0);
    glUniform2f(uR_sun, u, vv);
    glUniform1f(uR_dens, kRayDensity);
    glUniform1f(uR_thr, kThreshold);
    DrawTri();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, g_halfTex);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(g_progComp);
    glUniform1i(uC_tex, 0);
    glUniform1f(uC_int, k);
    glUniform3f(uC_tint, kTint[0], kTint[1], kTint[2]);
    DrawTri();

    Restore(st);
    while (glGetError() != GL_NO_ERROR) {}
}

static void RQ_Swap_hook(char*& cmd) {
    if (!g_glFailed) RenderRays();
    RQ_Swap_orig(cmd);
}

ON_MOD_PRELOAD() { LOG("SAFX preload"); }

ON_MOD_LOAD() {
    void* h = (void*)aml->GetLibHandle("libGTASA.so");
    if (!h) { LOG("no se encontro libGTASA.so"); return; }

    g_curStored  = (int*)  aml->GetSym(h, "_ZN10CTimeCycle20m_CurrentStoredValueE");
    g_vecToSun   = (float*)aml->GetSym(h, "_ZN10CTimeCycle13m_VectorToSunE");
    g_sunScrX    = (float*)aml->GetSym(h, "_ZN8CCoronas10SunScreenXE");
    g_sunScrY    = (float*)aml->GetSym(h, "_ZN8CCoronas10SunScreenYE");
    g_sunBlocked = (bool*) aml->GetSym(h, "_ZN8CCoronas18SunBlockedByCloudsE");
    void* swap   = (void*) aml->GetSym(h, "_Z24RQ_Command_rqSwapBuffersRPc");

    if (!g_curStored || !g_vecToSun || !g_sunScrX || !g_sunScrY || !g_sunBlocked || !swap) {
        LOG("faltan simbolos, plugin desactivado");
        return;
    }
    aml->Hook(swap, (void*)RQ_Swap_hook, (void**)&RQ_Swap_orig);
    LOG("SAFX activo: hook en RQ_Command_rqSwapBuffers");
}
