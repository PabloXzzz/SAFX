// SAFX64 v0.2 - efectos graficos para GTA SA Android 2.10 (arm64) con AML
// God rays modernos, bloom, flare solar, lluvia en pantalla, viento en vegetacion, agua con fresnel/reflejo/brillo solar.
// Configuracion: archivo SAFX.ini (se crea solo la primera vez, ver logcat tag SAFX para su ruta).

#include <mod/amlmod.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <string>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

MYMOD(net.safx.visuals, SAFX Visuals, 0.2, SAFX)

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "SAFX", __VA_ARGS__)

// =============================== CONFIG ===============================
struct Cfg {
    float godRays = 1, bloom = 1, flare = 1, rain = 1, wind = 1, water = 1;
    float rayInt = 0.50f, rayDens = 0.90f, rayThr = 0.72f;
    float bloomInt = 0.28f, bloomThr = 0.68f;
    float flareInt = 0.35f;
    float rainInt = 1.0f;
    float treeSway = 0.30f, grassGain = 1.7f;
    float waterFres = 0.70f, waterSpec = 1.0f;
    float sat = 1.05f, contrast = 1.03f;
    float tintR = 1.00f, tintG = 0.93f, tintB = 0.80f;
} C;

struct KV { const char* k; float* v; };
static KV kKeys[] = {
    {"GodRays",&C.godRays},{"Bloom",&C.bloom},{"SunFlare",&C.flare},{"Rain",&C.rain},{"Wind",&C.wind},{"Water",&C.water},
    {"RayIntensity",&C.rayInt},{"RayDensity",&C.rayDens},{"RayThreshold",&C.rayThr},
    {"BloomIntensity",&C.bloomInt},{"BloomThreshold",&C.bloomThr},
    {"FlareIntensity",&C.flareInt},{"RainIntensity",&C.rainInt},
    {"TreeSway",&C.treeSway},{"GrassWindGain",&C.grassGain},
    {"WaterFresnel",&C.waterFres},{"WaterSpecular",&C.waterSpec},
    {"Saturation",&C.sat},{"Contrast",&C.contrast},
    {"SunTintR",&C.tintR},{"SunTintG",&C.tintG},{"SunTintB",&C.tintB},
};

static void LoadConfig() {
    char pkg[128] = "com.rockstargames.gtasa";
    FILE* c = fopen("/proc/self/cmdline", "r");
    if (c) { char b[128] = {0}; if (fgets(b, sizeof b, c) && b[0]) snprintf(pkg, sizeof pkg, "%s", b); fclose(c); }
    char path[256];
    snprintf(path, sizeof path, "/storage/emulated/0/Android/data/%s/SAFX.ini", pkg);
    FILE* f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# SAFX64 - 1 = activado, 0 = desactivado\n");
            for (auto& kv : kKeys) fprintf(f, "%s=%.3f\n", kv.k, *kv.v);
            fclose(f);
            LOG("SAFX.ini creado en %s", path);
        } else LOG("no pude crear %s, usando valores por defecto", path);
        return;
    }
    char line[160];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char* key = line; char* val = eq + 1;
        while (*key == ' ') key++;
        for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        for (auto& kv : kKeys) if (!strcmp(kv.k, key)) *kv.v = (float)atof(val);
    }
    fclose(f);
    LOG("SAFX.ini leido de %s", path);
}

// =============================== GAME POINTERS ===============================
static int*   pCurStored = nullptr;
static float* pVecToSun = nullptr;
static float* pSunX = nullptr; static float* pSunY = nullptr;
static bool*  pSunBlocked = nullptr;
static float* pRain = nullptr; static float* pWind = nullptr; static float* pWindDir = nullptr;
static float* pTunnel = nullptr; static float* pUnder = nullptr;
static void*  pTheCamera = nullptr;
typedef bool (*CalcScreenFn)(const float*, float*, float*, float*, bool, bool);
static CalcScreenFn CalcScreen = nullptr;

typedef void     (*SwapFn)(char*&);
typedef uint64_t (*BuildFn)(void*, const char*, const char*);
typedef void     (*WindFn)(float);
static SwapFn  Swap_orig = nullptr;
static BuildFn Build_orig = nullptr;
static WindFn  Wind_orig = nullptr;

static int g_nWater = 0, g_nWind = 0, g_nPatchFail = 0;

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static inline float smooth(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static double NowSec() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

// =============================== LECTURA SEGURA / CAMARA ===============================
static int g_pipe[2] = {-1, -1};
static bool SafeRead(const void* src, void* dst, size_t n) {
    if (g_pipe[0] < 0) {
        if (pipe(g_pipe)) return false;
        fcntl(g_pipe[0], F_SETFL, O_NONBLOCK); fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
    }
    ssize_t w = write(g_pipe[1], src, n);
    if (w != (ssize_t)n) { char tmp[64]; while (read(g_pipe[0], tmp, sizeof tmp) > 0) {} return false; }
    return read(g_pipe[0], dst, n) == (ssize_t)n;
}
static bool BasisOK(const float* m) {
    auto len = [&](int o) { return sqrtf(m[o]*m[o] + m[o+1]*m[o+1] + m[o+2]*m[o+2]); };
    auto dot = [&](int a, int b) { return m[a]*m[b] + m[a+1]*m[b+1] + m[a+2]*m[b+2]; };
    for (int o : {0, 4, 8}) { float l = len(o); if (!(l > 0.97f && l < 1.03f)) return false; }
    return fabsf(dot(0, 8)) < 0.05f && fabsf(dot(4, 8)) < 0.05f && fabsf(dot(0, 4)) < 0.05f;
}
// Devuelve la matriz de la camara (right@0, top@4, at@8, pos@12 en floats). Busca el puntero y lo valida.
static bool ReadCamera(float* m) {
    if (!pTheCamera) return false;
    static int off = -1;
    uintptr_t base = (uintptr_t)pTheCamera;
    auto tryOff = [&](int o) {
        uintptr_t p = 0;
        if (!SafeRead((const void*)(base + o), &p, sizeof p)) return false;
        if (p < 0x10000 || (p & 3)) return false;
        if (!SafeRead((const void*)p, m, 64)) return false;
        return BasisOK(m);
    };
    if (off >= 0) return tryOff(off);
    for (int o = 8; o <= 0x58; o += 8) if (tryOff(o)) { off = o; LOG("matriz de camara encontrada en TheCamera+0x%x", o); return true; }
    return false;
}

// =============================== SHADERS DE POST-PROCESO ===============================
static const char* kVS = R"GLSL(
attribute vec2 aPos; varying vec2 vUV;
void main(){ vUV = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }
)GLSL";

static const char* kFSBright = R"GLSL(
precision mediump float;
uniform sampler2D uTex; uniform float uThr; varying vec2 vUV;
void main(){
  vec3 c = texture2D(uTex, vUV).rgb;
  float l = max(c.r, max(c.g, c.b));
  float m = smoothstep(uThr, uThr + 0.22, l);
  gl_FragColor = vec4(c * m, 1.0);
}
)GLSL";

static const char* kFSRays = R"GLSL(
precision mediump float;
uniform sampler2D uTex; uniform vec2 uSun; uniform float uDens; varying vec2 vUV;
void main(){
  vec2 delta = (uSun - vUV) * uDens / 28.0;
  vec2 uv = vUV; float w = 1.0; float ws = 0.0; vec3 acc = vec3(0.0);
  for (int i = 0; i < 28; i++) {
    uv += delta;
    acc += texture2D(uTex, uv).rgb * w; ws += w; w *= 0.96;
  }
  gl_FragColor = vec4(acc / ws, 1.0);
}
)GLSL";

