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
    float vignette = 0.25f, grain = 0.03f, chroma = 0.0005f, sharpen = 0.25f;
    float fxaa = 1, heat = 0, wet = 0.5f;
    float stR = 0.00f, stG = 0.02f, stB = 0.05f, htR = 0.05f, htG = 0.03f, htB = 0.00f;
} D;
struct Cfg3 {
    float master = 1, rain3d = 1, drops = 3500, radius = 30, length = 1.5f, speed = 20, bright = 1.0f, width = 1.0f;
    float samples = 48, cfgVer = 0;
} R;

// ---- archivo de configuracion: <configs de AML>/SAFX64.ini ----
struct Ent { const char* sec; const char* key; float* v; };
static Ent kEnt[] = {
    {"General","Enable",&R.master},{"General","ConfigVersion",&R.cfgVer},
    {"Post","Tonemap",&D.tonemap},{"Post","Exposure",&D.exposure},{"Post","Gamma",&D.gamma},{"Post","WhiteBalance",&D.wb},
    {"Post","Saturation",&C.sat},{"Post","Contrast",&C.contrast},{"Post","Sharpen",&D.sharpen},{"Post","FilmGrain",&D.grain},
    {"Post","Vignette",&D.vignette},{"Post","ChromaticAberration",&D.chroma},{"Post","FXAA",&D.fxaa},
    {"Grading","ShadowTintR",&D.stR},{"Grading","ShadowTintG",&D.stG},{"Grading","ShadowTintB",&D.stB},
    {"Grading","HighlightTintR",&D.htR},{"Grading","HighlightTintG",&D.htG},{"Grading","HighlightTintB",&D.htB},
    {"Bloom","Enable",&C.bloom},{"Bloom","Intensity",&C.bloomInt},{"Bloom","Threshold",&C.bloomThr},
    {"SunRays","Enable",&C.godRays},{"SunRays","Intensity",&C.rayInt},{"SunRays","Samples",&R.samples},{"SunRays","Density",&C.rayDens},{"SunRays","Threshold",&C.rayThr},
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
    C.rayInt = 1.6f; C.rayThr = 0.62f; C.tintR = 1.f; C.tintG = 1.f; C.tintB = 1.f;   // valores por defecto
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
    if (R.cfgVer < 2.f) {   // config de una version vieja: aplicar los valores nuevos y reescribir
        C.rayInt = 1.6f; C.rayThr = 0.62f; D.chroma = 0.0005f; D.heat = 0.f; R.samples = 48.f; R.cfgVer = 2.f; missing = true;
    }
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
    vec2 n = vec2(sin(uv.y * 22.0 + uB.x * 1.3), cos(uv.x * 18.0 + uB.x * 1.1));
    uv += n * 0.0006 * uE.x * band;
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
  rm = clamp((rm - 0.15) / 0.85, 0.0, 1.0);
  vec3 rays = vec3(pow(rm, 1.6)) * exp(-dsun * 1.15) * uA.x * uTint;
  rays *= mix(0.6, 1.0, smoothstep(0.0, 0.35, length(sc)));
  vec3 bloom = texture2D(uBloom, vUV).rgb * uA.y;

  float fl = exp(-dsun * 5.0) * 0.55 + exp(-dsun * 16.0) * 0.55;
  fl += smoothstep(0.075, 0.020, length(pc + sc * 0.45)) * 0.16;
  fl += smoothstep(0.110, 0.040, length(pc + sc * 0.85)) * 0.10;
  fl += smoothstep(0.050, 0.015, length(pc + sc * 1.45)) * 0.14;
  fl *= smoothstep(0.0, 0.25, length(sc)) * 0.6 + 0.4;
  vec3 flare = fl * uA.z * uTint;

  vec3 glow = min(rays + bloom + flare, vec3(0.88));
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

static const char* kFSRays2 = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTex; uniform vec2 uSun; uniform float uDens; uniform float uSamples; uniform float uDecay; varying vec2 vUV;
float jit(vec2 p){ return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main(){
  vec2 delta = (uSun - vUV) * uDens / uSamples;
  vec2 uv = vUV + delta * jit(vUV * 513.0);
  float w = 1.0; float ws = 0.0; vec3 acc = vec3(0.0);
  for (int i = 0; i < 64; i++) {
    if (float(i) >= uSamples) break;
    uv += delta;
    acc += texture2D(uTex, uv).rgb * w; ws += w; w *= uDecay;
  }
  gl_FragColor = vec4(acc / ws, 1.0);
}
)GLSL";
static Prog pRays2;

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

// =============================== CONFIG AMPLIADA (v0.5) ===============================
#include <stdarg.h>
struct Cfg4 {
    float giOn = 1, giStrength = 0.7f, hazeOn = 1, hazeDensity = 0.0009f, hazeFalloff = 0.03f;
    float puddles = 1, puddleRefl = 0.85f, ripples = 1, dryGround = 240.f;
    float carRain = 1, carStrength = 1.f, carStreaks = 1, dryCar = 120.f;
    float carDensity = 1.6f, splash = 1.f, whitecaps = 0.6f;
    float lightsOn = 1, lightStrength = 1.f, lightRange = 14.f;
    float glowOn = 1, glowStrength = 0.8f, glowSize = 2.0f;
    float cloudsOn = 1, cloudInt = 0.85f, cloudCover = 1.f, cloudHud = 1;
    float carRefl = 1.f;
} L4;
static int g_nWorld = 0, g_nVeh = 0;
static char g_logPath[340] = ""; static int g_logLines = 0;
static void FLog(const char* fmt, ...) {      // registro en <configs>/SAFX64.log (para diagnosticar sin logcat)
    if (!g_logPath[0] || g_logLines > 400) return;
    FILE* lf = fopen(g_logPath, "a"); if (!lf) return;
    va_list ap; va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap);
    fputc('\n', lf); fclose(lf); g_logLines++;
}
static Ent kEnt2[] = {
    {"Lighting","Enable",&L4.giOn},{"Lighting","Strength",&L4.giStrength},
    {"Haze","Enable",&L4.hazeOn},{"Haze","Density",&L4.hazeDensity},{"Haze","HeightFalloff",&L4.hazeFalloff},
    {"Puddles","Enable",&L4.puddles},{"Puddles","Reflection",&L4.puddleRefl},{"Puddles","Ripples",&L4.ripples},{"Puddles","DryTime",&L4.dryGround},{"Puddles","Splash",&L4.splash},
    {"VehicleRain","Enable",&L4.carRain},{"VehicleRain","Strength",&L4.carStrength},{"VehicleRain","Streaks",&L4.carStreaks},{"VehicleRain","DryTime",&L4.dryCar},{"VehicleRain","Density",&L4.carDensity},
    {"WaterFX","Whitecaps",&L4.whitecaps},
    {"DynLights","Enable",&L4.lightsOn},{"DynLights","Strength",&L4.lightStrength},{"DynLights","Range",&L4.lightRange},
    {"Glow","Enable",&L4.glowOn},{"Glow","Strength",&L4.glowStrength},{"Glow","Size",&L4.glowSize},
    {"Clouds","Enable",&L4.cloudsOn},{"Clouds","Intensity",&L4.cloudInt},{"Clouds","Coverage",&L4.cloudCover},{"Clouds","HudSafe",&L4.cloudHud},
    {"VehicleRefl","Strength",&L4.carRefl},
};
static void LoadConfig4() {
    const int NE = (int)(sizeof(kEnt) / sizeof(kEnt[0]));
    const int NE2 = (int)(sizeof(kEnt2) / sizeof(kEnt2[0]));
    char dir[300];
    const char* cp = ToCStr(aml->GetConfigPath());
    snprintf(dir, sizeof dir, "%s", (cp && cp[0]) ? cp : "/storage/emulated/0/Android/data/com.rockstargames.gtasa/configs");
    size_t n = strlen(dir); while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
    mkdir(dir, 0777);
    char path[340]; snprintf(path, sizeof path, "%s/SAFX64.ini", dir);
    snprintf(g_logPath, sizeof g_logPath, "%s/SAFX64.log", dir);
    { FILE* lf = fopen(g_logPath, "w"); if (lf) fclose(lf); }

    bool found2[64] = {false};
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
            for (int i = 0; i < NE2; i++)
                if (!strcmp(kEnt2[i].sec, sec) && !strcmp(kEnt2[i].key, key)) { *kEnt2[i].v = (float)atof(val); found2[i] = true; }
        }
        fclose(f);
    }
    bool missing = false;
    for (int i = 0; i < NE2; i++) if (!found2[i]) missing = true;
    if (R.cfgVer < 3.f) { D.wet = 0.f; R.cfgVer = 3.f; missing = true; }   // el brillo falso del suelo se reemplaza por charcos reales
    if (missing) {
        FILE* o = fopen(path, "w");
        if (o) {
            fprintf(o, "; SAFX64 - configuracion de graficos. 1 = activado, 0 = desactivado.\n; Se lee al abrir el juego.\n");
            const char* last = "";
            for (int i = 0; i < NE; i++) {
                if (strcmp(last, kEnt[i].sec)) { fprintf(o, "\n[%s]\n", kEnt[i].sec); last = kEnt[i].sec; }
                fprintf(o, "%s = %g\n", kEnt[i].key, *kEnt[i].v);
            }
            for (int i = 0; i < NE2; i++) {
                if (strcmp(last, kEnt2[i].sec)) { fprintf(o, "\n[%s]\n", kEnt2[i].sec); last = kEnt2[i].sec; }
                fprintf(o, "%s = %g\n", kEnt2[i].key, *kEnt2[i].v);
            }
            fclose(o); LOG("SAFX64.ini actualizado en %s", path);
        }
    }
    FLog("SAFX 0.5 cargado. config: %s", path);
}