static const char* kFSBlur = R"GLSL(
precision mediump float;
uniform sampler2D uTex; uniform vec2 uDir; varying vec2 vUV;
void main(){
  vec3 c = texture2D(uTex, vUV).rgb * 0.2270;
  c += texture2D(uTex, vUV + uDir*1.3846).rgb * 0.3162;
  c += texture2D(uTex, vUV - uDir*1.3846).rgb * 0.3162;
  c += texture2D(uTex, vUV + uDir*3.2308).rgb * 0.0703;
  c += texture2D(uTex, vUV - uDir*3.2308).rgb * 0.0703;
  gl_FragColor = vec4(c, 1.0);
}
)GLSL";

static const char* kFSFinal = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uScene; uniform sampler2D uRays; uniform sampler2D uBloom;
uniform vec2 uSun;
uniform vec4 uA;      // x=rayI  y=bloomI  z=flareI  w=rain
uniform vec4 uB;      // x=time  y=aspect  z=saturation  w=contrast
uniform vec3 uTint;
varying vec2 vUV;

float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float rainLayer(vec2 uv, float scale, float speed, float slant){
  vec2 p = vec2((uv.x + uv.y * slant) * scale * uB.y, uv.y * scale * 0.30 + uB.x * speed);
  vec2 id = floor(p); vec2 f = fract(p);
  float h = hash(id);
  float present = step(0.80, h);
  float cx = 0.25 + 0.5 * hash(id + 3.7);
  float line = smoothstep(0.06, 0.0, abs(f.x - cx));
  return present * line * pow(f.y, 4.0);
}

void main(){
  vec3 scene = texture2D(uScene, vUV).rgb;
  vec2 asp = vec2(uB.y, 1.0);
  vec2 pc = (vUV - 0.5) * asp;
  vec2 sc = (uSun - 0.5) * asp;

  // god rays: se atenuan con la distancia al sol y no ciegan al mirarlo de frente
  float dsun = length(pc - sc);
  vec3 rays = texture2D(uRays, vUV).rgb;
  rays = pow(rays, vec3(1.4)) * exp(-dsun * 2.0) * uA.x * uTint;
  float centerDim = mix(0.45, 1.0, smoothstep(0.0, 0.35, length(sc)));
  rays *= centerDim;

  vec3 bloom = texture2D(uBloom, vUV).rgb * uA.y;

  // flare: halo suave + 3 fantasmas sobre el eje sol-centro
  float fl = exp(-dsun * 7.0) * 0.50;
  fl += smoothstep(0.075, 0.020, length(pc + sc * 0.45)) * 0.16;
  fl += smoothstep(0.110, 0.040, length(pc + sc * 0.85)) * 0.10;
  fl += smoothstep(0.050, 0.015, length(pc + sc * 1.45)) * 0.14;
  fl *= smoothstep(0.0, 0.25, length(sc)) * 0.6 + 0.4;
  vec3 flare = fl * uA.z * vec3(1.0, 0.86, 0.66);

  vec3 glow = min(rays + bloom + flare, vec3(0.70));
  vec3 col = scene + (1.0 - scene) * glow;   // mezcla "screen": nunca se quema

  if (uA.w > 0.002) {
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(col, vec3(lum) * vec3(0.93, 0.97, 1.04), 0.20 * uA.w);
    col *= 1.0 - 0.10 * uA.w;
    float r = rainLayer(vUV, 60.0, 5.0, 0.18) * 0.55
            + rainLayer(vUV, 110.0, 7.5, 0.20) * 0.40
            + rainLayer(vUV, 190.0, 10.0, 0.22) * 0.30;
    col += r * uA.w * vec3(0.72, 0.80, 0.90) * 0.55;
  }

  float l2 = dot(col, vec3(0.299, 0.587, 0.114));
  col = mix(vec3(l2), col, uB.z);
  col = (col - 0.5) * uB.w + 0.5;
  gl_FragColor = vec4(col, 1.0);
}
)GLSL";

// =============================== GL ===============================
struct Prog { GLuint id = 0; GLint a = -1, b = -1, c = -1, d = -1, e = -1, f = -1, g = -1; };
static Prog pBright, pRays, pBlur, pFinal;
static GLuint tScene = 0, tBright = 0, tRays = 0, tBlurA = 0, tBlurB = 0, gFbo = 0;
static int gW = 0, gH = 0, hW = 0, hH = 0, qW = 0, qH = 0;
static bool gReady = false, gFailed = false;
static const GLfloat kTri[6] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };

static GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(s, sizeof b, nullptr, b); LOG("shader error: %s", b); return 0; }
    return s;
}
static GLuint Link(const char* vs, const char* fs) {
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glBindAttribLocation(p, 0, "aPos"); glLinkProgram(p);
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
static void Target(GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, gFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
}
static bool InitGL(int w, int h) {
    if (!pBright.id) {
        pBright.id = Link(kVS, kFSBright); pRays.id = Link(kVS, kFSRays);
        pBlur.id = Link(kVS, kFSBlur);     pFinal.id = Link(kVS, kFSFinal);
        if (!pBright.id || !pRays.id || !pBlur.id || !pFinal.id) return false;
        pBright.a = glGetUniformLocation(pBright.id, "uTex"); pBright.b = glGetUniformLocation(pBright.id, "uThr");
        pRays.a = glGetUniformLocation(pRays.id, "uTex"); pRays.b = glGetUniformLocation(pRays.id, "uSun"); pRays.c = glGetUniformLocation(pRays.id, "uDens");
        pBlur.a = glGetUniformLocation(pBlur.id, "uTex"); pBlur.b = glGetUniformLocation(pBlur.id, "uDir");
        pFinal.a = glGetUniformLocation(pFinal.id, "uScene"); pFinal.b = glGetUniformLocation(pFinal.id, "uRays");
        pFinal.c = glGetUniformLocation(pFinal.id, "uBloom"); pFinal.d = glGetUniformLocation(pFinal.id, "uSun");
        pFinal.e = glGetUniformLocation(pFinal.id, "uA");     pFinal.f = glGetUniformLocation(pFinal.id, "uB");
        pFinal.g = glGetUniformLocation(pFinal.id, "uTint");
        glGenFramebuffers(1, &gFbo);
    }
    gW = w; gH = h; hW = w / 2; hH = h / 2; qW = w / 4; qH = h / 4;
    MakeTex(tScene, GL_RGB, gW, gH);
    MakeTex(tBright, GL_RGBA, hW, hH); MakeTex(tRays, GL_RGBA, hW, hH);
    MakeTex(tBlurA, GL_RGBA, qW, qH);  MakeTex(tBlurB, GL_RGBA, qW, qH);
    GLuint ts[4] = { tBright, tRays, tBlurA, tBlurB };
    for (GLuint t : ts) {
        Target(t, 16, 16);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { LOG("FBO incompleto"); return false; }
    }
    return true;
}

struct GLState {
    GLint prog, fbo, tex[3], activeTex, arrayBuf, vp[4];
    GLint a0en, a0size, a0type, a0norm, a0stride, a0buf; void* a0ptr;
    GLint bSrcRGB, bDstRGB, bSrcA, bDstA;
    GLboolean blend, depth, cull, scissor, stencil, depthMask, colorMask[4];
};
static void Save(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    for (int i = 0; i < 3; i++) { glActiveTexture(GL_TEXTURE0 + i); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex[i]); }
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuf);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.a0en);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s.a0size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s.a0type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s.a0norm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s.a0stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s.a0buf);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s.a0ptr);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrcRGB); glGetIntegerv(GL_BLEND_DST_RGB, &s.bDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA); glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    s.blend = glIsEnabled(GL_BLEND); s.depth = glIsEnabled(GL_DEPTH_TEST); s.cull = glIsEnabled(GL_CULL_FACE);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST); s.stencil = glIsEnabled(GL_STENCIL_TEST);
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
    for (int i = 0; i < 3; i++) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, s.tex[i]); }
    glActiveTexture(s.activeTex);
    glBlendFuncSeparate(s.bSrcRGB, s.bDstRGB, s.bSrcA, s.bDstA);
    auto set = [](GLenum cap, GLboolean on) { if (on) glEnable(cap); else glDisable(cap); };
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
static void ScreenSize(int& w, int& h) {
    EGLDisplay d = eglGetCurrentDisplay(); EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    w = h = 0;
    if (d != EGL_NO_DISPLAY && s != EGL_NO_SURFACE) { eglQuerySurface(d, s, EGL_WIDTH, &w); eglQuerySurface(d, s, EGL_HEIGHT, &h); }
    if (w <= 0 || h <= 0) { GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp); w = vp[2]; h = vp[3]; }
}