// ---- luces dinamicas: se capturan los coronas que registra el juego (farolas, faros, semaforos) ----
struct LRec { float x, y, z, r, g, b, rad; uint32_t stamp; };
static LRec gLB[256];
static float* pCloudCov = nullptr;
static int gCoronaCalls = 0;
static int gLightN = 0; static float gLP[8][4], gLC[8][4];
static void StoreCorona(unsigned long long id, unsigned char r, unsigned char g, unsigned char b, unsigned char a, const float* pos, float radius) {
    gCoronaCalls++;
    if (!pos || a < 24 || radius < 0.01f) return;
    LRec& L = gLB[id % 256];
    float k = (a / 255.f);
    L.x = pos[0]; L.y = pos[1]; L.z = pos[2];
    L.r = (r / 255.f) * k; L.g = (g / 255.f) * k; L.b = (b / 255.f) * k; L.rad = radius;
    L.stamp = (uint32_t)(NowSec() * 1000.0);
}
typedef void (*CoronaFnA)(unsigned long long, void*, unsigned char, unsigned char, unsigned char, unsigned char, const float*, float, float, unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, float, bool, float, bool, float, bool, bool);
typedef void (*CoronaFnB)(unsigned long long, void*, unsigned char, unsigned char, unsigned char, unsigned char, const float*, float, float, void*, unsigned char, unsigned char, unsigned char, unsigned char, float, bool, float, bool, float, bool, bool);
static CoronaFnA CoronaA_orig = nullptr; static CoronaFnB CoronaB_orig = nullptr;
static void CoronaA_hook(unsigned long long id, void* ent, unsigned char r, unsigned char g, unsigned char b, unsigned char a, const float* pos, float radius, float farClip,
                         unsigned char t0, unsigned char t1, unsigned char t2, unsigned char t3, unsigned char t4, float angle, bool lng, float nearClip, bool fade, float fadeSpeed, bool below, bool delay) {
    StoreCorona(id, r, g, b, a, pos, radius);
    CoronaA_orig(id, ent, r, g, b, a, pos, radius, farClip, t0, t1, t2, t3, t4, angle, lng, nearClip, fade, fadeSpeed, below, delay);
}
static void CoronaB_hook(unsigned long long id, void* ent, unsigned char r, unsigned char g, unsigned char b, unsigned char a, const float* pos, float radius, float farClip, void* tex,
                         unsigned char t1, unsigned char t2, unsigned char t3, unsigned char t4, float angle, bool lng, float nearClip, bool fade, float fadeSpeed, bool below, bool delay) {
    StoreCorona(id, r, g, b, a, pos, radius);
    CoronaB_orig(id, ent, r, g, b, a, pos, radius, farClip, tex, t1, t2, t3, t4, angle, lng, nearClip, fade, fadeSpeed, below, delay);
}
static void SelectLights(const float* cp) {
    gLightN = 0;
    if (L4.lightsOn < 0.5f && L4.glowOn < 0.5f) return;
    uint32_t now = (uint32_t)(NowSec() * 1000.0);
    float bd[8]; int n = 0;
    for (int i = 0; i < 256; i++) {
        const LRec& L = gLB[i];
        if (L.stamp == 0 || (uint32_t)(now - L.stamp) > 200u) continue;
        float dx = L.x - cp[0], dy = L.y - cp[1], dz = L.z - cp[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 3600.f || (L.r + L.g + L.b) < 0.05f) continue;
        int pos = n < 8 ? n : 8;
        while (pos > 0 && bd[pos - 1] > d2) pos--;
        if (pos >= 8) continue;
        int last = n < 8 ? n : 7;
        for (int k = last; k > pos; k--) { bd[k] = bd[k - 1]; memcpy(gLP[k], gLP[k - 1], 16); memcpy(gLC[k], gLC[k - 1], 16); }
        bd[pos] = d2;
        float rng = L4.lightRange * fminf(fmaxf(0.6f + 0.5f * L.rad, 0.7f), 1.6f);
        gLP[pos][0] = L.x; gLP[pos][1] = L.y; gLP[pos][2] = L.z; gLP[pos][3] = rng;
        gLC[pos][0] = L.r; gLC[pos][1] = L.g; gLC[pos][2] = L.b; gLC[pos][3] = L.rad;
        if (n < 8) n++;
    }
    gLightN = n;
}

// ---- uniformes SAFX_* que los shaders parcheados del juego necesitan cada frame ----
struct GP2 { GLuint id; GLint u[13]; };   // 0..5 = P0..P5, 6 = Cam, 7 = VP, 8 = Refl, 9 = P6, 10 = P7, 11 = LP, 12 = LC
static const char* kUN[13] = { "SAFX_P0", "SAFX_P1", "SAFX_P2", "SAFX_P3", "SAFX_P4", "SAFX_P5", "SAFX_Cam", "SAFX_VP", "SAFX_Refl", "SAFX_P6", "SAFX_P7", "SAFX_LP", "SAFX_LC" };
static GP2 gP2[200]; static int gNP2 = 0;
static void RescanPrograms2() {
    for (GLuint p = 1; p <= 3000 && gNP2 < 200; p++) {
        if (!glIsProgram(p)) continue;
        bool known = false;
        for (int i = 0; i < gNP2; i++) if (gP2[i].id == p) { known = true; break; }
        if (known) continue;
        GP2 g; g.id = p; bool any = false;
        for (int k = 0; k < 13; k++) { g.u[k] = glGetUniformLocation(p, kUN[k]); if (g.u[k] >= 0) any = true; }
        if (any) gP2[gNP2++] = g;
    }
    while (glGetError() != GL_NO_ERROR) {}
}
static void UploadGameUniforms2(const float* VP, const float* camPos, float wetGround, float wetCarV, float rainNow) {
    float sc[3]; SunColor(F.sunZ, sc);
    float giK = L4.giOn > 0.5f ? L4.giStrength : 0.f;
    float hz = L4.hazeOn > 0.5f ? L4.hazeDensity : 0.f;
    for (int i = 0; i < gNP2; i++) {
        GP2& g = gP2[i];
        if (!glIsProgram(g.id)) continue;
        glUseProgram(g.id);
        if (g.u[0] >= 0) glUniform4f(g.u[0], F.t, F.windX, F.windY, F.windAmp);
        if (g.u[1] >= 0) glUniform4f(g.u[1], F.sunX, F.sunY, F.sunZ, C.waterSpec * F.sunUp);
        if (g.u[2] >= 0) glUniform4f(g.u[2], sc[0], sc[1], sc[2], C.waterFres);
        if (g.u[3] >= 0) glUniform4f(g.u[3], wetGround, wetCarV, rainNow, F.t);
        if (g.u[4] >= 0) glUniform4f(g.u[4], giK, hz, L4.hazeFalloff, F.sunUp);
        if (g.u[5] >= 0) glUniform4f(g.u[5], L4.puddleRefl, L4.ripples > 0.5f ? 1.f : 0.f, L4.carRain > 0.5f ? L4.carStrength : 0.f, L4.carStreaks > 0.5f ? 1.f : 0.f);
        if (g.u[6] >= 0 && camPos) glUniform3f(g.u[6], camPos[0], camPos[1], camPos[2]);
        if (g.u[7] >= 0 && VP) glUniformMatrix4fv(g.u[7], 1, GL_FALSE, VP);
        if (g.u[8] >= 0) glUniform1i(g.u[8], 6);
        if (g.u[9] >= 0) glUniform4f(g.u[9], L4.carDensity, L4.splash, L4.whitecaps, L4.lightsOn > 0.5f ? (float)gLightN : 0.f);
        if (g.u[10] >= 0) glUniform4f(g.u[10], L4.carRefl, L4.lightStrength, 0.f, 0.f);
        if (g.u[11] >= 0) glUniform4fv(g.u[11], 8, &gLP[0][0]);
        if (g.u[12] >= 0) glUniform4fv(g.u[12], 8, &gLC[0][0]);
    }
}

// ---- utilidades de matrices ----
static bool Inv4(const float* m, float* inv) {
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.f / det;
    for (int i = 0; i < 16; i++) inv[i] *= det;
    return true;
}

// ---- mascara para no pintar sobre el HUD (radar, reloj, botones) ----
static const char* kHudMaskGLSL = R"GLSL(
float hudm(vec2 u){
  float m = 1.0;
  m -= (1.0 - smoothstep(0.14, 0.17, u.x)) * smoothstep(0.64, 0.68, u.y);
  m -= smoothstep(0.75, 0.78, u.x) * smoothstep(0.64, 0.68, u.y);
  m -= smoothstep(0.90, 0.92, u.x);
  m -= ((1.0 - smoothstep(0.20, 0.23, u.x)) + smoothstep(0.80, 0.83, u.x)) * (1.0 - smoothstep(0.28, 0.32, u.y));
  return mix(1.0, clamp(m, 0.0, 1.0), uHud);
}
)GLSL";

// ---- resplandor de luces (halo volumetrico, con prueba de profundidad para que los muros lo tapen) ----
static const char* kVSGlow = R"GLSL(
attribute vec2 aPos; uniform mat4 uVP; uniform vec4 uP; uniform vec3 uR; uniform vec3 uU; varying vec2 vC;
void main(){ vC = aPos; vec3 w = uP.xyz + (uR * aPos.x + uU * aPos.y) * uP.w; gl_Position = uVP * vec4(w, 1.0); }
)GLSL";
static const char* kFSGlowA = R"GLSL(
precision mediump float;
varying vec2 vC; uniform vec3 uCol; uniform vec4 uS; uniform vec2 uRes; uniform float uHud;
)GLSL";
static const char* kFSGlowB = R"GLSL(
void main(){
  float q = dot(vC, vC);
  float core = exp(-q * 9.0);
  float halo = exp(-q * 2.6) * (1.0 - smoothstep(0.7, 1.0, q));
  float a = core * 0.8 + halo * uS.y;
  vec2 suv = gl_FragCoord.xy / uRes;
  gl_FragColor = vec4(uCol * a * uS.x * hudm(suv), 1.0);
}
)GLSL";
static GLuint gGlowProg = 0; static bool gGlowFail = false;
static GLint gl_VP = -1, gl_P = -1, gl_R = -1, gl_U = -1, gl_Col = -1, gl_S = -1, gl_Res = -1, gl_Hud = -1;
static bool DrawGlow(const float* VP, const float* cam, int w, int h, float rain) {
    if (gGlowFail || gLightN <= 0) return true;
    if (!gGlowProg) {
        std::string fs = std::string(kFSGlowA) + kHudMaskGLSL + kFSGlowB;
        gGlowProg = Link(kVSGlow, fs.c_str());
        if (!gGlowProg) { gGlowFail = true; FLog("resplandor: el shader no compila"); return false; }
        gl_VP = glGetUniformLocation(gGlowProg, "uVP"); gl_P = glGetUniformLocation(gGlowProg, "uP");
        gl_R = glGetUniformLocation(gGlowProg, "uR"); gl_U = glGetUniformLocation(gGlowProg, "uU");
        gl_Col = glGetUniformLocation(gGlowProg, "uCol"); gl_S = glGetUniformLocation(gGlowProg, "uS");
        gl_Res = glGetUniformLocation(gGlowProg, "uRes"); gl_Hud = glGetUniformLocation(gGlowProg, "uHud");
        FLog("resplandor listo");
    }
    GLint df = 0; glGetIntegerv(GL_DEPTH_FUNC, &df);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, w, h);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(gGlowProg);
    glUniformMatrix4fv(gl_VP, 1, GL_FALSE, VP);
    glUniform3f(gl_R, cam[0], cam[1], cam[2]); glUniform3f(gl_U, cam[4], cam[5], cam[6]);
    glUniform2f(gl_Res, (float)w, (float)h); glUniform1f(gl_Hud, L4.cloudHud);
    static const float quad[8] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    float dayK = 0.35f + 0.65f * (1.f - F.sunUp);
    for (int i = 0; i < gLightN; i++) {
        float dx = gLP[i][0] - cam[12], dy = gLP[i][1] - cam[13], dz = gLP[i][2] - cam[14];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        float nf = fminf(fmaxf((dist - 1.5f) / 5.5f, 0.f), 1.f);
        float size = L4.glowSize * fminf(fmaxf(0.5f + 0.5f * gLC[i][3], 0.6f), 1.8f) * (1.f + 0.8f * rain);
        float k = L4.glowStrength * dayK;
        glUniform4f(gl_P, gLP[i][0], gLP[i][1], gLP[i][2], size);
        glUniform3f(gl_Col, gLC[i][0] * k, gLC[i][1] * k, gLC[i][2] * k);
        glUniform4f(gl_S, nf, 0.45f + 0.7f * rain, 0.f, 0.f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDepthFunc((GLenum)df);
    return true;
}

// ---- nubes volumetricas: solo donde el buffer de profundidad esta en el fondo (cielo) ----
static const char* kVSCloud = R"GLSL(
precision highp float;
attribute vec2 aPos; uniform mat4 uInvVP; uniform vec3 uCam; varying vec3 vRay;
void main(){
  vec4 w = uInvVP * vec4(aPos, 1.0, 1.0);
  vRay = w.xyz / w.w - uCam;
  gl_Position = vec4(aPos, 1.0, 1.0);
}
)GLSL";
static const char* kFSCloudA = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec3 vRay;
uniform vec3 uSun; uniform vec3 uSunCol; uniform vec3 uCam;
uniform vec4 uA; uniform vec4 uB; uniform vec2 uRes; uniform float uHud;
)GLSL";
static const char* kFSCloudB = R"GLSL(
float hs(vec2 p){ vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float nz(vec2 p){
  vec2 i = floor(p); vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hs(i), hs(i + vec2(1.0, 0.0)), f.x), mix(hs(i + vec2(0.0, 1.0)), hs(i + vec2(1.0, 1.0)), f.x), f.y);
}
float fbm(vec2 p){
  float a = 0.5; float s = 0.0;
  for (int i = 0; i < 4; i++) { s += a * nz(p); p = mat2(0.8, -0.6, 0.6, 0.8) * p * 2.03; a *= 0.5; }
  return s;
}
void main(){
  vec3 rd = normalize(vRay);
  if (rd.z < 0.03) discard;
  vec2 suv = gl_FragCoord.xy / uRes;
  float hm = hudm(suv);
  if (hm < 0.01) discard;
  float cov = uA.y;
  float thr = mix(0.68, 0.36, cov);
  vec2 wind = uB.xy * uA.x * 0.00012;
  float sunUp = uA.w;
  vec2 sdir = normalize(uSun.xy + vec2(0.0001, 0.0));
  float t1 = 1800.0 / rd.z;
  vec2 p1 = (uCam.xy + rd.xy * t1) * 0.00032 + wind;
  float f1 = fbm(p1);
  float s1 = fbm(p1 + sdir * 0.03);
  float d1 = smoothstep(thr, thr + 0.22, f1);
  float l1 = clamp(0.55 + (f1 - s1) * 3.5, 0.0, 1.0);
  float t2 = 4200.0 / rd.z;
  vec2 p2 = (uCam.xy + rd.xy * t2) * 0.00018 + wind * 0.6 + vec2(7.3, 2.1);
  float d2 = smoothstep(thr + 0.04, thr + 0.30, fbm(p2));
  vec3 lit = uSunCol * (0.9 + 0.5 * sunUp) + vec3(0.18);
  vec3 shade = vec3(0.40, 0.46, 0.58) * (0.5 + 0.5 * sunUp);
  vec3 c1 = mix(shade, lit, l1);
  float edge = d1 * (1.0 - d1) * 4.0;
  c1 += uSunCol * edge * pow(max(dot(rd, uSun), 0.0), 6.0) * 0.5 * sunUp;
  vec3 c2 = mix(shade, lit, 0.7);
  float dark = mix(1.0, 0.50, uB.z) * (0.12 + 0.88 * clamp(sunUp * 1.4, 0.0, 1.0));
  c1 *= dark; c2 *= dark;
  float a1 = d1 * uA.z;
  float a2 = d2 * uA.z * 0.55;
  float al = 1.0 - (1.0 - a1) * (1.0 - a2);
  vec3 col = (c1 * a1 + c2 * a2 * (1.0 - a1)) / max(al, 0.001);
  al *= smoothstep(0.03, 0.28, rd.z) * hm;
  gl_FragColor = vec4(col, al);
}
)GLSL";
static GLuint gCloudProg = 0; static bool gCloudFail = false;
static GLint cl_Inv = -1, cl_Cam = -1, cl_Sun = -1, cl_SunCol = -1, cl_A = -1, cl_B = -1, cl_Res = -1, cl_Hud = -1;
static bool DrawClouds(const float* VP, const float* camPos, int w, int h, float rain, float windS) {
    if (gCloudFail) return false;
    if (!gCloudProg) {
        std::string fs = std::string(kFSCloudA) + kHudMaskGLSL + kFSCloudB;
        gCloudProg = Link(kVSCloud, fs.c_str());
        if (!gCloudProg) { gCloudFail = true; FLog("nubes: el shader no compila"); return false; }
        cl_Inv = glGetUniformLocation(gCloudProg, "uInvVP"); cl_Cam = glGetUniformLocation(gCloudProg, "uCam");
        cl_Sun = glGetUniformLocation(gCloudProg, "uSun"); cl_SunCol = glGetUniformLocation(gCloudProg, "uSunCol");
        cl_A = glGetUniformLocation(gCloudProg, "uA"); cl_B = glGetUniformLocation(gCloudProg, "uB");
        cl_Res = glGetUniformLocation(gCloudProg, "uRes"); cl_Hud = glGetUniformLocation(gCloudProg, "uHud");
        FLog("nubes listas");
    }
    float inv[16];
    if (!Inv4(VP, inv)) return false;
    GLint df = 0; glGetIntegerv(GL_DEPTH_FUNC, &df);
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, w, h);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(gCloudProg);
    float sc[3]; SunColor(F.sunZ, sc);
    float cc = pCloudCov ? clamp01(*pCloudCov) : 0.5f;
    float cov = fminf(fmaxf(L4.cloudCover * (0.30f + 0.55f * cc) + 0.40f * rain, 0.10f), 0.95f);
    glUniformMatrix4fv(cl_Inv, 1, GL_FALSE, inv);
    glUniform3f(cl_Cam, camPos[0], camPos[1], camPos[2]);
    glUniform3f(cl_Sun, F.sunX, F.sunY, F.sunZ);
    glUniform3f(cl_SunCol, sc[0], sc[1], sc[2]);
    glUniform4f(cl_A, F.t, cov, L4.cloudInt, F.sunUp);
    glUniform4f(cl_B, (F.windX + 0.3f) * (1.f + windS * 2.f), (F.windY + 0.1f) * (1.f + windS * 2.f), rain, 0.f);
    glUniform2f(cl_Res, (float)w, (float)h); glUniform1f(cl_Hud, L4.cloudHud);
    DrawTri();
    glDepthFunc((GLenum)df);
    return true;
}