// =============================== PROGRAMAS DEL JUEGO (uniformes SAFX_*) ===============================
struct GameProg { GLuint id; GLint l0, l1, l2; };
static GameProg gProgs[160]; static int gNProgs = 0;
static void RescanPrograms() {
    for (GLuint p = 1; p <= 3000 && gNProgs < 160; p++) {
        if (!glIsProgram(p)) continue;
        bool known = false;
        for (int i = 0; i < gNProgs; i++) if (gProgs[i].id == p) { known = true; break; }
        if (known) continue;
        GLint l0 = glGetUniformLocation(p, "SAFX_P0"), l1 = glGetUniformLocation(p, "SAFX_P1"), l2 = glGetUniformLocation(p, "SAFX_P2");
        if (l0 >= 0 || l1 >= 0 || l2 >= 0) gProgs[gNProgs++] = { p, l0, l1, l2 };
    }
    while (glGetError() != GL_NO_ERROR) {}
}

// =============================== ESTADO POR FRAME ===============================
struct Frame { float t, windX, windY, windAmp, sunX, sunY, sunZ, sunUp; } F;
static void UpdateFrameState() {
    F.t = (float)fmod(NowSec(), 6283.0);
    float ws = pWind ? fminf(fmaxf(*pWind, 0.f), 1.5f) : 0.3f;
    float wx = 1.f, wy = 0.3f;
    if (pWindDir) { wx = pWindDir[0]; wy = pWindDir[1]; }
    float wl = sqrtf(wx * wx + wy * wy);
    if (wl < 0.01f) { wx = 1.f; wy = 0.3f; wl = sqrtf(wx * wx + wy * wy); }
    F.windX = wx / wl; F.windY = wy / wl;
    F.windAmp = C.treeSway * (0.5f + ws);
    const float* v = pVecToSun + ((*pCurStored) & 15) * 3;
    F.sunX = v[0]; F.sunY = v[1]; F.sunZ = v[2];
    F.sunUp = clamp01(v[2] * 3.0f);
}
static void UploadGameUniforms() {
    for (int i = 0; i < gNProgs; i++) {
        GameProg& g = gProgs[i];
        if (!glIsProgram(g.id)) continue;
        glUseProgram(g.id);
        if (g.l0 >= 0) glUniform4f(g.l0, F.t, F.windX, F.windY, F.windAmp);
        if (g.l1 >= 0) glUniform4f(g.l1, F.sunX, F.sunY, F.sunZ, C.waterSpec * F.sunUp);
        if (g.l2 >= 0) glUniform4f(g.l2, 1.0f, 0.94f, 0.82f, C.waterFres);
    }
}

// =============================== EFECTOS EN PANTALLA ===============================
#include <sys/stat.h>
static float* pHeat = nullptr; static float* pWetRoads = nullptr; static int* pArea = nullptr;

// contadores para saber si el juego esta dibujando el mundo 3D o un menu
static volatile int gSceneCount = 0, gMenuCount = 0;
typedef uint64_t (*RSFn)(bool);
typedef uint64_t (*MenuFn)(void*);
static RSFn RS_orig = nullptr; static MenuFn Menu_orig = nullptr;
static uint64_t RS_hook(bool b) { gSceneCount = gSceneCount + 1; return RS_orig(b); }
static uint64_t Menu_hook(void* self) { gMenuCount = gMenuCount + 1; return Menu_orig(self); }

struct Cfg2 {
    float exposure = 1.0f, tonemap = 0.35f, gamma = 1.0f, wb = 0.02f;
    float vignette = 0.25f, grain = 0.03f, chroma = 0.0015f, sharpen = 0.25f;
    float fxaa = 1, heat = 1, wet = 0.5f;
    float stR = 0.00f, stG = 0.02f, stB = 0.05f, htR = 0.05f, htG = 0.03f, htB = 0.00f;
} D;
struct Cfg3 {
    float master = 1, rain3d = 1, drops = 3500, radius = 30, length = 1.5f, speed = 20, bright = 1.0f, width = 1.0f;
} R;

// ---- archivo de configuracion: <configs de AML>/SAFX64.ini ----
struct Ent { const char* sec; const char* key; float* v; };
static Ent kEnt[] = {
    {"General","Enable",&R.master},
    {"Post","Tonemap",&D.tonemap},{"Post","Exposure",&D.exposure},{"Post","Gamma",&D.gamma},{"Post","WhiteBalance",&D.wb},
    {"Post","Saturation",&C.sat},{"Post","Contrast",&C.contrast},{"Post","Sharpen",&D.sharpen},{"Post","FilmGrain",&D.grain},
    {"Post","Vignette",&D.vignette},{"Post","ChromaticAberration",&D.chroma},{"Post","FXAA",&D.fxaa},
    {"Grading","ShadowTintR",&D.stR},{"Grading","ShadowTintG",&D.stG},{"Grading","ShadowTintB",&D.stB},
    {"Grading","HighlightTintR",&D.htR},{"Grading","HighlightTintG",&D.htG},{"Grading","HighlightTintB",&D.htB},
    {"Bloom","Enable",&C.bloom},{"Bloom","Intensity",&C.bloomInt},{"Bloom","Threshold",&C.bloomThr},
    {"SunRays","Enable",&C.godRays},{"SunRays","Intensity",&C.rayInt},{"SunRays","Density",&C.rayDens},{"SunRays","Threshold",&C.rayThr},
    {"SunRays","TintR",&C.tintR},{"SunRays","TintG",&C.tintG},{"SunRays","TintB",&C.tintB},
    {"LensFlare","Enable",&C.flare},{"LensFlare","Intensity",&C.flareInt},
    {"Rain","Enable",&C.rain},{"Rain","Rain3D",&R.rain3d},{"Rain","Drops",&R.drops},{"Rain","Radius",&R.radius},
    {"Rain","Length",&R.length},{"Rain","Speed",&R.speed},{"Rain","Brightness",&R.bright},{"Rain","Width",&R.width},
    {"Rain","ScreenOverlay",&C.rainInt},
    {"Wet","Sheen",&D.wet},
    {"HeatHaze","Strength",&D.heat},
    {"Wind","Enable",&C.wind},{"Wind","TreeSway",&C.treeSway},{"Wind","GrassGain",&C.grassGain},
    {"Water","Enable",&C.water},{"Water","Fresnel",&C.waterFres},{"Water","Specular",&C.waterSpec},
};
static const char* ToCStr(const char* s) { return s; }
static const char* ToCStr(const std::string& s) { static std::string keep; keep = s; return keep.c_str(); }

static void LoadConfig3() {
    C.rayInt = 0.85f; C.tintR = 1.f; C.tintG = 1.f; C.tintB = 1.f;   // valores por defecto de v0.4
    const int NE = (int)(sizeof(kEnt) / sizeof(kEnt[0]));
    bool found[128] = {false};
    char dir[300];
    const char* cp = ToCStr(aml->GetConfigPath());
    snprintf(dir, sizeof dir, "%s", (cp && cp[0]) ? cp : "/storage/emulated/0/Android/data/com.rockstargames.gtasa/configs");
    size_t n = strlen(dir); while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    mkdir(dir, 0777);
    char path[340]; snprintf(path, sizeof path, "%s/SAFX64.ini", dir);

    FILE* f = fopen(path, "r");
    if (f) {
        char sec[48] = "", line[200];
        while (fgets(line, sizeof line, f)) {
            char* s = line; while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#' || *s == ';' || *s == '\n' || *s == '\r') continue;
            if (*s == '[') { char* e = strchr(s, ']'); if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", s + 1); } continue; }
            char* eq = strchr(s, '='); if (!eq) continue;
            *eq = 0; char* key = s; char* val = eq + 1;
            for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            for (int i = 0; i < NE; i++)
                if (!strcmp(kEnt[i].sec, sec) && !strcmp(kEnt[i].key, key)) { *kEnt[i].v = (float)atof(val); found[i] = true; }
        }
        fclose(f);
    }
    bool missing = false;
    for (int i = 0; i < NE; i++) if (!found[i]) missing = true;
    if (missing) {
        FILE* o = fopen(path, "w");
        if (o) {
            fprintf(o, "; SAFX64 - configuracion de graficos. 1 = activado, 0 = desactivado.\n; Se lee al abrir el juego.\n");
            const char* last = "";
            for (int i = 0; i < NE; i++) {
                if (strcmp(last, kEnt[i].sec)) { fprintf(o, "\n[%s]\n", kEnt[i].sec); last = kEnt[i].sec; }
                fprintf(o, "%s = %g\n", kEnt[i].key, *kEnt[i].v);
            }
            fclose(o); LOG("SAFX64.ini creado/actualizado en %s", path);
        } else LOG("no pude escribir %s", path);
    } else LOG("SAFX64.ini leido de %s", path);
}

// ---- shader final (post-proceso) ----
static const char* kFSFinal2 = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uScene; uniform sampler2D uRays; uniform sampler2D uBloom;
uniform vec2 uSun;
uniform vec4 uA;   // x=rayI y=bloomI z=flareI w=lluvia en pantalla (solo respaldo)
uniform vec4 uB;   // x=time y=aspect z=saturation w=contrast
uniform vec3 uTint; // color del sol segun la hora
uniform vec4 uC;   // x=exposure y=tonemap z=gamma w=temperatura
uniform vec4 uD;   // x=vineta y=grano z=aberracion w=sharpen
uniform vec4 uE;   // x=heat y=mojado z=fxaa
uniform vec4 uF;   // xy = tamano de texel
uniform vec3 uG;   // tinte sombras
uniform vec3 uH;   // tinte luces
varying vec2 vUV;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

float hash(vec2 p){ return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p){
  vec2 i = floor(p); vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float rainLayer(vec2 uv, float scale, float speed, float slant){
  vec2 p = vec2((uv.x + uv.y * slant) * scale * uB.y, uv.y * scale * 0.30 + uB.x * speed);
  vec2 id = floor(p); vec2 f = fract(p);
  float h = hash(id);
  float present = step(0.80, h);
  float cx = 0.25 + 0.5 * hash(id + 3.7);
  float line = smoothstep(0.06, 0.0, abs(f.x - cx));
  return present * line * pow(f.y, 4.0);
}
vec3 aces(vec3 x){ return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

vec3 fxaa(vec2 uv){
  vec2 px = uF.xy;
  vec3 rgbNW = texture2D(uScene, uv + vec2(-1.0, -1.0) * px).rgb;
  vec3 rgbNE = texture2D(uScene, uv + vec2( 1.0, -1.0) * px).rgb;
  vec3 rgbSW = texture2D(uScene, uv + vec2(-1.0,  1.0) * px).rgb;
  vec3 rgbSE = texture2D(uScene, uv + vec2( 1.0,  1.0) * px).rgb;
  vec3 rgbM  = texture2D(uScene, uv).rgb;
  float lNW = dot(rgbNW, LUMA), lNE = dot(rgbNE, LUMA), lSW = dot(rgbSW, LUMA), lSE = dot(rgbSE, LUMA), lM = dot(rgbM, LUMA);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  vec2 dir;
  dir.x = -((lNW + lNE) - (lSW + lSE));
  dir.y =  ((lNW + lSW) - (lNE + lSE));
  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = min(vec2(8.0), max(vec2(-8.0), dir * rcpDirMin)) * px;
  vec3 rgbA = 0.5 * (texture2D(uScene, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture2D(uScene, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 rgbB = rgbA * 0.5 + 0.25 * (texture2D(uScene, uv + dir * -0.5).rgb + texture2D(uScene, uv + dir * 0.5).rgb);
  float lB = dot(rgbB, LUMA);
  return (lB < lMin || lB > lMax) ? rgbA : rgbB;
}

void main(){
  vec2 uv = vUV;
  if (uE.x > 0.001) {
    float band = smoothstep(0.15, 0.55, uv.y) * smoothstep(0.95, 0.60, uv.y);
    vec2 n = vec2(sin(uv.y * 70.0 + uB.x * 2.3), cos(uv.x * 55.0 + uB.x * 1.9));
    uv += n * 0.0020 * uE.x * band;
  }
  vec3 scene = (uE.z > 0.5) ? fxaa(uv) : texture2D(uScene, uv).rgb;
  if (uD.z > 0.00001) {
    vec2 cd = (uv - 0.5) * uD.z;
    scene.r = texture2D(uScene, uv + cd).r;
    scene.b = texture2D(uScene, uv - cd).b;
  }
  if (uD.w > 0.001) {
    vec3 nb = (texture2D(uScene, uv + vec2(uF.x, 0.0)).rgb + texture2D(uScene, uv - vec2(uF.x, 0.0)).rgb
             + texture2D(uScene, uv + vec2(0.0, uF.y)).rgb + texture2D(uScene, uv - vec2(0.0, uF.y)).rgb) * 0.25;
    scene = clamp(scene + (scene - nb) * uD.w, 0.0, 1.0);
  }

  vec2 asp = vec2(uB.y, 1.0);
  vec2 pc = (vUV - 0.5) * asp;
  vec2 sc = (uSun - 0.5) * asp;
  float dsun = length(pc - sc);
  // rayos: solo la forma de la luz; el color viene de la hora del dia (amarillo/naranja)
  vec3 rr = texture2D(uRays, vUV).rgb;
  float rm = max(rr.r, max(rr.g, rr.b));
  vec3 rays = vec3(pow(rm, 1.4)) * exp(-dsun * 2.0) * uA.x * uTint;
  rays *= mix(0.45, 1.0, smoothstep(0.0, 0.35, length(sc)));
  vec3 bloom = texture2D(uBloom, vUV).rgb * uA.y;

  float fl = exp(-dsun * 7.0) * 0.50;
  fl += smoothstep(0.075, 0.020, length(pc + sc * 0.45)) * 0.16;
  fl += smoothstep(0.110, 0.040, length(pc + sc * 0.85)) * 0.10;
  fl += smoothstep(0.050, 0.015, length(pc + sc * 1.45)) * 0.14;
  fl *= smoothstep(0.0, 0.25, length(sc)) * 0.6 + 0.4;
  vec3 flare = fl * uA.z * uTint;

  vec3 glow = min(rays + bloom + flare, vec3(0.75));
  vec3 col = scene + (1.0 - scene) * glow;

  if (uA.w > 0.002) {
    float lum = dot(col, LUMA);
    col = mix(col, vec3(lum) * vec3(0.93, 0.97, 1.04), 0.20 * uA.w);
    col *= 1.0 - 0.10 * uA.w;
    float r = rainLayer(vUV, 60.0, 5.0, 0.18) * 0.55 + rainLayer(vUV, 110.0, 7.5, 0.20) * 0.40 + rainLayer(vUV, 190.0, 10.0, 0.22) * 0.30;
    col += r * uA.w * vec3(0.72, 0.80, 0.90) * 0.55;
  }
  if (uE.y > 0.002) {
    float ground = smoothstep(0.50, 0.08, vUV.y);
    float nz = vnoise(vUV * vec2(7.0 * uB.y, 11.0) + vec2(0.0, uB.x * 0.03));
    float m = ground * smoothstep(0.52, 0.78, nz) * uE.y;
    vec3 skyc = texture2D(uScene, vec2(0.5, 0.90)).rgb;
    col = mix(col, col * 0.75 + skyc * 0.30, m);
  }

  col *= uC.x;
  col = mix(col, aces(col), uC.y);
  col *= vec3(1.0 + uC.w, 1.0, 1.0 - uC.w);
  float lm = dot(col, LUMA);
  col += uG * (1.0 - smoothstep(0.0, 0.55, lm)) + uH * smoothstep(0.45, 1.0, lm);
  col = pow(max(col, vec3(0.0)), vec3(1.0 / uC.z));
  float l2 = dot(col, LUMA);
  col = mix(vec3(l2), col, uB.z);
  col = (col - 0.5) * uB.w + 0.5;
  float vd = length((vUV - 0.5) * vec2(uB.y * 0.8, 1.0));
  col *= 1.0 - uD.x * smoothstep(0.35, 1.0, vd);
  col += (hash(vUV * 1000.0 + fract(uB.x) * 91.7) - 0.5) * uD.y;
  gl_FragColor = vec4(col, 1.0);
}
)GLSL";

struct FinalLocs { GLint scene, rays, bloom, sun, A, B, tint, C, D, E, F, G, H; };
static Prog pFinal2; static FinalLocs fl;

// ---- lluvia 3D: gotas en el mundo que caen en la direccion del viento (con prueba de profundidad) ----
static const char* kVSRain = R"GLSL(
attribute vec4 aSeed;
uniform mat4 uVP; uniform vec3 uCam; uniform vec4 uP; uniform vec3 uVel; uniform vec3 uQ;
varying float vA;
void main(){
  vec3 box = vec3(uP.z, uP.z, uP.w);
  vec3 p = aSeed.xyz * box + uVel * uP.x;
  p = p - box * floor(p / box);
  p -= box * 0.5;
  vec3 head = uCam + p;
  vec3 tail = head - normalize(uVel) * uQ.x;
  vec4 h = uVP * vec4(head, 1.0);
  vec4 t = uVP * vec4(tail, 1.0);
  float along = step(1.5, aSeed.w);
  float side = mod(aSeed.w, 2.0) * 2.0 - 1.0;
  vec4 c = mix(h, t, along);
  vec2 d = t.xy / t.w - h.xy / h.w;
  float dl = length(d);
  d = (dl > 0.00001) ? d / dl : vec2(0.0, 1.0);
  vec2 n = vec2(-d.y, d.x) * vec2(1.0 / uQ.z, 1.0);
  c.xy += n * side * uQ.y * c.w;
  gl_Position = c;
  float dist = length(p.xy) / (uP.z * 0.5);
  vA = (1.0 - smoothstep(0.6, 1.0, dist)) * (1.0 - 0.55 * along);
}
)GLSL";
static const char* kFSRain = R"GLSL(
precision mediump float;
varying float vA; uniform vec3 uCol;
void main(){ gl_FragColor = vec4(uCol * vA, vA); }
)GLSL";

static GLuint gRainProg = 0, gRainVbo = 0, gRainIbo = 0; static int gRainN = 0; static bool gRainFailed = false;
static GLint rVP = -1, rCam = -1, rP = -1, rVel = -1, rQ = -1, rCol = -1;
struct MatProg { GLuint id; GLint lp, lv; };
static MatProg gMats[24]; static int gNMats = 0;

static void RescanMats() {
    for (GLuint p = 1; p <= 3000 && gNMats < 24; p++) {
        if (!glIsProgram(p)) continue;
        bool known = false;
        for (int i = 0; i < gNMats; i++) if (gMats[i].id == p) { known = true; break; }
        if (known) continue;
        GLint lp = glGetUniformLocation(p, "ProjMatrix"), lv = glGetUniformLocation(p, "ViewMatrix");
        if (lp >= 0 && lv >= 0) gMats[gNMats++] = { p, lp, lv };
    }
    while (glGetError() != GL_NO_ERROR) {}
}
static void MatMul(const float* a, const float* b, float* o) {   // o = a * b (columnas)
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
        o[c * 4 + r] = s;
    }
}
// Lee las matrices que el juego uso en sus shaders y se queda con las que cuadran con la posicion real de la camara.
static bool GetViewProj(float* VP, const float* camPos) {
    float P[16], V[16];
    for (int i = gNMats - 1; i >= 0; i--) {
        if (!glIsProgram(gMats[i].id)) continue;
        glGetUniformfv(gMats[i].id, gMats[i].lp, P);
        glGetUniformfv(gMats[i].id, gMats[i].lv, V);
        if (!(fabsf(fabsf(P[11]) - 1.f) < 0.01f && fabsf(P[15]) < 0.01f && fabsf(P[0]) > 0.05f && fabsf(P[5]) > 0.05f)) continue;
        float x = V[0] * camPos[0] + V[4] * camPos[1] + V[8]  * camPos[2] + V[12];
        float y = V[1] * camPos[0] + V[5] * camPos[1] + V[9]  * camPos[2] + V[13];
        float z = V[2] * camPos[0] + V[6] * camPos[1] + V[10] * camPos[2] + V[14];
        if (fabsf(x) + fabsf(y) + fabsf(z) > 1.0f) continue;   // matriz vieja: la camara no queda en el origen
        MatMul(P, V, VP);
        return true;
    }
    return false;
}
static bool InitRain() {
    GLuint v = Compile(GL_VERTEX_SHADER, kVSRain), f = Compile(GL_FRAGMENT_SHADER, kFSRain);
    if (!v || !f) return false;
    gRainProg = glCreateProgram();
    glAttachShader(gRainProg, v); glAttachShader(gRainProg, f);
    glBindAttribLocation(gRainProg, 0, "aSeed");
    glLinkProgram(gRainProg);
    GLint ok = 0; glGetProgramiv(gRainProg, GL_LINK_STATUS, &ok);
    if (!ok) { LOG("lluvia: link fallo"); return false; }
    rVP = glGetUniformLocation(gRainProg, "uVP"); rCam = glGetUniformLocation(gRainProg, "uCam");
    rP = glGetUniformLocation(gRainProg, "uP"); rVel = glGetUniformLocation(gRainProg, "uVel");
    rQ = glGetUniformLocation(gRainProg, "uQ"); rCol = glGetUniformLocation(gRainProg, "uCol");
    int n = (int)R.drops; if (n < 100) n = 100; if (n > 8000) n = 8000;
    gRainN = n;
    float* vt = (float*)malloc(sizeof(float) * 16 * n);
    unsigned short* ix = (unsigned short*)malloc(sizeof(unsigned short) * 6 * n);
    if (!vt || !ix) { free(vt); free(ix); return false; }
    srand(12345);
    for (int i = 0; i < n; i++) {
        float rx = rand() / (float)RAND_MAX, ry = rand() / (float)RAND_MAX, rz = rand() / (float)RAND_MAX;
        for (int k = 0; k < 4; k++) { float* q = vt + (i * 4 + k) * 4; q[0] = rx; q[1] = ry; q[2] = rz; q[3] = (float)k; }
        unsigned short b = (unsigned short)(i * 4);
        unsigned short* t = ix + i * 6;
        t[0] = b; t[1] = (unsigned short)(b + 1); t[2] = (unsigned short)(b + 2);
        t[3] = (unsigned short)(b + 1); t[4] = (unsigned short)(b + 3); t[5] = (unsigned short)(b + 2);
    }
    glGenBuffers(1, &gRainVbo); glBindBuffer(GL_ARRAY_BUFFER, gRainVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16 * n, vt, GL_STATIC_DRAW);
    glGenBuffers(1, &gRainIbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gRainIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned short) * 6 * n, ix, GL_STATIC_DRAW);
    free(vt); free(ix);
    LOG("lluvia 3D lista: %d gotas", n);
    return true;
}
// Devuelve true si dibujo la lluvia 3D.
static bool DrawRain3D(const float* cam, float amount, float aspect, float windX, float windY, float windS, float t) {
    if (gRainFailed) return false;
    float VP[16];
    if (!GetViewProj(VP, cam + 12)) return false;
    if (!gRainProg) { if (!InitRain()) { gRainFailed = true; LOG("lluvia 3D desactivada (no inicio)"); return false; } }
    int count = (int)(gRainN * clamp01(amount));
    if (count < 8) return true;

    GLint df = 0, eb = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &df); glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    float drift = 1.0f + windS * 8.0f;
    glUseProgram(gRainProg);
    glUniformMatrix4fv(rVP, 1, GL_FALSE, VP);
    glUniform3f(rCam, cam[12], cam[13], cam[14]);
    glUniform4f(rP, t, 0.f, R.radius * 2.f, 26.f);
    glUniform3f(rVel, windX * drift, windY * drift, -R.speed);
    glUniform3f(rQ, R.length, 0.0012f * R.width, aspect);
    glUniform3f(rCol, 0.72f * R.bright, 0.80f * R.bright, 0.92f * R.bright);
    glBindBuffer(GL_ARRAY_BUFFER, gRainVbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gRainIbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (const void*)0);
    glDrawElements(GL_TRIANGLES, count * 6, GL_UNSIGNED_SHORT, (const void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)eb);
    glDepthFunc((GLenum)df);
    return true;
}