// ---- lluvia 3D v2: las gotas muy cercanas a la camara se desvanecen (evita la "textura" en primera persona) ----
static const char* kVSRain2 = R"GLSL(
attribute vec4 aSeed;
uniform mat4 uVP; uniform vec3 uCam; uniform vec4 uP; uniform vec3 uVel; uniform vec3 uQ;
varying float vA;
void main(){
  vec3 box = vec3(uP.z, uP.z, uP.w);
  vec3 p = aSeed.xyz * box + uVel * uP.x;
  p = p - box * floor(p / box);
  p -= box * 0.5;
  float dist3 = length(p);
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
  float nearF = smoothstep(2.0, 7.0, dist3);
  vA = (1.0 - smoothstep(0.6, 1.0, dist)) * (1.0 - 0.55 * along) * nearF;
}
)GLSL";
static GLuint gRainProg2 = 0, gRainVbo2 = 0, gRainIbo2 = 0; static int gRainN2 = 0; static bool gRainFailed2 = false;
static GLint r2VP = -1, r2Cam = -1, r2P = -1, r2Vel = -1, r2Q = -1, r2Col = -1;
static bool InitRain2() {
    GLuint v = Compile(GL_VERTEX_SHADER, kVSRain2), f = Compile(GL_FRAGMENT_SHADER, kFSRain);
    if (!v || !f) return false;
    gRainProg2 = glCreateProgram();
    glAttachShader(gRainProg2, v); glAttachShader(gRainProg2, f);
    glBindAttribLocation(gRainProg2, 0, "aSeed");
    glLinkProgram(gRainProg2);
    GLint ok = 0; glGetProgramiv(gRainProg2, GL_LINK_STATUS, &ok);
    if (!ok) { FLog("lluvia v2: link fallo"); return false; }
    r2VP = glGetUniformLocation(gRainProg2, "uVP"); r2Cam = glGetUniformLocation(gRainProg2, "uCam");
    r2P = glGetUniformLocation(gRainProg2, "uP"); r2Vel = glGetUniformLocation(gRainProg2, "uVel");
    r2Q = glGetUniformLocation(gRainProg2, "uQ"); r2Col = glGetUniformLocation(gRainProg2, "uCol");
    int n = (int)R.drops; if (n < 100) n = 100; if (n > 8000) n = 8000;
    gRainN2 = n;
    float* vt = (float*)malloc(sizeof(float) * 16 * n);
    unsigned short* ix = (unsigned short*)malloc(sizeof(unsigned short) * 6 * n);
    if (!vt || !ix) { free(vt); free(ix); return false; }
    srand(777);
    for (int i = 0; i < n; i++) {
        float rx = rand() / (float)RAND_MAX, ry = rand() / (float)RAND_MAX, rz = rand() / (float)RAND_MAX;
        for (int k = 0; k < 4; k++) { float* q = vt + (i * 4 + k) * 4; q[0] = rx; q[1] = ry; q[2] = rz; q[3] = (float)k; }
        unsigned short b = (unsigned short)(i * 4);
        unsigned short* t = ix + i * 6;
        t[0] = b; t[1] = (unsigned short)(b + 1); t[2] = (unsigned short)(b + 2);
        t[3] = (unsigned short)(b + 1); t[4] = (unsigned short)(b + 3); t[5] = (unsigned short)(b + 2);
    }
    glGenBuffers(1, &gRainVbo2); glBindBuffer(GL_ARRAY_BUFFER, gRainVbo2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16 * n, vt, GL_STATIC_DRAW);
    glGenBuffers(1, &gRainIbo2); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gRainIbo2);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned short) * 6 * n, ix, GL_STATIC_DRAW);
    free(vt); free(ix);
    FLog("lluvia 3D v2 lista: %d gotas", n);
    return true;
}
static bool DrawRain3D2(const float* cam, float amount, float aspect, float windX, float windY, float windS, float t) {
    if (gRainFailed2) return false;
    float VP[16];
    if (!GetViewProj(VP, cam + 12)) return false;
    if (!gRainProg2) { if (!InitRain2()) { gRainFailed2 = true; return false; } }
    int count = (int)(gRainN2 * clamp01(amount));
    if (count < 8) return true;
    GLint df = 0, eb = 0;
    glGetIntegerv(GL_DEPTH_FUNC, &df); glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
    glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    float drift = 1.0f + windS * 8.0f;
    glUseProgram(gRainProg2);
    glUniformMatrix4fv(r2VP, 1, GL_FALSE, VP);
    glUniform3f(r2Cam, cam[12], cam[13], cam[14]);
    glUniform4f(r2P, t, 0.f, R.radius * 2.f, 26.f);
    glUniform3f(r2Vel, windX * drift, windY * drift, -R.speed);
    glUniform3f(r2Q, R.length, 0.0010f * R.width, aspect);
    glUniform3f(r2Col, 0.72f * R.bright, 0.80f * R.bright, 0.92f * R.bright);
    glBindBuffer(GL_ARRAY_BUFFER, gRainVbo2); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gRainIbo2);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (const void*)0);
    glDrawElements(GL_TRIANGLES, count * 6, GL_UNSIGNED_SHORT, (const void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)eb);
    glDepthFunc((GLenum)df);
    return true;
}

static void RunFX() {
    static int frame = 0; frame++;
    static int lastMenu = 0, lastScene = 0; static bool sceneSeen = false;
    int menuNow = gMenuCount, sceneNow = gSceneCount;
    bool menu = menuNow != lastMenu; lastMenu = menuNow;
    bool scene = sceneNow != lastScene; lastScene = sceneNow;
    if (sceneNow > 0) sceneSeen = true;
    static bool symsInit = false; static void* pPlayers = nullptr; static uint32_t* pTimeMs = nullptr;
    if (!symsInit) {
        symsInit = true;
        void* hh = (void*)aml->GetLibHandle("libGTASA.so");
        if (hh) { pPlayers = (void*)aml->GetSym(hh, "_ZN6CWorld7PlayersE"); pTimeMs = (uint32_t*)aml->GetSym(hh, "_ZN6CTimer22m_snTimeInMillisecondsE"); }
        LOG("compuerta de menu: players=%p timer=%p", pPlayers, (void*)pTimeMs);
    }
    bool worldLoaded = true;   // falso en el menu de inicio (aun no hay partida)
    if (pTimeMs && *pTimeMs == 0) worldLoaded = false;
    if (worldLoaded && pPlayers) { uintptr_t ped = 0; if (SafeRead(pPlayers, &ped, sizeof ped) && ped == 0) worldLoaded = false; }
    if (menu || !worldLoaded || (sceneSeen && !scene)) return;      // menus: sin efectos

    int w, h; ScreenSize(w, h);
    if (w < 64 || h < 64) return;
    UpdateFrameState();
    // suavizar el vector del sol y el aviso de nubes: el juego los cambia a saltos y eso causaba parpadeos
    static bool sInit = false; static float ssx = 0, ssy = 0, ssz = 1, sBlk = 0; static double sT = 0;
    double nowS = NowSec(); float dts = sInit ? fminf((float)(nowS - sT), 0.25f) : 1.f; sT = nowS;
    float ks = 1.f - expf(-dts * 3.0f);
    if (!sInit) { ssx = F.sunX; ssy = F.sunY; ssz = F.sunZ; sBlk = *pSunBlocked ? 1.f : 0.f; sInit = true; }
    else { ssx += (F.sunX - ssx) * ks; ssy += (F.sunY - ssy) * ks; ssz += (F.sunZ - ssz) * ks;
           sBlk += ((*pSunBlocked ? 1.f : 0.f) - sBlk) * (1.f - expf(-dts * 4.0f)); }
    { float ln = sqrtf(ssx * ssx + ssy * ssy + ssz * ssz); if (ln > 0.001f) { ssx /= ln; ssy /= ln; ssz /= ln; } }
    F.sunX = ssx; F.sunY = ssy; F.sunZ = ssz; F.sunUp = clamp01(ssz * 3.0f);

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
    float blockedK = sBlk;
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

    // humedad acumulada: sube mientras llueve y se seca con el tiempo
    static float wetG = 0.f, wetCar = 0.f; static double lastT = 0.0;
    double nowT = NowSec(); float dt = lastT > 0.0 ? fminf((float)(nowT - lastT), 0.25f) : 0.f; lastT = nowT;
    float rainNow = rainRaw * envK;
    if (rainNow > 0.05f) { wetG += rainNow * dt / 20.f; wetCar += rainNow * dt / 6.f; }
    else { wetG -= dt / fmaxf(L4.dryGround, 5.f); wetCar -= dt / fmaxf(L4.dryCar, 5.f); }
    wetG = clamp01(wetG); wetCar = clamp01(wetCar);
    float wetGroundUse = fmaxf(wetG, wetRoads * 0.85f * envK);

    if ((frame % 900) == 1)
        FLog("frame=%d camOK=%d sunUp=%.2f rain=%.2f wetG=%.2f wetCar=%.2f progs=%d mats=%d agua=%d viento=%d mundo=%d vehiculo=%d fallos=%d",
             frame, camOK, F.sunUp, rainRaw, wetG, wetCar, gNP2, gNMats, g_nWater, g_nWind, g_nWorld, g_nVeh, g_nPatchFail);
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
    if (!pRays2.id) {
        pRays2.id = Link(kVS, kFSRays2);
        if (!pRays2.id) { LOG("shader de rayos no compila"); gFailed = true; return; }
        pRays2.a = glGetUniformLocation(pRays2.id, "uTex"); pRays2.b = glGetUniformLocation(pRays2.id, "uSun");
        pRays2.c = glGetUniformLocation(pRays2.id, "uDens"); pRays2.d = glGetUniformLocation(pRays2.id, "uSamples");
        pRays2.e = glGetUniformLocation(pRays2.id, "uDecay");
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
        GLState sr; Save(sr); RescanPrograms2(); RescanMats(); Restore(sr);
    }

    GLState st; Save(st);
    float VPm[16]; bool vpOK = camOK && GetViewProj(VPm, cam + 12);
    if (camOK) SelectLights(cam + 12); else gLightN = 0;
    UploadGameUniforms2(vpOK ? VPm : nullptr, camOK ? cam + 12 : nullptr, wetGroundUse, wetCar, rainNow);
    if ((frame % 900) == 1) FLog("coronas=%d luces=%d nubes=%d resplandor=%d reflejos=%.2f", gCoronaCalls, gLightN, (int)(gCloudProg != 0), (int)(gGlowProg != 0), L4.carRefl);
    if (vpOK && L4.cloudsOn > 0.5f && outdoors && envK > 0.05f)
        DrawClouds(VPm, cam + 12, w, h, rainNow, pWind ? fminf(fmaxf(*pWind, 0.f), 1.5f) : 0.3f);

    // lluvia 3D (se dibuja antes del post-proceso para que reciba bloom y color)
    bool rain3dDone = false;
    if (R.rain3d > 0.5f && C.rain > 0.5f && camOK && envK * rainRaw > 0.01f)
        rain3dDone = DrawRain3D2(cam, rainRaw * envK, (float)w / (float)h, F.windX, F.windY,
                                pWind ? fminf(fmaxf(*pWind, 0.f), 1.5f) : 0.3f, F.t);
    float rainShown = rain3dDone ? 0.f : rainK;
    if (vpOK && L4.glowOn > 0.5f && gLightN > 0 && outdoors) DrawGlow(VPm, cam, w, h, rainNow);

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
            float ns = fminf(fmaxf(R.samples, 8.f), 64.f);
            glUseProgram(pRays2.id); glUniform1i(pRays2.a, 0);
            glUniform2f(pRays2.b, u, v); glUniform1f(pRays2.c, C.rayDens);
            glUniform1f(pRays2.d, ns); glUniform1f(pRays2.e, powf(0.96f, 28.f / ns));
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
    // la escena de este frame queda en la unidad 6 para que los charcos la reflejen en el siguiente
    glActiveTexture(GL_TEXTURE0 + 6); glBindTexture(GL_TEXTURE_2D, tScene); glActiveTexture(st.activeTex);
    while (glGetError() != GL_NO_ERROR) {}
}