static void SunColor(float z, float* c) {   // naranja con el sol bajo, amarillo calido con el sol alto
    float t = smooth(0.0f, 0.55f, z);
    c[0] = 1.0f; c[1] = 0.45f + 0.43f * t; c[2] = 0.18f + 0.42f * t;
}

static void RunFX() {
    static int frame = 0; frame++;
    static int lastMenu = 0, lastScene = 0; static bool sceneSeen = false;
    int menuNow = gMenuCount, sceneNow = gSceneCount;
    bool menu = menuNow != lastMenu; lastMenu = menuNow;
    bool scene = sceneNow != lastScene; lastScene = sceneNow;
    if (sceneNow > 0) sceneSeen = true;
    if (menu || (sceneSeen && !scene)) return;      // menus: sin efectos (evita la capa blanca)

    int w, h; ScreenSize(w, h);
    if (w < 64 || h < 64) return;
    UpdateFrameState();

    float u = 0.5f, v = 0.5f, vis = 0.f;
    float cam[16]; bool camOK = ReadCamera(cam);
    float gu = *pSunX / (float)w, gv = 1.0f - (*pSunY / (float)h);
    bool front = false;
    if (camOK) {
        float facing = cam[8] * F.sunX + cam[9] * F.sunY + cam[10] * F.sunZ;
        if (CalcScreen) {
            float world[3] = { cam[12] + F.sunX * 100.f, cam[13] + F.sunY * 100.f, cam[14] + F.sunZ * 100.f };
            float out[3] = {0, 0, 0}, sw = 0, sh = 0;
            front = CalcScreen(world, out, &sw, &sh, false, false);
            if (front) { u = out[0] / (float)w; v = 1.0f - out[1] / (float)h; vis = 1.f; }
        } else if (facing > 0.1f) { u = gu; v = gv; vis = smooth(0.1f, 0.3f, facing); }
    } else { u = gu; v = gv; vis = 1.f; }
    if (!(fabsf(u) < 4.f && fabsf(v) < 4.f)) vis = 0.f;

    float ex = fmaxf(fmaxf(-u, u - 1.f), 0.f), ey = fmaxf(fmaxf(-v, v - 1.f), 0.f);
    float edge = 1.f - smooth(0.f, 0.45f, fmaxf(ex, ey));
    float blockedK = *pSunBlocked ? 1.f : 0.f;
    float rayK   = C.godRays > 0.5f ? C.rayInt * F.sunUp * vis * edge * (1.f - 0.65f * blockedK) : 0.f;
    float flareK = C.flare   > 0.5f ? C.flareInt * F.sunUp * vis * edge * (1.f - 0.80f * blockedK) : 0.f;
    float bloomK = C.bloom   > 0.5f ? C.bloomInt : 0.f;

    float envK = 1.f;
    if (pTunnel) envK *= 1.f - clamp01(*pTunnel);
    if (pUnder)  envK *= 1.f - clamp01(*pUnder);
    bool outdoors = !(pArea && *pArea != 0);
    if (!outdoors) envK = 0.f;
    float rainRaw = pRain ? clamp01(*pRain) : 0.f;
    float rainK = C.rain > 0.5f ? rainRaw * C.rainInt * envK : 0.f;
    float wetRoads = pWetRoads ? clamp01(*pWetRoads) : 0.f;
    float wetK = D.wet * clamp01(fmaxf(rainRaw, wetRoads)) * envK;
    float heatK = (pHeat ? clamp01(*pHeat) : 0.f) * D.heat * envK;

    if ((frame % 300) == 1)
        LOG("camOK=%d front=%d sunUV=(%.2f,%.2f) sunUp=%.2f rain=%.2f wind=%.2f progs=%d mats=%d agua=%d viento=%d fallos=%d",
            camOK, front, u, v, F.sunUp, rainRaw, pWind ? *pWind : -1.f, gNProgs, gNMats, g_nWater, g_nWind, g_nPatchFail);

    bool anyFX = rayK > 0.005f || flareK > 0.005f || bloomK > 0.005f || rainK > 0.005f || wetK > 0.005f || heatK > 0.005f
        || D.fxaa > 0.5f || D.vignette > 0.001f || D.grain > 0.001f || D.chroma > 0.00001f || D.sharpen > 0.001f
        || D.tonemap > 0.001f || fabsf(D.exposure - 1.f) > 0.01f || fabsf(D.gamma - 1.f) > 0.01f || fabsf(D.wb) > 0.001f
        || fabsf(C.sat - 1.f) > 0.01f || fabsf(C.contrast - 1.f) > 0.01f;

    if (!gReady || w != gW || h != gH) {
        GLState s0; Save(s0);
        gReady = InitGL(w, h);
        Restore(s0);
        if (!gReady) { gFailed = true; return; }
    }
    if (!pFinal2.id) {
        pFinal2.id = Link(kVS, kFSFinal2);
        if (!pFinal2.id) { LOG("shader final no compila, efectos de pantalla desactivados"); gFailed = true; return; }
        GLuint p = pFinal2.id;
        fl.scene = glGetUniformLocation(p, "uScene"); fl.rays = glGetUniformLocation(p, "uRays"); fl.bloom = glGetUniformLocation(p, "uBloom");
        fl.sun = glGetUniformLocation(p, "uSun"); fl.A = glGetUniformLocation(p, "uA"); fl.B = glGetUniformLocation(p, "uB");
        fl.tint = glGetUniformLocation(p, "uTint"); fl.C = glGetUniformLocation(p, "uC"); fl.D = glGetUniformLocation(p, "uD");
        fl.E = glGetUniformLocation(p, "uE"); fl.F = glGetUniformLocation(p, "uF"); fl.G = glGetUniformLocation(p, "uG"); fl.H = glGetUniformLocation(p, "uH");
    }
    if (frame < 600 ? (frame % 30) == 0 : (frame % 300) == 0) {
        GLState sr; Save(sr); RescanPrograms(); RescanMats(); Restore(sr);
    }

    GLState st; Save(st);
    UploadGameUniforms();


    // lluvia 3D (se dibuja antes del post-proceso para que reciba bloom y color)
    bool rain3dDone = false;
    if (R.rain3d > 0.5f && C.rain > 0.5f && camOK && envK * rainRaw > 0.01f)
        rain3dDone = DrawRain3D(cam, rainRaw * envK, (float)w / (float)h, F.windX, F.windY,
                                pWind ? fminf(fmaxf(*pWind, 0.f), 1.5f) : 0.3f, F.t);
    float rainShown = rain3dDone ? 0.f : rainK;

    if (anyFX) {
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
        glDepthMask(GL_FALSE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tScene);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

        if (rayK > 0.005f || bloomK > 0.005f) {
            Target(tBright, hW, hH);
            glUseProgram(pBright.id); glUniform1i(pBright.a, 0);
            glUniform1f(pBright.b, fminf(C.rayThr, C.bloomThr));
            glBindTexture(GL_TEXTURE_2D, tScene); DrawTri();
        }
        if (rayK > 0.005f) {
            Target(tRays, hW, hH);
            glUseProgram(pRays.id); glUniform1i(pRays.a, 0);
            glUniform2f(pRays.b, u, v); glUniform1f(pRays.c, C.rayDens);
            glBindTexture(GL_TEXTURE_2D, tBright); DrawTri();
        }
        if (bloomK > 0.005f) {
            glUseProgram(pBlur.id); glUniform1i(pBlur.a, 0);
            Target(tBlurA, qW, qH); glUniform2f(pBlur.b, 1.f / hW, 0.f);
            glBindTexture(GL_TEXTURE_2D, tBright); DrawTri();
            Target(tBlurB, qW, qH); glUniform2f(pBlur.b, 0.f, 1.f / qH);
            glBindTexture(GL_TEXTURE_2D, tBlurA); DrawTri();
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, w, h);
        glUseProgram(pFinal2.id);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tScene);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tRays);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tBlurB);
        glUniform1i(fl.scene, 0); glUniform1i(fl.rays, 1); glUniform1i(fl.bloom, 2);
        glUniform2f(fl.sun, u, v);
        glUniform4f(fl.A, rayK, bloomK, flareK, rainShown);
        glUniform4f(fl.B, F.t, (float)w / (float)h, C.sat, C.contrast);
        float sc[3]; SunColor(F.sunZ, sc);
        glUniform3f(fl.tint, sc[0] * C.tintR, sc[1] * C.tintG, sc[2] * C.tintB);
        glUniform4f(fl.C, D.exposure, D.tonemap, fmaxf(D.gamma, 0.2f), D.wb);
        glUniform4f(fl.D, D.vignette, D.grain, D.chroma, D.sharpen);
        glUniform4f(fl.E, heatK, wetK, D.fxaa > 0.5f ? 1.f : 0.f, 0.f);
        glUniform4f(fl.F, 1.f / (float)w, 1.f / (float)h, 0.f, 0.f);
        glUniform3f(fl.G, D.stR, D.stG, D.stB);
        glUniform3f(fl.H, D.htR, D.htG, D.htB);
        DrawTri();
    }
    Restore(st);
    while (glGetError() != GL_NO_ERROR) {}
}