static void Swap_hook(char*& cmd) {
    if (!gFailed) RunFX();
    Swap_orig(cmd);
}

// =============================== PARCHES DE SHADERS DEL JUEGO ===============================
static bool TestCompile(const char* vs, const char* fs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    if (!v) { static bool once = false; if (!once) { once = true; FLog("TestCompile: no hay contexto GL en este hilo (ctx=%p)", (void*)eglGetCurrentContext()); } return false; }
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint p = glCreateProgram();
    GLint ok = 0, ok2 = 0, ok3 = 0;
    glShaderSource(v, 1, &vs, nullptr); glCompileShader(v); glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    glShaderSource(f, 1, &fs, nullptr); glCompileShader(f); glGetShaderiv(f, GL_COMPILE_STATUS, &ok2);
    if (ok && ok2) { glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p); glGetProgramiv(p, GL_LINK_STATUS, &ok3); }
    if (!(ok && ok2 && ok3) && g_nPatchFail < 6) {   // guardar el motivo en SAFX64.log
        char b[700]; b[0] = 0;
        if (!ok) glGetShaderInfoLog(v, sizeof b, nullptr, b);
        else if (!ok2) glGetShaderInfoLog(f, sizeof b, nullptr, b);
        else glGetProgramInfoLog(p, sizeof b, nullptr, b);
        FLog("parche rechazado (%s): %s", !ok ? "vertex" : (!ok2 ? "fragment" : "link"), b);
    }
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

static const char* kWaterHdrB = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define SAFX_HP highp
#else
#define SAFX_HP mediump
#endif
float SAFX_wn(SAFX_HP vec2 p){
  SAFX_HP vec2 i = floor(p); SAFX_HP vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);
  float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
  float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
  float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
)GLSL";

static const char* kWaterCodeA = R"GLSL(
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
  float SAFX_sp = pow(SAFX_d, 260.0) * 2.4 + pow(SAFX_d, 36.0) * 0.16;
  float SAFX_cr = texture2D(EnvMap, Out_WaterDetail * 0.5, -1.0).x;
  float SAFX_foam = smoothstep(0.66, 0.84, SAFX_cr) * SAFX_P6.z * (1.0 - SAFX_F);
  vec3 SAFX_deep = vec3(0.01, 0.11, 0.19);
  fcolor.xyz = mix(fcolor.xyz, SAFX_deep + fcolor.xyz * 0.40, 0.30);
  fcolor.xyz = mix(fcolor.xyz, SAFX_sky, SAFX_F * SAFX_P2.w);
  fcolor.xyz = mix(fcolor.xyz, vec3(0.90, 0.95, 1.0) * (0.45 + 0.55 * SAFX_P4.w), SAFX_foam * 0.65);
  fcolor.xyz += SAFX_P2.xyz * SAFX_sp * SAFX_P1.w;
}
)GLSL";

static const char* kWaterCodeB = R"GLSL(
{
  vec3 SAFX_V = normalize(SAFX_View);
  SAFX_HP vec2 SAFX_p1 = Out_WaterDetail * 3.0;
  SAFX_HP vec2 SAFX_p2 = Out_WaterDetail2 * 3.0 + 5.7;
  float SAFX_e = 0.08;
  vec2 SAFX_g = vec2(SAFX_wn(SAFX_p1 + vec2(SAFX_e, 0.0)) - SAFX_wn(SAFX_p1 - vec2(SAFX_e, 0.0)),
                     SAFX_wn(SAFX_p1 + vec2(0.0, SAFX_e)) - SAFX_wn(SAFX_p1 - vec2(0.0, SAFX_e)));
  SAFX_g += vec2(SAFX_wn(SAFX_p2 + vec2(SAFX_e, 0.0)) - SAFX_wn(SAFX_p2 - vec2(SAFX_e, 0.0)),
                 SAFX_wn(SAFX_p2 + vec2(0.0, SAFX_e)) - SAFX_wn(SAFX_p2 - vec2(0.0, SAFX_e)));
  vec3 SAFX_N = normalize(vec3(SAFX_g * 1.8, 1.0));
  float SAFX_c = clamp(dot(-SAFX_V, SAFX_N), 0.0, 1.0);
  float SAFX_F = 0.02 + 0.98 * pow(1.0 - SAFX_c, 5.0);
  vec3 SAFX_R = reflect(SAFX_V, SAFX_N);
  vec3 SAFX_sky = mix(FogColor, FogColor * 1.25 + vec3(0.04, 0.07, 0.12), clamp(SAFX_R.z * 1.5, 0.0, 1.0));
  float SAFX_d = max(dot(SAFX_R, SAFX_P1.xyz), 0.0);
  float SAFX_gl = 0.6 + 0.8 * SAFX_wn(SAFX_p1 * 6.0 + vec2(SAFX_P3.w * 0.7, 0.0));
  float SAFX_sp = pow(SAFX_d, 380.0) * 3.0 * SAFX_gl + pow(SAFX_d, 40.0) * 0.18;
  float SAFX_nf = 1.0 - Out_FogAmt;
  float SAFX_cr = SAFX_wn(SAFX_p1 * 0.7 + vec2(SAFX_P3.w * 0.04, 0.0)) * 0.6 + SAFX_wn(SAFX_p2 * 1.3 + vec2(0.0, SAFX_P3.w * 0.03)) * 0.4;
  float SAFX_foam = smoothstep(0.66, 0.84, SAFX_cr) * SAFX_P6.z * (1.0 - SAFX_F) * SAFX_nf;
  vec3 SAFX_deep = vec3(0.01, 0.11, 0.19);
  fcolor.xyz = mix(fcolor.xyz, SAFX_deep + fcolor.xyz * 0.40, 0.30 * SAFX_nf);
  fcolor.xyz = mix(fcolor.xyz, SAFX_sky, SAFX_F * SAFX_P2.w * SAFX_nf);
  fcolor.xyz = mix(fcolor.xyz, vec3(0.90, 0.95, 1.0) * (0.45 + 0.55 * SAFX_P4.w), SAFX_foam * 0.65);
  fcolor.xyz += SAFX_P2.xyz * SAFX_sp * SAFX_P1.w * SAFX_nf;
}
)GLSL";

static bool PatchWater(std::string& vs, std::string& fs) {
    if (fs.find("FogColor") == std::string::npos || vs.find("CameraPosition") == std::string::npos) return false;
    if (!InsertBeforeMain(vs, "varying mediump vec3 SAFX_View;\n")) return false;
    if (!InsertAfter(vs, kWorldPosAnchor, "\n\tSAFX_View = WorldPos.xyz - CameraPosition.xyz;\n")) return false;
    bool detail = fs.find("waterDetail * 1.1, Out_Color.w);") != std::string::npos;
    std::string hdr = "varying mediump vec3 SAFX_View;\nuniform mediump vec4 SAFX_P1;\nuniform mediump vec4 SAFX_P2;\nuniform mediump vec4 SAFX_P3;\nuniform mediump vec4 SAFX_P4;\nuniform mediump vec4 SAFX_P6;\n";
    if (!detail) hdr += kWaterHdrB;
    if (!InsertBeforeMain(fs, hdr.c_str())) return false;
    if (detail) return InsertAfter(fs, "waterDetail * 1.1, Out_Color.w);", kWaterCodeA);
    size_t gp = fs.find("gl_FragColor = fcolor;");
    if (gp == std::string::npos) return false;
    fs.insert(gp, kWaterCodeB);
    return true;
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


// ---- parche del mundo: iluminacion por pixel, bruma con dispersion solar, charcos con reflejo y gotas en carros ----
static const char* kWHeader = R"GLSL(
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define SAFX_HP highp
#else
#define SAFX_HP mediump
#endif
varying SAFX_HP vec3 SAFX_Wp; varying mediump vec3 SAFX_Wn; varying mediump vec3 SAFX_Lp;
uniform SAFX_HP vec3 SAFX_Cam; uniform SAFX_HP mat4 SAFX_VP; uniform sampler2D SAFX_Refl;
uniform mediump vec4 SAFX_P1; uniform mediump vec4 SAFX_P2; uniform mediump vec4 SAFX_P3; uniform mediump vec4 SAFX_P4; uniform mediump vec4 SAFX_P5; uniform mediump vec4 SAFX_P6; uniform mediump vec4 SAFX_P7;
uniform SAFX_HP vec4 SAFX_LP[8]; uniform mediump vec4 SAFX_LC[8];
float SAFX_h(SAFX_HP vec2 p){
  SAFX_HP vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float SAFX_n(SAFX_HP vec2 p){
  SAFX_HP vec2 i = floor(p); SAFX_HP vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(SAFX_h(i), SAFX_h(i + vec2(1.0, 0.0)), f.x), mix(SAFX_h(i + vec2(0.0, 1.0)), SAFX_h(i + vec2(1.0, 1.0)), f.x), f.y);
}
float SAFX_pf(SAFX_HP vec2 p){
  vec2 w = vec2(SAFX_n(p * 0.35 + 3.1), SAFX_n(p * 0.35 + 8.7)) - 0.5;
  p += w * 2.2;
  float a = SAFX_n(p * 0.22) * 0.55;
  p = mat2(0.80, -0.60, 0.60, 0.80) * p;
  a += SAFX_n(p * 0.55) * 0.30;
  p = mat2(0.80, -0.60, 0.60, 0.80) * p;
  a += SAFX_n(p * 1.30) * 0.15;
  return a;
}
vec3 SAFX_bead(SAFX_HP vec2 uv, float cover, float seed){
  SAFX_HP vec2 bi = floor(uv);
  vec2 bf = fract(uv) - 0.5;
  float h1 = SAFX_h(bi + seed);
  float h2 = SAFX_h(bi + seed + 17.3);
  float h3 = SAFX_h(bi + seed + 41.7);
  float on = step(h1, cover);
  vec2 off = (vec2(h2, h3) - 0.5) * 0.5;
  float r = 0.10 + 0.17 * h2;
  float d = length(bf - off);
  float body = smoothstep(r, r * 0.80, d) * on;
  float rim = (smoothstep(r, r * 0.85, d) - smoothstep(r * 0.85, r * 0.55, d)) * on;
  vec2 hl = (bf - off) / r;
  float sp = smoothstep(0.38, 0.0, length(hl - vec2(-0.32, 0.36))) * body;
  return vec3(body, rim, sp);
}
)GLSL";