static void Swap_hook(char*& cmd) {
    if (!gFailed) RunFX();
    Swap_orig(cmd);
}

// =============================== PARCHES DE SHADERS DEL JUEGO ===============================
static bool TestCompile(const char* vs, const char* fs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER); if (!v) return false;
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint p = glCreateProgram();
    GLint ok = 0, ok2 = 0, ok3 = 0;
    glShaderSource(v, 1, &vs, nullptr); glCompileShader(v); glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    glShaderSource(f, 1, &fs, nullptr); glCompileShader(f); glGetShaderiv(f, GL_COMPILE_STATUS, &ok2);
    if (ok && ok2) { glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p); glGetProgramiv(p, GL_LINK_STATUS, &ok3); }
    glDeleteProgram(p); glDeleteShader(v); glDeleteShader(f);
    while (glGetError() != GL_NO_ERROR) {}
    return ok && ok2 && ok3;
}
static bool InsertAfter(std::string& s, const char* anchor, const char* text) {
    size_t p = s.find(anchor); if (p == std::string::npos) return false;
    s.insert(p + strlen(anchor), text); return true;
}
static bool InsertBeforeMain(std::string& s, const char* text) {
    size_t p = s.find("void main"); if (p == std::string::npos) return false;
    s.insert(p, text); return true;
}