static const char* kWCommon = R"GLSL(
{
  vec3 SAFX_N = SAFX_Wn;
  float SAFX_nn = dot(SAFX_N, SAFX_N);
  vec3 SAFX_Vw = SAFX_Wp - SAFX_Cam;
  float SAFX_D = max(length(SAFX_Vw), 0.01);
  vec3 SAFX_V = SAFX_Vw / SAFX_D;
  float SAFX_nOK = 0.0;
#ifdef GL_OES_standard_derivatives
  SAFX_HP vec3 SAFX_dx = dFdx(SAFX_Wp) * 16.0;
  SAFX_HP vec3 SAFX_dy = dFdy(SAFX_Wp) * 16.0;
  SAFX_HP vec3 SAFX_gn = cross(SAFX_dx, SAFX_dy);
  SAFX_HP float SAFX_gl = dot(SAFX_gn, SAFX_gn);
#endif
  if (SAFX_nn > 0.01) {
    SAFX_N = SAFX_N * inversesqrt(SAFX_nn);
    SAFX_nOK = 1.0;
  } else {
    SAFX_N = vec3(0.0);
#ifdef GL_OES_standard_derivatives
    if (SAFX_gl > 1.0e-12) {
      SAFX_gn = SAFX_gn * inversesqrt(SAFX_gl);
      SAFX_N = (dot(SAFX_gn, SAFX_V) > 0.0) ? -SAFX_gn : SAFX_gn;
      SAFX_nOK = 1.0;
    }
#endif
  }
  float SAFX_sunUp = SAFX_P4.w;
)GLSL";

static const char* kWGI = R"GLSL(
  {
    float SAFX_gk = SAFX_P4.x * smoothstep(0.0, 0.35, SAFX_sunUp) * SAFX_nOK;
    float SAFX_lw = smoothstep(-0.15, 0.55, dot(SAFX_N, SAFX_P1.xyz));
    float SAFX_lu = smoothstep(-0.15, 0.55, SAFX_P1.z);
    float SAFX_lit = mix(SAFX_lw, SAFX_lu, smoothstep(0.70, 0.95, SAFX_N.z));
    vec3 SAFX_hs = FogColor * 1.1 + vec3(0.02, 0.04, 0.08);
    vec3 SAFX_hg = vec3(0.30, 0.26, 0.22) * (0.4 + 0.6 * SAFX_sunUp);
    vec3 SAFX_am = mix(SAFX_hg, SAFX_hs, SAFX_N.z * 0.5 + 0.5);
    vec3 SAFX_gi = SAFX_am * 0.55 + SAFX_P2.xyz * (SAFX_sunUp * SAFX_lit * 0.75);
    fcolor.xyz *= mix(vec3(1.0), SAFX_gi, SAFX_gk);
  }
)GLSL";

static const char* kWHaze = R"GLSL(
  {
    float SAFX_hh = exp(-max(0.5 * (SAFX_Wp.z + SAFX_Cam.z), 0.0) * SAFX_P4.z);
    float SAFX_fa = clamp((1.0 - exp(-SAFX_D * SAFX_P4.y)) * SAFX_hh, 0.0, 0.8);
    float SAFX_sd = pow(max(dot(SAFX_V, SAFX_P1.xyz), 0.0), 5.0);
    vec3 SAFX_hc = mix(FogColor, SAFX_P2.xyz, SAFX_sd * SAFX_sunUp * 0.85) * 1.4;
    fcolor.xyz = mix(fcolor.xyz, SAFX_hc, SAFX_fa);
  }
)GLSL";

static const char* kWGround = R"GLSL(
  if (SAFX_nOK > 0.5 && SAFX_P3.x > 0.01 && SAFX_N.z > 0.5) {
    float SAFX_W = SAFX_P3.x;
    float SAFX_up = smoothstep(0.50, 0.85, SAFX_N.z);
    float SAFX_pn = SAFX_pf(SAFX_Wp.xy);
    float SAFX_t0 = 0.64 - 0.16 * SAFX_W;
    float SAFX_edge = smoothstep(SAFX_t0 - 0.07, SAFX_t0 + 0.05, SAFX_pn) * SAFX_up;
    float SAFX_pd = smoothstep(SAFX_t0 + 0.02, SAFX_t0 + 0.20, SAFX_pn) * SAFX_up;
    fcolor.xyz *= 1.0 - (0.20 * SAFX_W * SAFX_up + 0.18 * SAFX_edge + 0.12 * SAFX_pd);
    vec2 SAFX_rp = vec2(0.0);
    float SAFX_rg = 0.0;
    if (SAFX_P5.y > 0.5 && SAFX_P3.z > 0.02 && SAFX_W > 0.15) {
      SAFX_HP vec2 SAFX_q = SAFX_Wp.xy * 1.3;
      SAFX_HP vec2 SAFX_id = floor(SAFX_q);
      vec2 SAFX_f = fract(SAFX_q) - 0.5;
      float SAFX_tt = SAFX_P3.w * 1.25 + SAFX_h(SAFX_id) * 7.0;
      float SAFX_cyc = floor(SAFX_tt);
      float SAFX_ph = fract(SAFX_tt);
      SAFX_HP vec2 SAFX_id2 = SAFX_id + SAFX_cyc * 17.13;
      float SAFX_on = step(1.0 - 0.55 * SAFX_P3.z, SAFX_h(SAFX_id2 + 7.0));
      vec2 SAFX_off = (vec2(SAFX_h(SAFX_id2), SAFX_h(SAFX_id2 + 3.3)) - 0.5) * 0.4;
      float SAFX_rr = length(SAFX_f - SAFX_off);
      float SAFX_rad = SAFX_ph * 0.28;
      float SAFX_wd = 0.03 + 0.03 * SAFX_ph;
      float SAFX_fade = (1.0 - SAFX_ph) * (1.0 - SAFX_ph);
      float SAFX_ring = smoothstep(SAFX_wd, 0.0, abs(SAFX_rr - SAFX_rad)) * SAFX_fade;
      float SAFX_ring2 = smoothstep(SAFX_wd, 0.0, abs(SAFX_rr - SAFX_rad * 0.62)) * SAFX_fade * 0.6;
      float SAFX_crown = smoothstep(0.05, 0.0, SAFX_rr) * max(0.0, 1.0 - SAFX_ph * 6.0) * (0.3 + 0.7 * SAFX_pd);
      float SAFX_df = 1.0 - smoothstep(14.0, 32.0, SAFX_D);
      SAFX_rg = (SAFX_ring + SAFX_ring2 + SAFX_crown * 0.5) * SAFX_on * SAFX_P6.y * SAFX_up * SAFX_df;
      SAFX_rp = (SAFX_f - SAFX_off) * SAFX_rg * 0.03;
    }
    vec3 SAFX_R = reflect(SAFX_V, vec3(0.0, 0.0, 1.0));
    vec3 SAFX_R2 = normalize(vec3(SAFX_R.xy, max(SAFX_R.z * 2.2, 0.22)));
    SAFX_HP vec4 SAFX_c = SAFX_VP * vec4(SAFX_Wp + SAFX_R2 * 90.0, 1.0);
    vec2 SAFX_uv = SAFX_c.xy / SAFX_c.w * 0.5 + 0.5 + SAFX_rp;
    float SAFX_mu = smoothstep(0.10, 0.30, SAFX_uv.x) * smoothstep(0.90, 0.70, SAFX_uv.x);
    float SAFX_mv = smoothstep(0.02, 0.20, SAFX_uv.y) * smoothstep(0.80, 0.60, SAFX_uv.y);
    float SAFX_vis = (SAFX_c.w > 0.1) ? SAFX_mu * SAFX_mv : 0.0;
    vec3 SAFX_rc = texture2D(SAFX_Refl, clamp(SAFX_uv, 0.003, 0.997)).rgb * 0.5
                 + texture2D(SAFX_Refl, clamp(SAFX_uv + vec2(0.0, 0.012), 0.003, 0.997)).rgb * 0.25
                 + texture2D(SAFX_Refl, clamp(SAFX_uv - vec2(0.0, 0.012), 0.003, 0.997)).rgb * 0.25;
    vec3 SAFX_sk = mix(FogColor, FogColor * 1.15 + vec3(0.02, 0.04, 0.07), clamp(SAFX_R.z * 1.5, 0.0, 1.0));
    SAFX_sk = mix(SAFX_sk, vec3(dot(SAFX_sk, vec3(0.33))), 0.30) * 1.15;
    SAFX_rc = mix(SAFX_sk, SAFX_rc, SAFX_vis * 0.85);
    float SAFX_F = 0.04 + 0.96 * pow(1.0 - clamp(-SAFX_V.z, 0.0, 1.0), 5.0);
    fcolor.xyz = mix(fcolor.xyz, SAFX_rc, clamp(SAFX_pd * (0.22 + 0.78 * SAFX_F) * SAFX_P5.x, 0.0, 0.85));
    fcolor.xyz += SAFX_P2.xyz * (pow(max(dot(SAFX_R, SAFX_P1.xyz), 0.0), 90.0) * SAFX_pd * SAFX_sunUp * 1.2);
    fcolor.xyz += vec3(0.85, 0.92, 1.00) * SAFX_rg * 0.30;
  }
)GLSL";