static const char* kWorldPosAnchor = "vec4 WorldPos = ObjMatrix * vec4(Position,1.0);";

static bool PatchWater(std::string& vs, std::string& fs) {
    if (fs.find("FogColor") == std::string::npos || vs.find("CameraPosition") == std::string::npos) return false;
    if (!InsertBeforeMain(vs, "varying mediump vec3 SAFX_View;\n")) return false;
    if (!InsertAfter(vs, kWorldPosAnchor, "\n\tSAFX_View = WorldPos.xyz - CameraPosition.xyz;\n")) return false;
    if (!InsertBeforeMain(fs, "varying mediump vec3 SAFX_View;\nuniform mediump vec4 SAFX_P1;\nuniform mediump vec4 SAFX_P2;\n")) return false;
    const char* code = R"GLSL(
{
  vec3 SAFX_V = normalize(SAFX_View);
  float SAFX_e = 0.006;
  vec2 SAFX_g = vec2(
    texture2D(EnvMap, Out_WaterDetail + vec2(SAFX_e, 0.0), -1.0).x - texture2D(EnvMap, Out_WaterDetail - vec2(SAFX_e, 0.0), -1.0).x,
    texture2D(EnvMap, Out_WaterDetail2 + vec2(0.0, SAFX_e), -1.0).x - texture2D(EnvMap, Out_WaterDetail2 - vec2(0.0, SAFX_e), -1.0).x);
  vec3 SAFX_N = normalize(vec3(SAFX_g * 3.0, 1.0));
  float SAFX_c = clamp(dot(-SAFX_V, SAFX_N), 0.0, 1.0);
  float SAFX_F = 0.02 + 0.98 * pow(1.0 - SAFX_c, 5.0);
  vec3 SAFX_R = reflect(SAFX_V, SAFX_N);
  vec3 SAFX_sky = mix(FogColor, FogColor * 1.25 + vec3(0.04, 0.07, 0.12), clamp(SAFX_R.z * 1.5, 0.0, 1.0));
  float SAFX_d = max(dot(SAFX_R, SAFX_P1.xyz), 0.0);
  float SAFX_sp = pow(SAFX_d, 140.0) * 1.6 + pow(SAFX_d, 22.0) * 0.12;
  fcolor.xyz = mix(fcolor.xyz, SAFX_sky, SAFX_F * SAFX_P2.w) + SAFX_P2.xyz * SAFX_sp * SAFX_P1.w;
}
)GLSL";
    return InsertAfter(fs, "waterDetail * 1.1, Out_Color.w);", code);
}