static const char* kWCar = R"GLSL(
  if (SAFX_P3.y > 0.01 && SAFX_P5.z > 0.01) {
    float SAFX_cv = SAFX_P3.y * SAFX_P5.z;
    float SAFX_dn = SAFX_P6.x;
    vec2 SAFX_pl = SAFX_Lp.xy + SAFX_Lp.zz * 0.5;
    float SAFX_hzc = SAFX_Lp.x;
    float SAFX_vert = 0.0;
#ifdef GL_OES_standard_derivatives
    SAFX_HP vec3 SAFX_lg = cross(dFdx(SAFX_Lp) * 16.0, dFdy(SAFX_Lp) * 16.0);
    vec3 SAFX_la = abs(SAFX_lg);
    if (SAFX_la.z >= SAFX_la.x && SAFX_la.z >= SAFX_la.y) {
      SAFX_pl = SAFX_Lp.xy;
    } else if (SAFX_la.y >= SAFX_la.x) {
      SAFX_pl = SAFX_Lp.xz; SAFX_hzc = SAFX_Lp.x; SAFX_vert = 1.0;
    } else {
      SAFX_pl = SAFX_Lp.yz; SAFX_hzc = SAFX_Lp.y; SAFX_vert = 1.0;
    }
#endif
    vec3 SAFX_b1 = SAFX_bead(SAFX_pl * 12.0, SAFX_cv * 0.30 * SAFX_dn, 0.0);
    vec3 SAFX_b2 = SAFX_bead(SAFX_pl * 24.0 + 3.1, SAFX_cv * 0.50 * SAFX_dn, 7.0);
    vec3 SAFX_b3 = SAFX_bead(SAFX_pl * 46.0 + 9.7, SAFX_cv * 0.75 * SAFX_dn, 13.0);
    float SAFX_body = clamp(SAFX_b1.x + SAFX_b2.x + SAFX_b3.x, 0.0, 1.0);
    float SAFX_rim = clamp(SAFX_b1.y + SAFX_b2.y + SAFX_b3.y, 0.0, 1.0);
    float SAFX_spc = clamp(SAFX_b1.z + SAFX_b2.z + SAFX_b3.z, 0.0, 1.0);
    vec3 SAFX_skyc = FogColor * 1.4;
    fcolor.xyz = mix(fcolor.xyz, fcolor.xyz * 0.72 + SAFX_skyc * 0.10, SAFX_body * 0.55);
    fcolor.xyz *= 1.0 - 0.35 * SAFX_rim;
    fcolor.xyz += (SAFX_P2.xyz * 0.5 + 0.12) * SAFX_spc * 0.30 * (0.30 + 0.70 * SAFX_sunUp);
    if (fcolor.a < 0.92) { fcolor.a = max(fcolor.a, SAFX_body * 0.75 + SAFX_rim * 0.25); }
    if (SAFX_P5.w > 0.5 && SAFX_vert > 0.5) {
      SAFX_HP vec2 SAFX_su = vec2(SAFX_hzc * 9.0, SAFX_Lp.z * 2.4);
      SAFX_HP float SAFX_col = floor(SAFX_su.x);
      SAFX_HP float SAFX_cz = floor(SAFX_su.y);
      float SAFX_sh = SAFX_h(vec2(SAFX_col, 3.0));
      float SAFX_pr = step(SAFX_h(vec2(SAFX_col, SAFX_cz)), SAFX_cv * 0.22 * SAFX_dn);
      float SAFX_hp = 1.0 - fract(SAFX_sh * 7.0 + SAFX_P3.w * (0.04 + 0.07 * SAFX_sh));
      float SAFX_fz = fract(SAFX_su.y);
      float SAFX_cx = fract(SAFX_su.x) - 0.5 - (SAFX_sh - 0.5) * 0.3;
      float SAFX_dy = (SAFX_fz - SAFX_hp) * 3.8;
      float SAFX_hr = 0.16 + 0.07 * SAFX_sh;
      float SAFX_dh = length(vec2(SAFX_cx, SAFX_dy * 0.85));
      float SAFX_hb = smoothstep(SAFX_hr, SAFX_hr * 0.75, SAFX_dh);
      float SAFX_hrim = (smoothstep(SAFX_hr, SAFX_hr * 0.88, SAFX_dh) - smoothstep(SAFX_hr * 0.88, SAFX_hr * 0.60, SAFX_dh));
      float SAFX_hsp = smoothstep(0.36, 0.0, length((vec2(SAFX_cx, SAFX_dy * 0.85) / SAFX_hr) - vec2(-0.30, 0.34))) * SAFX_hb;
      float SAFX_tw = 0.035 + 0.02 * SAFX_h(vec2(SAFX_col, floor(SAFX_dy * 6.0)));
      float SAFX_tl = step(0.0, SAFX_dy) * smoothstep(1.7, 0.0, SAFX_dy) * smoothstep(SAFX_tw, 0.0, abs(SAFX_cx)) * 0.45;
      float SAFX_ds = (SAFX_hb * 0.55 + SAFX_hrim * 0.40 + SAFX_tl) * SAFX_pr;
      fcolor.xyz = mix(fcolor.xyz, fcolor.xyz * 0.74 + SAFX_skyc * 0.12, SAFX_ds);
      fcolor.xyz += (SAFX_P2.xyz * 0.5 + 0.12) * SAFX_hsp * SAFX_pr * 0.30 * (0.30 + 0.70 * SAFX_sunUp);
      if (fcolor.a < 0.92) { fcolor.a = max(fcolor.a, SAFX_ds * 0.7); }
    }
  }
)GLSL";

static const char* kWLights = R"GLSL(
  if (SAFX_P6.w > 0.5) {
    vec3 SAFX_acc = vec3(0.0);
    for (int i = 0; i < 8; i++) {
      if (float(i) >= SAFX_P6.w) break;
      vec3 SAFX_ld = SAFX_LP[i].xyz - SAFX_Wp;
      float SAFX_dd = dot(SAFX_ld, SAFX_ld);
      float SAFX_rg2 = SAFX_LP[i].w * SAFX_LP[i].w;
      float SAFX_at = 1.0 - clamp(SAFX_dd / SAFX_rg2, 0.0, 1.0);
      SAFX_at *= SAFX_at;
      float SAFX_nl = (SAFX_nOK > 0.5) ? clamp(dot(SAFX_N, SAFX_ld * inversesqrt(max(SAFX_dd, 0.01))) * 0.6 + 0.4, 0.0, 1.0) : 0.6;
      SAFX_acc += SAFX_LC[i].xyz * (SAFX_at * SAFX_nl);
    }
    float SAFX_nk = 1.0 - 0.85 * smoothstep(0.0, 0.6, SAFX_sunUp);
    fcolor.xyz += (fcolor.xyz * 0.8 + 0.06) * SAFX_acc * SAFX_P7.y * SAFX_nk;
  }
)GLSL";

static const char* kWCarRefl = R"GLSL(
  if (SAFX_nOK > 0.5 && SAFX_P7.x > 0.01 && fcolor.a > 0.92) {
    vec3 SAFX_Rv = reflect(SAFX_V, SAFX_N);
    float SAFX_Fv = 0.05 + 0.95 * pow(1.0 - clamp(dot(-SAFX_V, SAFX_N), 0.0, 1.0), 5.0);
    vec3 SAFX_sv = mix(FogColor * 0.9, FogColor * 1.25 + vec3(0.04, 0.07, 0.12), clamp(SAFX_Rv.z * 1.4 + 0.2, 0.0, 1.0));
    SAFX_sv = mix(vec3(0.12, 0.11, 0.10) * (0.5 + 0.5 * SAFX_sunUp), SAFX_sv, smoothstep(-0.25, 0.10, SAFX_Rv.z));
    SAFX_HP vec4 SAFX_cv2 = SAFX_VP * vec4(SAFX_Wp + SAFX_Rv * 14.0, 1.0);
    vec2 SAFX_uv2 = SAFX_cv2.xy / SAFX_cv2.w * 0.5 + 0.5;
    float SAFX_vs = (SAFX_cv2.w > 0.1) ? smoothstep(0.08, 0.25, SAFX_uv2.x) * smoothstep(0.92, 0.75, SAFX_uv2.x) * smoothstep(0.05, 0.22, SAFX_uv2.y) * smoothstep(0.80, 0.62, SAFX_uv2.y) : 0.0;
    vec3 SAFX_sc2 = texture2D(SAFX_Refl, clamp(SAFX_uv2, 0.003, 0.997)).rgb;
    vec3 SAFX_ev = mix(SAFX_sv, SAFX_sc2, SAFX_vs * 0.55);
    float SAFX_ks = clamp(SAFX_P7.x * (0.10 + 0.90 * SAFX_Fv) * (1.0 + 0.7 * SAFX_P3.y) * 0.55, 0.0, 0.75);
    fcolor.xyz = mix(fcolor.xyz, SAFX_ev, SAFX_ks);
    float SAFX_sl = pow(max(dot(SAFX_Rv, SAFX_P1.xyz), 0.0), 110.0) * 1.3 + pow(max(dot(SAFX_Rv, SAFX_P1.xyz), 0.0), 14.0) * 0.12;
    fcolor.xyz += SAFX_P2.xyz * SAFX_sl * SAFX_sunUp * SAFX_P7.x;
  }
)GLSL";

static bool StmtEnd(const std::string& s, const char* start, size_t& endPos) {
    size_t p = s.find(start); if (p == std::string::npos) return false;
    size_t e = s.find(';', p); if (e == std::string::npos) return false;
    endPos = e + 1; return true;
}

static bool PatchWorld(std::string& vs, std::string& fs, bool vehicle) {
    if (fs.find("FogColor") == std::string::npos || fs.find("gl_FragColor = fcolor;") == std::string::npos) return false;
    if (vs.find("void main") == std::string::npos) return false;
    size_t wp = 0, wn = 0;
    if (!StmtEnd(vs, "vec4 WorldPos =", wp)) return false;
    bool hasN = StmtEnd(vs, "vec3 WorldNormal =", wn);
    std::string insWp = "\n\tSAFX_Wp = WorldPos.xyz; SAFX_Lp = Position;\n";
    if (!hasN) insWp += "\tSAFX_Wn = vec3(0.0);\n";
    const char* insWn = "\n\tSAFX_Wn = WorldNormal;\n";
    if (hasN && wn > wp) { vs.insert(wn, insWn); vs.insert(wp, insWp); }
    else if (hasN) { vs.insert(wp, insWp); vs.insert(wn, insWn); }
    else vs.insert(wp, insWp);
    if (!InsertBeforeMain(vs, "varying highp vec3 SAFX_Wp;\nvarying mediump vec3 SAFX_Wn;\nvarying mediump vec3 SAFX_Lp;\n")) return false;

    std::string body = kWCommon;
    if (L4.giOn > 0.5f) body += kWGI;
    if (L4.lightsOn > 0.5f) body += kWLights;
    if (L4.hazeOn > 0.5f) body += kWHaze;
    if (!vehicle && L4.puddles > 0.5f) body += kWGround;
    if (vehicle && L4.carRefl > 0.05f) body += kWCarRefl;
    if (vehicle && L4.carRain > 0.5f && fs.find("Out_Tex0") != std::string::npos) body += kWCar;
    body += "}\n";
    if (!InsertBeforeMain(fs, kWHeader)) return false;
    size_t gp = fs.find("gl_FragColor = fcolor;");
    if (gp == std::string::npos) return false;
    fs.insert(gp, body);
    fs.insert(0, "#extension GL_OES_standard_derivatives : enable\n");
    return true;
}

// ---- diagnostico: cuantas veces se llama a Build y volcado de los primeros shaders ----
static int g_nBuildCalls = 0, g_nDumped = 0; static bool g_dumpWater = false, g_dumpVeh = false;
static void DumpShader(const char* tag, const char* vs, const char* fs) {
    if (g_nDumped >= 9 || !g_logPath[0]) return;
    char pth[360]; snprintf(pth, sizeof pth, "%s", g_logPath);
    char* s = strstr(pth, "SAFX64.log"); if (!s) return;
    strcpy(s, "SAFX64_shaders.txt");
    FILE* f = fopen(pth, g_nDumped == 0 ? "w" : "a"); if (!f) return;
    fprintf(f, "\n========== %s #%d ==========\n---- VERTEX ----\n%s\n---- FRAGMENT ----\n%s\n", tag, g_nDumped, vs ? vs : "(null)", fs ? fs : "(null)");
    fclose(f); g_nDumped++;
}

static uint64_t Build_hook(void* self, const char* a1, const char* a2) {
    g_nBuildCalls++;
    bool wantWorld = L4.giOn > 0.5f || L4.hazeOn > 0.5f || L4.puddles > 0.5f || L4.carRain > 0.5f || L4.lightsOn > 0.5f || L4.carRefl > 0.05f;
    if (a1 && a2 && (C.water > 0.5f || C.wind > 0.5f || wantWorld)) {
        // El juego llama a Build(fragmentos, vertices). Se detecta por contenido por si cambia el orden.
        bool a1IsVs = strstr(a1, "gl_Position") != nullptr;
        const char* vsSrc = a1IsVs ? a1 : a2;
        const char* fsSrc = a1IsVs ? a2 : a1;
        std::string v0(vsSrc), f0(fsSrc);
        bool water = f0.find("Out_WaterDetail") != std::string::npos && v0.find("Out_WaterDetail") != std::string::npos;
        bool skyVar = v0.find("ReflPos.xy = normalize") != std::string::npos;
        bool foliage = !water && v0.find("BoneToLocal") == std::string::npos && v0.find(kWorldPosAnchor) != std::string::npos
                       && f0.find("discard") != std::string::npos && f0.find("EnvMapCoefficient") == std::string::npos;
        bool vehicle = f0.find("EnvMapCoefficient") != std::string::npos;
        bool doWind = foliage && C.wind > 0.5f;
        bool doWorld = !water && !skyVar && wantWorld && f0.find("Out_Color") != std::string::npos;
        if (g_nBuildCalls <= 6 || g_nBuildCalls % 60 == 0)
            FLog("Build #%d orden=%s vs=%zu fs=%zu agua=%d follaje=%d vehiculo=%d mundo=%d | WorldPos=%d WorldNormal=%d FragColor=%d FogColor=%d",
                 g_nBuildCalls, a1IsVs ? "VS,FS" : "FS,VS", v0.size(), f0.size(), water, foliage, vehicle, doWorld,
                 (int)(v0.find("vec4 WorldPos =") != std::string::npos), (int)(v0.find("vec3 WorldNormal =") != std::string::npos),
                 (int)(f0.find("gl_FragColor = fcolor;") != std::string::npos), (int)(f0.find("FogColor") != std::string::npos));
        std::string v = v0, f = f0;
        bool ch = false;
        if (water && C.water > 0.5f) ch = PatchWater(v, f);
        else {
            if (doWind) ch |= PatchWind(v);
            if (doWorld) ch |= PatchWorld(v, f, vehicle);
        }
        if (ch) {
            bool okc = TestCompile(v.c_str(), f.c_str());
            if (okc) {
                if (water) g_nWater++; else { if (doWind) g_nWind++; if (doWorld) { g_nWorld++; if (vehicle) g_nVeh++; } }
                if (g_nWorld + g_nWater <= 3) DumpShader(water ? "parchado agua" : (vehicle ? "parchado vehiculo" : "parchado mundo"), v.c_str(), f.c_str());
                const char* nv = strdup(v.c_str()); const char* nf = strdup(f.c_str());
                return a1IsVs ? Build_orig(self, nv, nf) : Build_orig(self, nf, nv);
            }
            g_nPatchFail++;
            if (g_nPatchFail <= 3) DumpShader("rechazado", v.c_str(), f.c_str());
            if (doWind && doWorld) {   // reintentar solo con el viento
                std::string v2 = v0, f2 = f0;
                if (PatchWind(v2) && TestCompile(v2.c_str(), f2.c_str())) {
                    g_nWind++;
                    const char* nv = strdup(v2.c_str()); const char* nf = strdup(f2.c_str());
                    return a1IsVs ? Build_orig(self, nv, nf) : Build_orig(self, nf, nv);
                }
            }
        }
    }
    return Build_orig(self, a1, a2);
}

// Viento en pasto/plantas: amplifica la inclinacion que calcula el juego
static void Wind_hook(float v) {
    float gust = 0.04f * sinf((float)fmod(NowSec(), 6283.0) * 1.3f);
    Wind_orig(v * C.grassGain + gust);
}

// =============================== CARGA DEL MOD ===============================
ON_MOD_PRELOAD() { LOG("SAFX 0.5 preload"); }

ON_MOD_LOAD() {
    LoadConfig3();
    LoadConfig4();
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
    pCloudCov   = (float*)sym("_ZN8CWeather13CloudCoverageE");
    pTheCamera  =         sym("TheCamera");
    CalcScreen  = (CalcScreenFn)sym("_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb");

    void* swap  = sym("_Z24RQ_Command_rqSwapBuffersRPc");
    void* rscene = sym("_Z11RenderSceneb");
    void* rmenu  = sym("_ZN12CMenuManager12DrawFrontEndEv");
    void* build = sym("_ZN9ES2Shader5BuildEPKcS1_");
    void* wind  = sym("_ZN14CGrassRenderer20SetGlobalWindBendingEf");

    if (!pCurStored || !pVecToSun || !pSunX || !pSunY || !pSunBlocked || !swap) {
        LOG("faltan simbolos base, plugin desactivado");
        return;
    }
    aml->Hook(swap, (void*)Swap_hook, (void**)&Swap_orig);
    if (rscene) aml->Hook(rscene, (void*)RS_hook, (void**)&RS_orig);
    if (rmenu)  aml->Hook(rmenu, (void*)Menu_hook, (void**)&Menu_orig);
    if (build) aml->Hook(build, (void*)Build_hook, (void**)&Build_orig); else LOG("sin ES2Shader::Build: agua/arboles desactivados");
    if (wind && C.wind > 0.5f) aml->Hook(wind, (void*)Wind_hook, (void**)&Wind_orig);
    void* corA = sym("_ZN8CCoronas14RegisterCoronaEyP7CEntityhhhhRK7CVectorffhhhhhfbfbfbb");
    void* corB = sym("_ZN8CCoronas14RegisterCoronaEyP7CEntityhhhhRK7CVectorffP9RwTexturehhhhfbfbfbb");
    if (corA) aml->Hook(corA, (void*)CoronaA_hook, (void**)&CoronaA_orig);
    if (corB) aml->Hook(corB, (void*)CoronaB_hook, (void**)&CoronaB_orig);
    FLog("coronas: A=%p B=%p nubes(cobertura)=%p", corA, corB, (void*)pCloudCov);
    LOG("SAFX activo (rain=%p wind=%p camera=%p calc=%p build=%p grass=%p)", pRain, pWind, pTheCamera, (void*)CalcScreen, build, wind);
    FLog("simbolos: swap=%p renderScene=%p menu=%p build=%p viento=%p camara=%p rain=%p", swap, rscene, rmenu, build, wind, pTheCamera, (void*)pRain);
    FLog("hooks: swap_orig=%p build_orig=%p scene_orig=%p", (void*)Swap_orig, (void*)Build_orig, (void*)RS_orig);
}