static bool PatchWind(std::string& vs) {
    if (!InsertBeforeMain(vs, "uniform highp vec4 SAFX_P0;\n")) return false;
    const char* code = R"GLSL(

  float SAFX_h = smoothstep(1.0, 12.0, Position.z);
  float SAFX_s = sin(WorldPos.x * 0.07 + SAFX_P0.x * 1.7) + sin(WorldPos.y * 0.05 + SAFX_P0.x * 1.1 + 1.3) * 0.7 + 1.2;
  WorldPos.xy += SAFX_P0.yz * (SAFX_s * 0.35) * SAFX_P0.w * SAFX_h;
)GLSL";
    return InsertAfter(vs, kWorldPosAnchor, code);
}

static uint64_t Build_hook(void* self, const char* vs, const char* fs) {
    if (vs && fs && (C.water > 0.5f || C.wind > 0.5f)) {
        std::string v(vs), f(fs);
        bool water = f.find("Out_WaterDetail") != std::string::npos && v.find("Out_WaterDetail") != std::string::npos;
        bool foliage = !water && v.find("BoneToLocal") == std::string::npos && v.find(kWorldPosAnchor) != std::string::npos
                       && f.find("discard") != std::string::npos && f.find("EnvMapCoefficient") == std::string::npos;
        bool changed = false;
        if (water && C.water > 0.5f) changed = PatchWater(v, f);
        else if (foliage && C.wind > 0.5f) changed = PatchWind(v);
        if (changed) {
            if (TestCompile(v.c_str(), f.c_str())) {
                if (water) g_nWater++; else g_nWind++;
                return Build_orig(self, strdup(v.c_str()), strdup(f.c_str()));
            }
            g_nPatchFail++;
        }
    }
    return Build_orig(self, vs, fs);
}

// Viento en pasto/plantas: amplifica la inclinacion que calcula el juego
static void Wind_hook(float v) {
    float gust = 0.04f * sinf((float)fmod(NowSec(), 6283.0) * 1.3f);
    Wind_orig(v * C.grassGain + gust);
}

// =============================== CARGA DEL MOD ===============================
ON_MOD_PRELOAD() { LOG("SAFX 0.4 preload"); }

ON_MOD_LOAD() {
    LoadConfig3();
    if (R.master < 0.5f) { LOG("desactivado en SAFX64.ini (General/Enable=0)"); return; }
    void* h = (void*)aml->GetLibHandle("libGTASA.so");
    if (!h) { LOG("no se encontro libGTASA.so"); return; }
    auto sym = [&](const char* n) { return (void*)aml->GetSym(h, n); };

    pCurStored  = (int*)  sym("_ZN10CTimeCycle20m_CurrentStoredValueE");
    pVecToSun   = (float*)sym("_ZN10CTimeCycle13m_VectorToSunE");
    pSunX       = (float*)sym("_ZN8CCoronas10SunScreenXE");
    pSunY       = (float*)sym("_ZN8CCoronas10SunScreenYE");
    pSunBlocked = (bool*) sym("_ZN8CCoronas18SunBlockedByCloudsE");
    pRain       = (float*)sym("_ZN8CWeather4RainE");
    pWind       = (float*)sym("_ZN8CWeather4WindE");
    pWindDir    = (float*)sym("_ZN8CWeather7WindDirE");
    pTunnel     = (float*)sym("_ZN8CWeather12InTunnelnessE");
    pUnder      = (float*)sym("_ZN8CWeather14UnderWaternessE");
    pHeat       = (float*)sym("_ZN8CWeather8HeatHazeE");
    pWetRoads   = (float*)sym("_ZN8CWeather8WetRoadsE");
    pArea       = (int*)  sym("_ZN5CGame8currAreaE");
    pTheCamera  =         sym("TheCamera");
    CalcScreen  = (CalcScreenFn)sym("_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb");

    void* swap   = sym("_Z24RQ_Command_rqSwapBuffersRPc");
    void* rscene = sym("_Z11RenderSceneb");
    void* rmenu  = sym("_ZN12CMenuManager12DrawFrontEndEv");
    void* build  = sym("_ZN9ES2Shader5BuildEPKcS1_");
    void* wind   = sym("_ZN14CGrassRenderer20SetGlobalWindBendingEf");

    if (!pCurStored || !pVecToSun || !pSunX || !pSunY || !pSunBlocked || !swap) {
        LOG("faltan simbolos base, plugin desactivado");
        return;
    }
    aml->Hook(swap, (void*)Swap_hook, (void**)&Swap_orig);
    if (rscene) aml->Hook(rscene, (void*)RS_hook, (void**)&RS_orig);
    if (rmenu)  aml->Hook(rmenu, (void*)Menu_hook, (void**)&Menu_orig);
    if (build) aml->Hook(build, (void*)Build_hook, (void**)&Build_orig); else LOG("sin ES2Shader::Build: agua/arboles desactivados");
    if (wind && C.wind > 0.5f) aml->Hook(wind, (void*)Wind_hook, (void**)&Wind_orig);
    LOG("SAFX activo (rain=%p wind=%p camera=%p calc=%p build=%p grass=%p)", pRain, pWind, pTheCamera, (void*)CalcScreen, build, wind);
}
