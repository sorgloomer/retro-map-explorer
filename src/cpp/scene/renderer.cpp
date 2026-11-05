#include "./renderer.h"
#include "scene/mover_pose.h"
#include "profiler.h"
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

const char* kShadowChunk = R"(
uniform sampler2D uShadowMap, uShadowMap2, uAoMap;   // the near and the far cascade of the sun's, and the occlusion from above
uniform mat4 uShadowVP, uShadowVP2, uAoVP;
uniform float uShadowOn;                 // 1: this surface gets shadows and occlusion
uniform vec3 uShadowDir;                 // unit vector towards the sun
uniform vec4 uCas0, uCas1;               // of a cascade: the size of a texel (world), the bias, the radius of the search for blockers (uv), the penumbra per unit of depth
uniform vec2 uCasUV0, uCasUV1;           // the least and the most of the radius of the soft edge (uv)
uniform float uShadowStrength;
uniform float uAoTop, uAoDepth, uAoUV, uUnit, uAoStrength;
const vec2 kDisk[16] = vec2[16](vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.094184101, -0.92938870), vec2(0.34495938, 0.29387760),
  vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
  vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
  vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));
float noiseAngle() { return 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))); }

// How much of the sun one cascade lets reach a point (1: all of it): percentage-closer soft shadows. The blockers are looked for near the point, and
// the farther they are from it the wider the penumbra, so that a shadow is sharp where an object touches the ground and soft away from it. `edge` is how
// far inside the map the point is (negative: outside).
float pcss(sampler2D map, mat4 vp, vec4 c, vec2 limits, vec3 wp, vec3 n, out float edge) {
  vec3 p = wp + n * (c.x * 2.5);
  vec3 s = (vp * vec4(p, 1.0)).xyz * 0.5 + 0.5;
  edge = min(min(s.x, 1.0 - s.x), min(s.y, 1.0 - s.y));
  if (edge < 0.0 || s.z > 1.0) return 1.0;
  float ref = s.z - c.y;
  float a = noiseAngle();
  mat2 R = mat2(cos(a), sin(a), -sin(a), cos(a));
  float sum = 0.0, count = 0.0;
  for (int i = 0; i < 12; i++) {
    float d = texture(map, s.xy + R * kDisk[i] * c.z).r;
    if (d < ref) { sum += d; count += 1.0; }
  }
  if (count < 0.5) return 1.0;
  float radius = clamp((s.z - sum / count) * c.w, limits.x, limits.y);
  float lit = 0.0;
  for (int i = 0; i < 16; i++) lit += (texture(map, s.xy + R * kDisk[i] * radius).r < ref) ? 0.0 : 1.0;
  return lit / 16.0;
}

// The sun's: the near cascade where the point is inside it, the far one beyond, and the two blended at the edge of the near one.
float sunVisibility(vec3 wp, vec3 n) {
  if (uShadowOn < 0.5) return 1.0;
  float e0;
  float v0 = pcss(uShadowMap, uShadowVP, uCas0, uCasUV0, wp, n, e0);
  if (e0 > 0.08) return v0;
  float e1;
  float v1 = pcss(uShadowMap2, uShadowVP2, uCas1, uCasUV1, wp, n, e1);
  return mix(v1, v0, smoothstep(0.0, 0.08, max(e0, 0.0)));
}

// Ambient occlusion from above: the nearest surfaces seen from straight above, round the point: what is higher than the point (but not far higher)
// shuts out some of the sky. It darkens the road under and beside the car and the objects, and the foot of walls; only for surfaces that face up.
float ambientOcclusion(vec3 wp, vec3 n) {
  if (uShadowOn < 0.5) return 1.0;
  vec2 uv = (uAoVP * vec4(wp, 1.0)).xy * 0.5 + 0.5;
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
  float a = noiseAngle();
  mat2 R = mat2(cos(a), sin(a), -sin(a), cos(a));
  float occ = 0.0;
  for (int i = 0; i < 12; i++) {
    float ring = 0.3 + 0.7 * float(i % 4) / 3.0;
    float d = texture(uAoMap, uv + R * kDisk[i] * (uAoUV * ring)).r;
    float dh = (uAoTop - d * uAoDepth - wp.y) / uUnit;   // metres: how far the surface that is seen from above is over the point
    occ += smoothstep(0.12, 0.5, dh) * (1.0 - smoothstep(1.5, 3.0, dh));
  }
  float faceUp = smoothstep(0.3, 0.8, n.y);
  float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
  return 1.0 - uAoStrength * (occ / 12.0) * faceUp * smoothstep(0.0, 0.08, edge);
}
)";

const char* kWorldVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
layout(location=2) in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec4 vCol; out vec2 vUV; out vec3 vWorld;
void main(){ vCol=aCol; vUV=aUV; vec4 w = uModel*vec4(aPos,1.0); vWorld = w.xyz; gl_Position=uVP*w; }
)";

const char* kWorldFS = R"(#version 300 es
precision highp float;
in vec4 vCol; in vec2 vUV; in vec3 vWorld;
uniform sampler2D uTex;
uniform float uTextured, uCutout, uExposure, uWire, uFade;
uniform float uBlendAdd;
out vec4 o;
//SHADOW_CHUNK
void main(){
  vec4 t = uTextured > 0.5 ? texture(uTex, vUV) : vec4(0.8,0.8,0.8,1.0);
  if (uCutout > 0.5 && t.a < 0.02) discard;  // partial alpha is handled by alpha-to-coverage
  if (uBlendAdd > 0.5) {
    o = vec4(t.rgb * vCol.rgb * (uExposure * vCol.a * t.a * uFade), 1.0);
  } else {
    float shade = 1.0;
    if (uShadowOn > 0.5) {
      vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
      if (dot(n, uShadowDir) < 0.0) n = -n;
      float vis = sunVisibility(vWorld, n);
      shade = mix(1.0 - uShadowStrength, 1.0, vis) * ambientOcclusion(vWorld, vec3(n.x, abs(n.y), n.z));
    }
    o = vec4(t.rgb * vCol.rgb * (uExposure * shade), vCol.a * t.a * uFade);
  }
}
)";

// The lit variant of the world shader, for the minigame's car: same inputs plus a normal, and the scene's lighting
// (ambient + one directional light + up to kMaxLights point lights, chosen on the CPU).
const char* kLitVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
layout(location=2) in vec2 aUV;
layout(location=3) in vec3 aNrm;
uniform mat4 uVP;
uniform mat4 uModel;
out vec4 vCol; out vec2 vUV; out vec3 vWorld; out vec3 vN;
void main(){
  vCol=aCol; vUV=aUV;
  vec4 w = uModel*vec4(aPos,1.0);
  vWorld = w.xyz;
  vN = mat3(uModel)*aNrm;
  gl_Position = uVP*w;
}
)";

const char* kLitFS = R"(#version 300 es
precision highp float;
#define MAX_LIGHTS 8
in vec4 vCol; in vec2 vUV; in vec3 vWorld; in vec3 vN;
uniform sampler2D uTex;
uniform float uTextured, uCutout, uExposure, uFade, uBlendAdd;
uniform vec3 uCam;
uniform vec3 uAmbient, uSunDir, uSunColor;      // uSunDir: unit vector towards the sun
uniform int uNumLights;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightCol[MAX_LIGHTS];
uniform vec2 uLightRad[MAX_LIGHTS];             // inner radius (full intensity up to it), outer radius (zero from it)
out vec4 o;
//SHADOW_CHUNK

float lightcurve(float cos_theta) {
  return smoothstep(-0.5, 0.5, cos_theta);
}

void main(){
  vec4 t = uTextured > 0.5 ? texture(uTex, vUV) : vec4(0.8,0.8,0.8,1.0);
  if (uCutout > 0.5 && t.a < 0.02) discard;
  vec3 n = length(vN) > 1e-6 ? normalize(vN) : vec3(0.0, 1.0, 0.0);
  // if (dot(n, uCam - vWorld) < 0.0) n = -n; // normal flipping does not work entirely with interpolated normals
  vec3 light = uAmbient * ambientOcclusion(vWorld, n) + uSunColor * (lightcurve(dot(n, uSunDir)) * sunVisibility(vWorld, n));
  for (int i = 0; i < MAX_LIGHTS; i++) {
    if (i >= uNumLights) break;
    vec3 L = uLightPos[i] - vWorld;
    float d = length(L);
    float fall = smoothstep(uLightRad[i].y, uLightRad[i].x, d);
    light += uLightCol[i] * fall * lightcurve(dot(n, L) / max(d, 1e-4));
  }
  if (uBlendAdd > 0.5) {  // glowing parts (lamps) are not shaded
    o = vec4(t.rgb * vCol.rgb * (uExposure * vCol.a * t.a * uFade), 1.0);
  } else {
    o = vec4(t.rgb * vCol.rgb * light * uExposure, vCol.a * t.a * uFade);
  }
}
)";

const char* kDepthVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec2 vUV;
void main(){ vUV = aUV; gl_Position = uVP * (uModel * vec4(aPos, 1.0)); }
)";

const char* kDepthFS = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uTex;
uniform float uTextured, uCutout;
void main(){ if (uCutout > 0.5 && uTextured > 0.5 && texture(uTex, vUV).a < 0.5) discard; }
)";

const char* kMarkerVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aInst;
uniform mat4 uVP;
uniform vec3 uCam;
uniform float uBase, uPix, uSize, uLift, uRaise;  // uRaise: fixed lift in world units
out vec3 vN;
void main(){
  float d = distance(aInst.xyz, uCam);
  float s = max(uBase, d * uPix) * uSize;
  float c = cos(aInst.w), si = sin(aInst.w);
  vec3 p = vec3(c*aPos.x + si*aPos.z, aPos.y, -si*aPos.x + c*aPos.z);
  vN = vec3(c*aNrm.x + si*aNrm.z, aNrm.y, -si*aNrm.x + c*aNrm.z);
  gl_Position = uVP * vec4(aInst.xyz + p * s + vec3(0.0, s * uLift + uRaise, 0.0), 1.0);
}
)";

const char* kMarkerFS = R"(#version 300 es
precision highp float;
in vec3 vN;
uniform vec4 uColor;
out vec4 o;
void main(){
  float l = 0.55 + 0.45 * max(dot(normalize(vN), normalize(vec3(0.3,0.9,0.4))), 0.0);
  o = vec4(uColor.rgb * l, uColor.a);
}
)";

const char* kLineVS = R"(#version 300 es
layout(location=0) in vec3 aA;
layout(location=1) in vec3 aB;
uniform mat4 uVP;
uniform vec2 uViewport;
uniform float uWidth;
void main(){
  vec4 ca = uVP * vec4(aA, 1.0);
  vec4 cb = uVP * vec4(aB, 1.0);
  const float nearW = 0.05;
  if (ca.w < nearW && cb.w < nearW) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
  if (ca.w < nearW) ca = mix(ca, cb, (nearW - ca.w) / (cb.w - ca.w));
  if (cb.w < nearW) cb = mix(cb, ca, (nearW - cb.w) / (ca.w - cb.w));
  vec2 half_ = uViewport * 0.5;
  vec2 sa = ca.xy / ca.w * half_, sb = cb.xy / cb.w * half_;
  vec2 dir = sb - sa;
  float len = length(dir);
  dir = len > 1e-4 ? dir / len : vec2(1.0, 0.0);
  vec2 nrm = vec2(-dir.y, dir.x);
  float cx = float(gl_VertexID >> 1);
  float cy = float(gl_VertexID & 1) * 2.0 - 1.0;
  vec2 s = mix(sa, sb, cx) + nrm * cy * uWidth * 0.5 + dir * (cx * 2.0 - 1.0) * uWidth * 0.5;
  float z = mix(ca.z / ca.w, cb.z / cb.w, cx) - 0.0004;
  gl_Position = vec4(s / half_, z, 1.0);
}
)";

const char* kLineFS = R"(#version 300 es
precision highp float;
uniform vec4 uColor;
out vec4 o;
void main(){ o = uColor; }
)";

// The six planes of a view volume (a camera's or a light's), from its view-projection matrix; a sphere is tested against them.
struct Frustum {
    float plane[6][4];
    void set(const Mat4f& m) {
        auto row = [&](int r, float out[4]) { for (int c = 0; c < 4; c++) out[c] = m.m[c * 4 + r]; };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0); row(1, r1); row(2, r2); row(3, r3);
        for (int c = 0; c < 4; c++) {
            plane[0][c] = r3[c] + r0[c];   // left
            plane[1][c] = r3[c] - r0[c];   // right
            plane[2][c] = r3[c] + r1[c];   // bottom
            plane[3][c] = r3[c] - r1[c];   // top
            plane[4][c] = r3[c] + r2[c];   // near
            plane[5][c] = r3[c] - r2[c];   // far
        }
        for (auto& p : plane) {
            const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (l > 1e-9f) for (float& v : p) v /= l;
        }
    }
    bool sphereVisible(const Vec3f& c, float r) const {
        for (const auto& p : plane)
            if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -r) return false;
        return true;
    }
};

void pushV(std::vector<float>& v, Vec3f p, Vec3f n) {
    v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
}
void pushTri(std::vector<float>& v, Vec3f a, Vec3f b, Vec3f c) {
    Vec3f n = normalize(cross(b - a, c - a));
    // keep normals pointing away from the origin of the unit shape
    Vec3f centre = (a + b + c) * (1.0f / 3.0f);
    if (dot(n, centre) < 0) n = n * -1.0f;
    pushV(v, a, n); pushV(v, b, n); pushV(v, c, n);
}
void pushQuad(std::vector<float>& v, Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    pushTri(v, a, b, c);
    pushTri(v, a, c, d);
}

}  // namespace

unsigned Renderer::compile(const char* vsSrc, const char* fsSrc) {
    auto mk = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            fprintf(stderr, "shader error: %s\n", log);
        }
        return s;
    };
    GLuint p = glCreateProgram();
    GLuint v = mk(GL_VERTEX_SHADER, vsSrc), f = mk(GL_FRAGMENT_SHADER, fsSrc);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        fprintf(stderr, "link error: %s\n", log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

bool Renderer::init(const char* canvasSelector) {
    EmscriptenWebGLContextAttributes attr;
    emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 2;
    attr.minorVersion = 0;
    attr.alpha = false;
    attr.depth = true;
    attr.antialias = true;
    attr.stencil = false;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context(canvasSelector, &attr);
    if (ctx <= 0) return false;
    emscripten_webgl_make_context_current(ctx);
    auto withShadows = [](const char* source) {
        std::string text = source;
        const size_t at = text.find("//SHADOW_CHUNK");
        if (at != std::string::npos) text.replace(at, 14, kShadowChunk);
        return text;
    };
    progWorld = compile(kWorldVS, withShadows(kWorldFS).c_str());
    progLit = compile(kLitVS, withShadows(kLitFS).c_str());
    progDepth = compile(kDepthVS, kDepthFS);
    // the variants with the model matrix per instance: the uniform is an attribute (a matrix takes the locations 4 to 7)
    auto instanced = [](const char* source) {
        std::string text = source;
        const std::string declaration = "uniform mat4 uModel;";
        const size_t at = text.find(declaration);
        if (at != std::string::npos) text.replace(at, declaration.size(), "layout(location=4) in mat4 aModel;");
        for (size_t from = 0; (from = text.find("uModel", from)) != std::string::npos;) text.replace(from, 6, "aModel");
        return text;
    };
    progWorldInst = compile(instanced(kWorldVS).c_str(), withShadows(kWorldFS).c_str());
    progDepthInst = compile(instanced(kDepthVS).c_str(), kDepthFS);
    progLitInst = compile(instanced(kLitVS).c_str(), withShadows(kLitFS).c_str());
    gpuOk_ = emscripten_webgl_enable_extension(ctx, "EXT_disjoint_timer_query_webgl2") != 0;
    if (gpuOk_)
        for (int w = 0; w < 2; w++) glGenQueries(4, gpuQuery_[w]);
    makeDepthTarget(nearStatic_, kNearSize);
    makeDepthTarget(near_, kNearSize);
    makeDepthTarget(far_, kFarSize);
    makeDepthTarget(aoStatic_, kAoSize);
    makeDepthTarget(ao_, kAoSize);
    progMarker = compile(kMarkerVS, kMarkerFS);
    progLine = compile(kLineVS, kLineFS);
    buildShapes();
    return true;
}

// A depth-only target: a depth texture (read as plain numbers, not compared) that a pass draws into.
void Renderer::makeDepthTarget(DepthTarget& target, int size) {
    unsigned& fbo = target.fbo;
    unsigned& tex = target.tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "shadow map target is not complete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

namespace {
constexpr unsigned kTimeElapsed = 0x88BF;   // GL_TIME_ELAPSED_EXT
constexpr unsigned kGpuDisjoint = 0x8FBB;   // GL_GPU_DISJOINT_EXT
}

void Renderer::gpuBegin(int which) {
    if (!gpuOk_ || gpuPending_[which] >= 4) { gpuActive_[which] = false; return; }
    glBeginQuery(kTimeElapsed, gpuQuery_[which][(gpuHead_[which] + gpuPending_[which]) % 4]);
    gpuActive_[which] = true;
}

void Renderer::gpuEnd(int which) {
    if (!gpuActive_[which]) return;
    glEndQuery(kTimeElapsed);
    gpuActive_[which] = false;
    gpuPending_[which]++;
}

void Renderer::gpuCollect() {
    if (!gpuOk_) return;
    for (int which = 0; which < 2; which++)
        while (gpuPending_[which] > 0) {
            const unsigned id = gpuQuery_[which][gpuHead_[which]];
            GLuint available = 0;
            glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) break;
            GLint disjoint = 0;
            glGetIntegerv(kGpuDisjoint, &disjoint);
            GLuint nanoseconds = 0;
            glGetQueryObjectuiv(id, GL_QUERY_RESULT, &nanoseconds);
            if (!disjoint) prof::Profiler::get().sample(which == 0 ? prof::GPU_SHADOWS : prof::GPU_MAIN, (double)nanoseconds * 1e-6);
            gpuHead_[which] = (gpuHead_[which] + 1) % 4;
            gpuPending_[which]--;
        }
}

void Renderer::buildShapes() {
    std::vector<float> d, a, c;
    // diamond (octahedron)
    {
        Vec3f px(1, 0, 0), nx(-1, 0, 0), py(0, 1.4f, 0), ny(0, -1.4f, 0), pz(0, 0, 1), nz(0, 0, -1);
        pushTri(d, px, py, pz); pushTri(d, pz, py, nx); pushTri(d, nx, py, nz); pushTri(d, nz, py, px);
        pushTri(d, pz, ny, px); pushTri(d, nx, ny, pz); pushTri(d, nz, ny, nx); pushTri(d, px, ny, nz);
    }
    // arrow pointing +Z (flat prism)
    {
        float t = 0.35f;
        Vec3f tip(0, 0, 1.8f), l(-1.0f, 0, 0.2f), r(1.0f, 0, 0.2f);
        Vec3f sl(-0.35f, 0, 0.2f), sr(0.35f, 0, 0.2f), bl(-0.35f, 0, -1.0f), br(0.35f, 0, -1.0f);
        auto up = [&](Vec3f p) { return Vec3f(p.x, t, p.z); };
        auto dn = [&](Vec3f p) { return Vec3f(p.x, -t, p.z); };
        pushTri(a, up(tip), up(l), up(r)); pushTri(a, dn(tip), dn(l), dn(r));
        pushQuad(a, up(sl), up(sr), up(br), up(bl)); pushQuad(a, dn(sl), dn(sr), dn(br), dn(bl));
        pushQuad(a, up(tip), up(l), dn(l), dn(tip)); pushQuad(a, up(tip), up(r), dn(r), dn(tip));
        pushQuad(a, up(l), up(sl), dn(sl), dn(l)); pushQuad(a, up(r), up(sr), dn(sr), dn(r));
        pushQuad(a, up(sl), up(bl), dn(bl), dn(sl)); pushQuad(a, up(sr), up(br), dn(br), dn(sr));
        pushQuad(a, up(bl), up(br), dn(br), dn(bl));
    }
    // cube
    {
        Vec3f p[8];
        for (int i = 0; i < 8; i++) p[i] = Vec3f((i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : -1.f);
        pushQuad(c, p[0], p[1], p[3], p[2]); pushQuad(c, p[4], p[5], p[7], p[6]);
        pushQuad(c, p[0], p[1], p[5], p[4]); pushQuad(c, p[2], p[3], p[7], p[6]);
        pushQuad(c, p[0], p[2], p[6], p[4]); pushQuad(c, p[1], p[3], p[7], p[5]);
    }
    std::vector<float> sph;
    {
        const int lat = 6, lon = 10;
        const float pi = 3.14159265f;
        auto pt = [&](int i, int j) {
            float th = pi * i / lat, ph = 2 * pi * j / lon;
            return Vec3f(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
        };
        for (int i = 0; i < lat; i++)
            for (int j = 0; j < lon; j++) pushQuad(sph, pt(i, j), pt(i + 1, j), pt(i + 1, j + 1), pt(i, j + 1));
    }
    const std::vector<float>* src[4] = {&d, &a, &c, &sph};
    glGenBuffers(1, &markerInst);
    glGenBuffers(1, &pivotBuf);
    glBindBuffer(GL_ARRAY_BUFFER, pivotBuf);
    glBufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
    for (int i = 0; i < 4; i++) {
        Shape& s = shapes[i];
        glGenVertexArrays(1, &s.vao);
        glGenBuffers(1, &s.vbo);
        glBindVertexArray(s.vao);
        glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
        glBufferData(GL_ARRAY_BUFFER, src[i]->size() * 4, src[i]->data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
        glEnableVertexAttribArray(2);
        glVertexAttribDivisor(2, 1);
        s.count = (int)(src[i]->size() / 6);

        // the same shape as triangle edges (hover highlight)
        std::vector<float> w;
        const std::vector<float>& t = *src[i];
        for (size_t k = 0; k + 17 < t.size(); k += 18)
            for (int e = 0; e < 3; e++) {
                const float* p = &t[k + e * 6];
                const float* q = &t[k + ((e + 1) % 3) * 6];
                w.insert(w.end(), p, p + 6);
                w.insert(w.end(), q, q + 6);
            }
        glGenVertexArrays(1, &s.wvao);
        glGenBuffers(1, &s.wvbo);
        glBindVertexArray(s.wvao);
        glBindBuffer(GL_ARRAY_BUFFER, s.wvbo);
        glBufferData(GL_ARRAY_BUFFER, w.size() * 4, w.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
        glEnableVertexAttribArray(2);
        glVertexAttribDivisor(2, 1);
        s.wcount = (int)(w.size() / 6);
    }
    glBindVertexArray(0);
    glGenBuffers(1, &lineVbo);
    glGenVertexArrays(1, &lineVao);
}

void Renderer::clearScene() {
    for (unsigned b : geomInst_) if (b) glDeleteBuffers(1, &b);
    geomInst_.clear();
    geomBounds_.clear();
    nearValid_ = farValid_ = aoValid_ = false;
    freeSet(batches);
    freeSet(collisionGpu);
    for (auto& set : geomGpu) freeSet(set);
    geomGpu.clear();
    for (unsigned t : textures) glDeleteTextures(1, &t);
    textures.clear();
    texFrame.clear();
    catRanges.clear();
}

// The collision overlay is read on demand, after the scene was set.
void Renderer::uploadCollision() {
    freeSet(collisionGpu);
    if (scene) uploadSet(scene->collision.batches, collisionGpu);
    glBindVertexArray(0);
}

void Renderer::freeSet(std::vector<GpuBatch>& set) {
    for (auto& b : set) {
        if (b.lineIbo) glDeleteBuffers(1, &b.lineIbo);
        glDeleteVertexArrays(1, &b.vao);
        if (b.nbo) glDeleteBuffers(1, &b.nbo);
        glDeleteBuffers(1, &b.vbo);
        glDeleteBuffers(1, &b.ibo);
    }
    set.clear();
}

unsigned Renderer::instanceBuffer(int geom) {
    if (geom < 0 || geom >= (int)geomInst_.size()) return 0;
    if (geomInst_[geom]) return geomInst_[geom];
    unsigned buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    const Mat4f identity = Mat4f::identity();
    glBufferData(GL_ARRAY_BUFFER, sizeof identity.m, identity.m, GL_DYNAMIC_DRAW);   // (one matrix: a draw of one object reads it too)
    for (const GpuBatch& b : geomGpu[geom]) {
        glBindVertexArray(b.vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        for (int k = 0; k < 4; k++) {
            glEnableVertexAttribArray(4 + k);
            glVertexAttribPointer(4 + k, 4, GL_FLOAT, GL_FALSE, 64, (void*)(size_t)(16 * k));
            glVertexAttribDivisor(4 + k, 1);
        }
    }
    glBindVertexArray(0);
    geomInst_[geom] = buffer;
    return buffer;
}

void Renderer::uploadSet(const std::vector<Batch>& src, std::vector<GpuBatch>& dst, bool lit) {
    for (const Batch& b : src) {
        GpuBatch g;
        g.tex = b.texture;
        g.blend = b.blend;
        g.twoSided = b.twoSided;
        g.count = (int)b.idx.size();
        g.src = &b;
        glGenVertexArrays(1, &g.vao);
        glGenBuffers(1, &g.vbo);
        glGenBuffers(1, &g.ibo);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, b.verts.size() * sizeof(SceneVertex), b.verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, b.idx.size() * 4, b.idx.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SceneVertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(SceneVertex), (void*)12);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(SceneVertex), (void*)16);
        if (lit) {
            // smooth vertex normals: the area-weighted mean of the faces that share the vertex
            std::vector<Vec3f> nrm(b.verts.size(), Vec3f(0, 0, 0));
            for (size_t i = 0; i + 2 < b.idx.size(); i += 3) {
                const SceneVertex &a = b.verts[b.idx[i]], &bb = b.verts[b.idx[i + 1]], &c = b.verts[b.idx[i + 2]];
                Vec3f fn = cross(Vec3f(bb.x - a.x, bb.y - a.y, bb.z - a.z), Vec3f(c.x - a.x, c.y - a.y, c.z - a.z));
                nrm[b.idx[i]] = nrm[b.idx[i]] + fn;
                nrm[b.idx[i + 1]] = nrm[b.idx[i + 1]] + fn;
                nrm[b.idx[i + 2]] = nrm[b.idx[i + 2]] + fn;
            }
            for (Vec3f& n : nrm) {
                float l = length(n);
                n = l > 1e-12f ? n * (1.0f / l) : Vec3f(0, 1, 0);
            }
            // the file's own normals win where it has them (the computed ones are only the fallback)
            if (b.normals.size() == b.verts.size())
                for (size_t i = 0; i < nrm.size(); i++)
                    if (length(b.normals[i]) > 1e-6f) nrm[i] = b.normals[i];
            glGenBuffers(1, &g.nbo);
            glBindBuffer(GL_ARRAY_BUFFER, g.nbo);
            glBufferData(GL_ARRAY_BUFFER, nrm.size() * sizeof(Vec3f), nrm.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3f), (void*)0);
        }
        dst.push_back(g);
    }
}

void Renderer::setScene(const Scene* s, float radius) {
    clearScene();
    scene = s;
    if (!s) return;
    // textures
    for (const Image& img : s->textures) {
        unsigned t;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        textures.push_back(t);
        texFrame.push_back((int)textures.size() - 1);
    }
    // geometry batches (level + moving objects)
    uploadSet(s->batches, batches);
    uploadSet(s->collision.batches, collisionGpu);
    geomGpu.resize(s->moverGeoms.size());
    geomRevision.assign(s->moverGeoms.size(), 0);
    for (size_t i = 0; i < s->moverGeoms.size(); i++) {
        uploadSet(s->moverGeoms[i].batches, geomGpu[i], s->moverGeoms[i].lit);
        geomRevision[i] = s->moverGeoms[i].revision;
    }
    glBindVertexArray(0);
    // a sphere round every geometry of the movers, to leave out what is out of view
    geomBounds_.assign(s->moverGeoms.size(), GeomBounds());
    for (size_t i = 0; i < s->moverGeoms.size(); i++) {
        Vec3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        bool any = false;
        for (const Batch& b : s->moverGeoms[i].batches)
            for (const SceneVertex& v : b.verts) {
                lo = Vec3f(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
                hi = Vec3f(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
                any = true;
            }
        if (!any) continue;
        geomBounds_[i].centre = (lo + hi) * 0.5f;
        geomBounds_[i].radius = 0.5f * length(hi - lo) + 1e-3f;
        geomBounds_[i].valid = true;
    }
    geomInst_.assign(s->moverGeoms.size(), 0u);

    sceneRadius = std::max(1.0f, radius);
    markerBase = sceneRadius * 0.0035f;

    // markers sorted by category
    size_t nc = s->cats.size();
    catRanges.assign(nc, CatLines());
    std::vector<float> inst;
    markerSlot.assign(s->markers.size(), -1);
    markerOrder.clear();
    for (size_t c = 0; c < nc; c++) {
        catRanges[c].firstMarker = (int)markerOrder.size();
        for (size_t i = 0; i < s->markers.size(); i++) {
            if (s->markers[i].cat != (int)c) continue;
            const Marker& m = s->markers[i];
            markerSlot[i] = (int)markerOrder.size();
            markerOrder.push_back((int)i);
            inst.insert(inst.end(), {m.pos.x, m.pos.y, m.pos.z, m.yaw});
        }
        catRanges[c].markerCount = (int)markerOrder.size() - catRanges[c].firstMarker;
    }
    glBindBuffer(GL_ARRAY_BUFFER, markerInst);
    glBufferData(GL_ARRAY_BUFFER, inst.size() * 4, inst.empty() ? nullptr : inst.data(), GL_STATIC_DRAW);

    // lines (plus vertical beams for marker categories that ask for one)
    std::vector<float> ln;
    float beamH = sceneRadius * 0.05f;
    for (size_t c = 0; c < nc; c++) {
        catRanges[c].firstLine = (int)(ln.size() / 6);
        for (const Line& l : s->lines)
            if (l.cat == (int)c) ln.insert(ln.end(), {l.a.x, l.a.y, l.a.z, l.b.x, l.b.y, l.b.z});
        catRanges[c].lineCount = (int)(ln.size() / 6) - catRanges[c].firstLine;
        catRanges[c].firstBeam = (int)(ln.size() / 6);
        if (s->cats[c].beam)
            for (const Marker& m : s->markers)
                if (m.cat == (int)c) ln.insert(ln.end(), {m.pos.x, m.pos.y, m.pos.z, m.pos.x, m.pos.y + beamH, m.pos.z});
        catRanges[c].beamCount = (int)(ln.size() / 6) - catRanges[c].firstBeam;
    }
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glBufferData(GL_ARRAY_BUFFER, ln.size() * 4, ln.empty() ? nullptr : ln.data(), GL_STATIC_DRAW);
}

// Powerup scale factor: 1 (the exact in-game size) up close; far away an icon grows so that it keeps a minimum size on screen.
// Meshes and diamonds both use it, so their sizes do not depend on which of them is drawn.
float Renderer::iconScale(const Vec3f& at, const ViewParams& vp, const RenderOptions& opt) const {
    float pixelWorld = 2.0f * std::tan(vp.fovY * 0.5f) / (float)vp.height;  // world units per pixel at distance 1
    return std::max(1.0f, 16.0f * opt.powerupScale * length(at - vp.eye) * pixelWorld / scene->pickupRadius);
}

void Renderer::drawMarkers(const ViewParams& vp, const RenderOptions& opt, bool xrayPass, int highlight) {
    glUseProgram(progMarker);
    glUniformMatrix4fv(glGetUniformLocation(progMarker, "uVP"), 1, GL_FALSE, vp.viewProj.m);
    glUniform3f(glGetUniformLocation(progMarker, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
    glUniform1f(glGetUniformLocation(progMarker, "uBase"), markerBase * opt.markerScale);
    float pixelWorld = 2.0f * std::tan(vp.fovY * 0.5f) / (float)vp.height;  // world units per pixel at distance 1
    glUniform1f(glGetUniformLocation(progMarker, "uPix"), pixelWorld * 7.0f * opt.markerScale);
    GLint locSize = glGetUniformLocation(progMarker, "uSize");
    GLint locColor = glGetUniformLocation(progMarker, "uColor");
    GLint locLift = glGetUniformLocation(progMarker, "uLift"), locRaise = glGetUniformLocation(progMarker, "uRaise");
    GLint locBase = glGetUniformLocation(progMarker, "uBase"), locPix = glGetUniformLocation(progMarker, "uPix");
    // Powerup diamonds are drawn like the in-game icon meshes (see iconScale): a diamond whose half width is kDiamond of the mesh's
    // bounding radius, centred where the mesh is centred. Like the meshes, it grows from the bottom of the powerup.
    const float kDiamond = 0.585f;
    const float baseBase = markerBase * opt.markerScale, basePix = pixelWorld * 7.0f * opt.markerScale;
    auto powerupSize = [&](float k) {
        glUniform1f(locBase, kDiamond * scene->pickupRadius * k);
        glUniform1f(locPix, pixelWorld * 16.0f * opt.powerupScale * kDiamond * k);
        // the height of the centre above the bottom grows with the size; the bottom stays put
        glUniform1f(locLift, (scene->pickupCentreY - scene->pickupBottomY) / (kDiamond * scene->pickupRadius * k));
        glUniform1f(locRaise, scene->pickupBottomY);
    };
    auto plainSize = [&]() {
        glUniform1f(locBase, baseBase);
        glUniform1f(locPix, basePix);
        glUniform1f(locLift, 0.6f);
        glUniform1f(locRaise, 0.0f);
    };
    glBindBuffer(GL_ARRAY_BUFFER, markerInst);
    for (size_t c = 0; c < scene->cats.size(); c++) {
        const Category& cat = scene->cats[c];
        if (!cat.visible) continue;
        if (cat.hasMesh && opt.powerupMeshes) continue;  // drawn as in-game meshes instead
        const CatLines& r = catRanges[c];
        if (r.markerCount == 0) continue;
        const Color3f col = Color3f::from3b(cat.color);
        glUniform4f(locColor, col.r, col.g, col.b, xrayPass ? opt.xrayStrength : 1.0f);
        if (cat.hasMesh) { powerupSize(1.0f); glUniform1f(locSize, 1.0f); }
        else { plainSize(); glUniform1f(locSize, cat.size * (cat.isLine ? 0.5f : 1.0f)); }
        Shape& s = shapes[cat.shape];
        glBindVertexArray(s.vao);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 16, (void*)((size_t)r.firstMarker * 16));
        glDrawArraysInstanced(GL_TRIANGLES, 0, s.count, r.markerCount);
    }
    if (!xrayPass && highlight >= 0 && highlight < (int)markerSlot.size() && markerSlot[highlight] >= 0) {
        const Marker& m = scene->markers[highlight];
        const Category& cat = scene->cats[m.cat];
        glUniform4f(locColor, 1, 1, 1, 1);
        // The wireframe is concentric with what it marks and a little larger. A powerup keeps the size and centre of its
        // diamond / in-game mesh; other markers are scaled about the diamond's lift of 0.6, which needs a lift of 0.6 / k.
        const float kHighlight = 1.20f;
        if (cat.hasMesh) { powerupSize(kHighlight); glUniform1f(locSize, 1.0f); }
        else {
            plainSize();
            glUniform1f(locSize, cat.size * kHighlight * (cat.isLine ? 0.5f : 1.0f));
            glUniform1f(locLift, 0.6f / kHighlight);
        }
        Shape& s = shapes[cat.shape];
        glBindVertexArray(s.wvao);  // wireframe: see-through
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 16, (void*)((size_t)markerSlot[highlight] * 16));
        glDrawArraysInstanced(GL_LINES, 0, s.wcount, 1);
    }
    glBindVertexArray(0);
}

void Renderer::drawLines(const ViewParams& vp, const RenderOptions& opt, bool xrayPass) {
    glUseProgram(progLine);
    glUniformMatrix4fv(glGetUniformLocation(progLine, "uVP"), 1, GL_FALSE, vp.viewProj.m);
    glUniform2f(glGetUniformLocation(progLine, "uViewport"), (float)vp.width, (float)vp.height);
    glUniform1f(glGetUniformLocation(progLine, "uWidth"), opt.lineWidth * (xrayPass ? 0.7f : 1.0f));
    GLint locColor = glGetUniformLocation(progLine, "uColor");
    glBindVertexArray(lineVao);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);
    for (size_t c = 0; c < scene->cats.size(); c++) {
        const Category& cat = scene->cats[c];
        if (!cat.visible) continue;
        const auto col = cat.color.to_f();
        glUniform4f(locColor, col.r, col.g, col.b, xrayPass ? opt.xrayStrength : 1.0f);
        const CatLines& r = catRanges[c];
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1 && !opt.beams) continue;
            int first = pass == 0 ? r.firstLine : r.firstBeam;
            int count = pass == 0 ? r.lineCount : r.beamCount;
            if (count == 0) continue;
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)((size_t)first * 24));
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)((size_t)first * 24 + 12));
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
        }
    }
    glVertexAttribDivisor(0, 0);
    glVertexAttribDivisor(1, 0);
    glBindVertexArray(0);
}

void Renderer::render(const ViewParams& vp, const RenderOptions& opt, int highlightMarker) {
    glViewport(0, 0, vp.width, vp.height);
    Vec3f bg = scene ? scene->background : Vec3f(0.08f, 0.1f, 0.14f);
    glClearColor(bg.x, bg.y, bg.z, 1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!scene) return;
    gpuCollect();
    int drawCalls = 0, instancedDraws = 0, moversDrawn = 0, moversCulled = 0;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    bool cull = scene->cullBackfaces && !opt.backFaces && !opt.wireframe;

    if (opt.showGeometry) {
        glUseProgram(progWorld);

        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniform1i(glGetUniformLocation(progWorld, "uTex"), 0);
        glUseProgram(progWorldInst);
        glUniformMatrix4fv(glGetUniformLocation(progWorldInst, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniform1i(glGetUniformLocation(progWorldInst, "uTex"), 0);
        glUseProgram(progWorld);

        GLint locExposure = glGetUniformLocation(progWorld, "uExposure");
        GLint locBlendAdd = glGetUniformLocation(progWorld, "uBlendAdd");
        GLint locTextured = glGetUniformLocation(progWorld, "uTextured");
        GLint locCutout = glGetUniformLocation(progWorld, "uCutout");
        GLint locModel = glGetUniformLocation(progWorld, "uModel");
        GLint locWire = glGetUniformLocation(progWorld, "uWire");
        GLint locFade = glGetUniformLocation(progWorld, "uFade");

        // the lit program (the car): its own uniforms, kept in step with the world program's exposure and fade
        struct WorldLocs { GLint exposure, blendAdd, textured, cutout, model, wire, fade, shadowOn; };
        const WorldLocs worldLocs{locExposure, locBlendAdd, locTextured, locCutout, locModel, locWire, locFade, glGetUniformLocation(progWorld, "uShadowOn")};
        const WorldLocs litLocs{glGetUniformLocation(progLit, "uExposure"), glGetUniformLocation(progLit, "uBlendAdd"),
                                glGetUniformLocation(progLit, "uTextured"), glGetUniformLocation(progLit, "uCutout"),
                                glGetUniformLocation(progLit, "uModel"), glGetUniformLocation(progLit, "uWire"),
                                glGetUniformLocation(progLit, "uFade"), glGetUniformLocation(progLit, "uShadowOn")};
        const WorldLocs instLocs{glGetUniformLocation(progWorldInst, "uExposure"), glGetUniformLocation(progWorldInst, "uBlendAdd"),
                                 glGetUniformLocation(progWorldInst, "uTextured"), glGetUniformLocation(progWorldInst, "uCutout"), -1,
                                 glGetUniformLocation(progWorldInst, "uWire"), glGetUniformLocation(progWorldInst, "uFade"), glGetUniformLocation(progWorldInst, "uShadowOn")};
        const WorldLocs litInstLocs{glGetUniformLocation(progLitInst, "uExposure"), glGetUniformLocation(progLitInst, "uBlendAdd"),
                                    glGetUniformLocation(progLitInst, "uTextured"), glGetUniformLocation(progLitInst, "uCutout"), -1,
                                    glGetUniformLocation(progLitInst, "uWire"), glGetUniformLocation(progLitInst, "uFade"), glGetUniformLocation(progLitInst, "uShadowOn")};
        float curExposure = opt.exposure, curFade = 1.0f;
        auto setExposure = [&](float v) { curExposure = v; glUniform1f(locExposure, v); };
        auto setFade = [&](float v) { curFade = v; glUniform1f(locFade, v); };
        setFade(1.0f);
        setExposure(opt.exposure);
        glUniform1f(locBlendAdd, 0.0f);

        if (cull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CW); }

        for (const TexAnim& a : scene->texAnims)
            if (a.texture >= 0 && a.texture < (int)texFrame.size() && !a.frames.empty() && a.fps > 0)
                texFrame[a.texture] = a.frames[(size_t)std::floor(vp.time * a.fps) % a.frames.size()];
        // the geometries that were changed since they were uploaded (the skid marks): their data goes up again
        for (size_t g = 0; g < scene->moverGeoms.size() && g < geomGpu.size(); g++) {
            if (scene->moverGeoms[g].revision == geomRevision[g]) continue;
            geomRevision[g] = scene->moverGeoms[g].revision;
            for (GpuBatch& b : geomGpu[g]) {
                if (!b.src) continue;
                glBindVertexArray(b.vao);
                glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
                glBufferData(GL_ARRAY_BUFFER, b.src->verts.size() * sizeof(SceneVertex), b.src->verts.data(), GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, b.src->idx.size() * 4, b.src->idx.data(), GL_DYNAMIC_DRAW);
                b.count = (int)b.src->idx.size();
            }
            glBindVertexArray(0);
        }
        // model matrices of the moving objects at the current time
        std::vector<Mat4f> models(scene->movers.size(), Mat4f::identity());
        std::vector<char> shown(scene->movers.size(), 0);
        for (size_t i = 0; i < scene->movers.size(); i++) {
            const Mover& m = scene->movers[i];
            if (m.hidden || m.geom >= (int)geomGpu.size()) continue;
            if (m.cat >= 0 && !scene->cats[m.cat].visible) continue;
            if (m.powerupMesh && !opt.powerupMeshes) continue;
            if (m.external) { models[i] = m.model; shown[i] = 1; continue; }
            if (m.staticModel) { shown[i] = 1; continue; }
            if (m.billboard) {  // the halo of a powerup icon turns towards the camera, as in the game
                float sc = iconScale(m.path[0], vp, opt);
                Vec3f c = m.path[0] + Vec3f(0, scene->pickupBottomY * (1.0f - sc), 0) + m.anchor * sc;  // scaled from the icon's bottom
                Vec3f z = normalize(vp.eye - c);
                Vec3f upv = std::fabs(z.y) > 0.999f ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0);
                Vec3f r = normalize(cross(upv, z));
                Vec3f u = cross(z, r);
                Mat4f& M = models[i];
                M.m[0] = r.x * sc; M.m[1] = r.y * sc; M.m[2] = r.z * sc;
                M.m[4] = u.x * sc; M.m[5] = u.y * sc; M.m[6] = u.z * sc;
                M.m[8] = z.x * sc; M.m[9] = z.y * sc; M.m[10] = z.z * sc;
                M.m[12] = c.x; M.m[13] = c.y; M.m[14] = c.z;
                shown[i] = 1;
                continue;
            }
            if (m.fixed) {  // powerup icon: spin on the spot
                float a = vp.time * 1.4f + m.phase;
                Mat4f& M = models[i];
                // exact in-game scale up close; far away it grows so that it keeps a minimum size on screen
                float sc = iconScale(m.path[0], vp, opt);  // grows from the icon's bottom, which stays where it is
                M.m[0] = std::cos(a) * sc; M.m[2] = -std::sin(a) * sc;
                M.m[5] = sc;
                M.m[8] = std::sin(a) * sc; M.m[10] = std::cos(a) * sc;
                M.m[12] = m.path[0].x; M.m[13] = m.path[0].y + scene->pickupBottomY * (1.0f - sc); M.m[14] = m.path[0].z;
                shown[i] = 1;
                continue;
            }
            if (moverPoseAt(m, vp.time, models[i])) shown[i] = 1;   // the animated props and the path followers
        }

        // Whether a mover is inside a view volume (the camera's, or a light's): a sphere round its geometry, moved and scaled by its matrix. Not for what
        // changes shape (the skid marks, the effects), the car (it is where the light probe is) and the debug markers.
        auto moverVisible = [&](size_t i, const Frustum& frustum) {
            const Mover& m = scene->movers[i];
            if (m.staticModel || m.alwaysOnTop || m.lightProbe || m.geom < 0 || m.geom >= (int)geomBounds_.size() || !geomBounds_[m.geom].valid) return true;
            const GeomBounds& bounds = geomBounds_[m.geom];
            const Mat4f& M = models[i];
            auto columnLength = [&](int c) { return std::sqrt(M.m[c * 4] * M.m[c * 4] + M.m[c * 4 + 1] * M.m[c * 4 + 1] + M.m[c * 4 + 2] * M.m[c * 4 + 2]); };
            const float scale = std::max(columnLength(0), std::max(columnLength(1), columnLength(2)));
            return frustum.sphereVisible(M.transformPoint(bounds.centre), bounds.radius * scale);
        };
        // The model matrices of some movers of one geometry, into its instance buffer.
        auto uploadInstances = [&](int geom, const std::vector<int>& movers) {
            std::vector<float> data;
            data.reserve(movers.size() * 16);
            for (int i : movers) data.insert(data.end(), models[(size_t)i].m, models[(size_t)i].m + 16);
            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer(geom));
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(data.size() * sizeof(float)), data.data(), GL_DYNAMIC_DRAW);
        };
        Frustum viewFrustum;
        viewFrustum.set(vp.viewProj);
        std::vector<char> inView(scene->movers.size(), 0);
        for (size_t i = 0; i < scene->movers.size(); i++) {
            if (!shown[i]) continue;
            inView[i] = moverVisible(i, viewFrustum) ? 1 : 0;
            if (inView[i]) moversDrawn++; else moversCulled++;
        }

        // The shadow maps: what the car and the movable objects (the external movers) block of the sun, seen from the sun, and the level and
        // those seen from straight above, both over the area round the car. The main pass reads them.
        bool shadowsOn = false;
        if (scene->softShadows && progDepth && near_.fbo && far_.fbo && ao_.fbo) {
            shadowsOn = true;
            gpuBegin(0);
            const float unit = std::max(1e-3f, scene->shadowUnit);
            Vec3f sun = normalize(scene->lighting.sunDir);
            if (sun.y < 0.15f) sun = normalize(Vec3f(sun.x, 0.15f, sun.z));
            const Vec3f centre = scene->shadowCentre;
            const Vec3f across = normalize(cross(Vec3f(0, 1, 0), sun)), upward = cross(sun, across);
            // an orthographic view along the sun's light over a square of side 2 * half, round (cu, cv) across and upward and cw along the light, as deep as range
            auto sunMatrix = [&](float cu, float cv, float cw, float half, float range) {
                Mat4f m = Mat4f::identity();
                const float farW = cw + range * 0.5f;   // (depth grows towards the ground)
                m.m[0] = across.x / half;  m.m[4] = across.y / half;  m.m[8] = across.z / half;  m.m[12] = -cu / half;
                m.m[1] = upward.x / half;  m.m[5] = upward.y / half;  m.m[9] = upward.z / half;  m.m[13] = -cv / half;
                m.m[2] = -sun.x * 2.0f / range; m.m[6] = -sun.y * 2.0f / range; m.m[10] = -sun.z * 2.0f / range; m.m[14] = 2.0f * farW / range - 1.0f;
                return m;
            };
            const GLint depthVP = glGetUniformLocation(progDepth, "uVP"), depthModel = glGetUniformLocation(progDepth, "uModel");
            const GLint depthTextured = glGetUniformLocation(progDepth, "uTextured"), depthCutout = glGetUniformLocation(progDepth, "uCutout");
            const GLint depthInstVP = glGetUniformLocation(progDepthInst, "uVP");
            const GLint depthInstTextured = glGetUniformLocation(progDepthInst, "uTextured"), depthInstCutout = glGetUniformLocation(progDepthInst, "uCutout");
            auto beginDepth = [&](const DepthTarget& target, int size, const Mat4f& vpm, bool clear) {
                glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
                glViewport(0, 0, size, size);
                glDepthMask(GL_TRUE);
                if (clear) glClear(GL_DEPTH_BUFFER_BIT);
                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glUseProgram(progDepthInst);
                glUniformMatrix4fv(depthInstVP, 1, GL_FALSE, vpm.m);
                glUniform1i(glGetUniformLocation(progDepthInst, "uTex"), 0);
                glUseProgram(progDepth);
                glUniformMatrix4fv(depthVP, 1, GL_FALSE, vpm.m);
                glUniform1i(glGetUniformLocation(progDepth, "uTex"), 0);
            };
            auto drawDepth = [&](const std::vector<GpuBatch>& set, const Mat4f& model, bool cutouts, int instances = 0) {
                const bool inst = instances > 1;
                glUseProgram(inst ? progDepthInst : progDepth);
                if (!inst) glUniformMatrix4fv(depthModel, 1, GL_FALSE, model.m);
                for (const GpuBatch& b : set) {
                    if (b.blend == BLEND_ALPHA || b.blend == BLEND_ADD || (b.blend == BLEND_CUTOUT && !cutouts)) continue;
                    const bool hasTex = b.tex >= 0;
                    glUniform1f(inst ? depthInstTextured : depthTextured, hasTex ? 1.f : 0.f);
                    glUniform1f(inst ? depthInstCutout : depthCutout, b.blend == BLEND_CUTOUT ? 1.f : 0.f);
                    if (hasTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, textures[texFrame[b.tex]]); }
                    glBindVertexArray(b.vao);
                    if (inst) { glDrawElementsInstanced(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr, instances); instancedDraws++; }
                    else glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
                    drawCalls++;
                }
            };
            auto drawMoversDepth = [&](bool cutouts, const Mat4f& lightVP) {   // what moves: the car and the movable objects that are in the light's view
                Frustum lightFrustum;
                lightFrustum.set(lightVP);
                std::unordered_map<int, std::vector<int>> groups;   // by geometry: the ones that can be drawn together
                for (size_t i = 0; i < scene->movers.size(); i++) {
                    const Mover& m = scene->movers[i];
                    if (!shown[i] || !m.external || m.alwaysOnTop || !moverVisible(i, lightFrustum)) continue;
                    groups[m.geom].push_back((int)i);
                }
                for (const auto& group : groups) {
                    if (group.second.size() >= 2) {
                        uploadInstances(group.first, group.second);
                        drawDepth(geomGpu[group.first], Mat4f::identity(), cutouts, (int)group.second.size());
                    } else {
                        drawDepth(geomGpu[group.first], models[(size_t)group.second[0]], cutouts);
                    }
                }
                glBindVertexArray(0);
            };
            auto copyDepth = [&](const DepthTarget& from, const DepthTarget& to, int size) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, from.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to.fbo);
                glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
            };
            const bool sunMoved = length(sun - keySun_) > 1e-4f;
            if (sunMoved) { nearValid_ = farValid_ = false; keySun_ = sun; }

            // the near cascade: 56 m, on a grid of cells of a quarter of that; the level is drawn again when the car is in another cell
            const float nearHalf = 28.0f * unit, nearRange = 120.0f * unit, nearStep = 14.0f * unit;
            const int nearCell[3] = {(int)std::floor(dot(centre, across) / nearStep), (int)std::floor(dot(centre, upward) / nearStep), (int)std::floor(dot(centre, sun) / nearStep)};
            const Mat4f nearVP = sunMatrix(((float)nearCell[0] + 0.5f) * nearStep, ((float)nearCell[1] + 0.5f) * nearStep, ((float)nearCell[2] + 0.5f) * nearStep, nearHalf, nearRange);
            if (!nearValid_ || std::memcmp(nearCell, nearKey_, sizeof nearCell) != 0) {
                prof::Scope timer(prof::SHADOW_LEVEL);
                beginDepth(nearStatic_, kNearSize, nearVP, true);
                drawDepth(batches, Mat4f::identity(), true);
                std::memcpy(nearKey_, nearCell, sizeof nearCell);
                nearValid_ = true;
            }
            {
                prof::Scope timer(prof::SHADOW_MOVING);
                copyDepth(nearStatic_, near_, kNearSize);
                beginDepth(near_, kNearSize, nearVP, false);
                drawMoversDepth(true, nearVP);
            }

            // the far cascade: twenty times as wide, the level only, drawn again when the car has gone a long way
            const float farHalf = 280.0f * unit, farRange = 1400.0f * unit, farStep = 64.0f * unit;
            const int farCell[3] = {(int)std::floor(dot(centre, across) / farStep), (int)std::floor(dot(centre, upward) / farStep), (int)std::floor(dot(centre, sun) / farStep)};
            const Mat4f farVP = sunMatrix(((float)farCell[0] + 0.5f) * farStep, ((float)farCell[1] + 0.5f) * farStep, ((float)farCell[2] + 0.5f) * farStep, farHalf, farRange);
            if (!farValid_ || std::memcmp(farCell, farKey_, sizeof farCell) != 0) {
                prof::Scope timer(prof::SHADOW_LEVEL);
                beginDepth(far_, kFarSize, farVP, true);
                drawDepth(batches, Mat4f::identity(), true);
                std::memcpy(farKey_, farCell, sizeof farCell);
                farValid_ = true;
            }

            // the occlusion from above: x and z across, the height as the depth (down is deeper); the same way, with a grid
            const float aoHalf = 12.0f * unit, aoDepth = 36.0f * unit, aoStep = 6.0f * unit;
            const int aoCell[3] = {(int)std::floor(centre.x / aoStep), (int)std::floor(centre.z / aoStep), (int)std::floor(centre.y / aoStep)};
            const float aoCx = ((float)aoCell[0] + 0.5f) * aoStep, aoCz = ((float)aoCell[1] + 0.5f) * aoStep;
            const float aoTop = ((float)aoCell[2] + 0.5f) * aoStep + aoDepth * 0.5f;
            Mat4f aoVP = Mat4f::identity();
            for (int k = 0; k < 16; k++) aoVP.m[k] = 0.0f;
            aoVP.m[0] = 1.0f / aoHalf;  aoVP.m[12] = -aoCx / aoHalf;
            aoVP.m[9] = 1.0f / aoHalf;  aoVP.m[13] = -aoCz / aoHalf;
            aoVP.m[6] = -2.0f / aoDepth; aoVP.m[14] = 2.0f * aoTop / aoDepth - 1.0f;
            aoVP.m[15] = 1.0f;
            if (!aoValid_ || std::memcmp(aoCell, aoKey_, sizeof aoCell) != 0) {
                prof::Scope timer(prof::SHADOW_LEVEL);
                beginDepth(aoStatic_, kAoSize, aoVP, true);
                drawDepth(batches, Mat4f::identity(), false);
                std::memcpy(aoKey_, aoCell, sizeof aoCell);
                aoValid_ = true;
            }
            {
                prof::Scope timer(prof::SHADOW_MOVING);
                copyDepth(aoStatic_, ao_, kAoSize);
                beginDepth(ao_, kAoSize, aoVP, false);
                drawMoversDepth(false, aoVP);
            }

            glBindVertexArray(0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, vp.width, vp.height);
            // what the shaders of the level and the car read
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, near_.tex);
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, ao_.tex);
            glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, far_.tex);
            glActiveTexture(GL_TEXTURE0);
            const float nearTexel = 2.0f * nearHalf / (float)kNearSize, farTexel = 2.0f * farHalf / (float)kFarSize;
            for (unsigned prog : {progWorld, progLit, progWorldInst, progLitInst}) {
                glUseProgram(prog);
                auto at = [&](const char* name) { return glGetUniformLocation(prog, name); };
                glUniform1i(at("uShadowMap"), 1);
                glUniform1i(at("uAoMap"), 2);
                glUniform1i(at("uShadowMap2"), 3);
                glUniformMatrix4fv(at("uShadowVP"), 1, GL_FALSE, nearVP.m);
                glUniformMatrix4fv(at("uShadowVP2"), 1, GL_FALSE, farVP.m);
                glUniformMatrix4fv(at("uAoVP"), 1, GL_FALSE, aoVP.m);
                glUniform3f(at("uShadowDir"), sun.x, sun.y, sun.z);
                glUniform4f(at("uCas0"), nearTexel, 1.5f * nearTexel / nearRange + 0.0004f, 0.5f * unit / (2.0f * nearHalf), nearRange * 0.06f / (2.0f * nearHalf));
                glUniform2f(at("uCasUV0"), 1.5f / (float)kNearSize, 0.7f * unit / (2.0f * nearHalf));
                glUniform4f(at("uCas1"), farTexel, 1.5f * farTexel / farRange + 0.0002f, 2.0f * unit / (2.0f * farHalf), farRange * 0.06f / (2.0f * farHalf));
                glUniform2f(at("uCasUV1"), 1.5f / (float)kFarSize, 6.0f * unit / (2.0f * farHalf));
                glUniform1f(at("uShadowStrength"), 0.4f);
                glUniform1f(at("uAoTop"), aoTop);
                glUniform1f(at("uAoDepth"), aoDepth);
                glUniform1f(at("uAoUV"), 1.4f * unit / (2.0f * aoHalf));
                glUniform1f(at("uUnit"), unit);
                glUniform1f(at("uAoStrength"), 0.7f);
            }
            glUseProgram(progWorld);
            gpuEnd(0);
        }
        gpuBegin(1);

        // Lighting of the lit meshes (the car): the sun and ambient light, and the point lights that reach it, nearest first.
        lightsInRange_ = 0;
        lightColours_.clear();
        {
            const Mover* car = nullptr;
            Vec3f carPos;
            for (size_t i = 0; i < scene->movers.size(); i++)
                if (shown[i] && scene->movers[i].lit && (scene->movers[i].lightProbe || !car)) {
                    car = &scene->movers[i];
                    carPos = Vec3f(models[i].m[12], models[i].m[13], models[i].m[14]);
                    if (car->lightProbe) break;
                }
            if (car) {
                const Lighting& li = scene->lighting;
                std::vector<std::pair<float, int>> near;
                for (size_t k = 0; k < li.lights.size(); k++) {
                    float s = length(li.lights[k].pos - carPos) - li.lights[k].outer; // (distance beyond the light's reach, index): negative = in reach
                    if (s < 0) near.emplace_back(s, (int)k);
                }
                lightsInRange_ = (int)near.size();
                const int n = (int)std::min<size_t>(near.size(), kMaxLights);
                std::sort(near.begin(), near.end());
                for (const auto& entry : near) {
                    const Vec3f& c = li.lights[entry.second].color;
                    auto byte = [](float v) { return (unsigned)(std::max(0.0f, std::min(1.0f, v)) * 255.0f + 0.5f); };
                    lightColours_.push_back(byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z));
                }
                float pos[kMaxLights * 3] = {}, col[kMaxLights * 3] = {}, rad[kMaxLights * 2] = {};
                for (int k = 0; k < n; k++) {
                    const PointLight& pl = li.lights[near[k].second];
                    pos[k * 3] = pl.pos.x; pos[k * 3 + 1] = pl.pos.y; pos[k * 3 + 2] = pl.pos.z;
                    col[k * 3] = pl.color.x; col[k * 3 + 1] = pl.color.y; col[k * 3 + 2] = pl.color.z;
                    rad[k * 2] = pl.inner; rad[k * 2 + 1] = pl.outer;
                }
                for (unsigned prog : {progLit, progLitInst}) {
                glUseProgram(prog);
                glUniformMatrix4fv(glGetUniformLocation(prog, "uVP"), 1, GL_FALSE, vp.viewProj.m);
                glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
                glUniform3f(glGetUniformLocation(prog, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
                glUniform3f(glGetUniformLocation(prog, "uAmbient"), li.ambient.x, li.ambient.y, li.ambient.z);
                glUniform3f(glGetUniformLocation(prog, "uSunDir"), li.sunDir.x, li.sunDir.y, li.sunDir.z);
                glUniform3f(glGetUniformLocation(prog, "uSunColor"), li.sunColor.x * scene->carBrightness, li.sunColor.y * scene->carBrightness, li.sunColor.z * scene->carBrightness);
                glUniform1i(glGetUniformLocation(prog, "uNumLights"), n);
                glUniform3fv(glGetUniformLocation(prog, "uLightPos"), kMaxLights, pos);
                glUniform3fv(glGetUniformLocation(prog, "uLightCol"), kMaxLights, col);
                glUniform2fv(glGetUniformLocation(prog, "uLightRad"), kMaxLights, rad);
                }
                glUseProgram(progWorld);
            }
        }

        auto drawSet = [&](const std::vector<GpuBatch>& set, int pass, const Mat4f& model, bool lit = false, int instances = 0) {
            const bool inst = instances > 1;
            const WorldLocs& L = lit ? (inst ? litInstLocs : litLocs) : (inst ? instLocs : worldLocs);
            if (lit || inst) {
                glUseProgram(lit ? (inst ? progLitInst : progLit) : (inst ? progWorldInst : progWorld));
                glUniform1f(L.exposure, curExposure);
                glUniform1f(L.fade, curFade);
            }
            if (!inst) glUniformMatrix4fv(L.model, 1, GL_FALSE, model.m);
            glUniform1f(L.wire, 0.f);
            for (const GpuBatch& b : set) {
                bool blended = b.blend == BLEND_ALPHA || b.blend == BLEND_ADD;
                if (blended != (pass == 1)) continue;
                if (cull) {
                    if (!b.twoSided) glEnable(GL_CULL_FACE);
                    else glDisable(GL_CULL_FACE);
                }
                if (b.blend == BLEND_ADD) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                bool hasTex = opt.textured && b.tex >= 0;
                glUniform1f(L.shadowOn, (shadowsOn && (b.blend == BLEND_OPAQUE || b.blend == BLEND_CUTOUT)) ? 1.0f : 0.0f);
                glUniform1f(L.blendAdd, b.blend == BLEND_ADD ? 1.0f : 0.0f);
                glUniform1f(L.textured, hasTex ? 1.f : 0.f);
                glUniform1f(L.cutout, b.blend == BLEND_CUTOUT ? 1.f : 0.f);
                // Alpha-tested textures use alpha-to-coverage: far away, the small holes average out to partial transparency.
                if (b.blend == BLEND_CUTOUT) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                if (hasTex) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, textures[texFrame[b.tex]]);
                }
                glBindVertexArray(b.vao);
                if (inst) { glDrawElementsInstanced(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr, instances); instancedDraws++; }
                else glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
                drawCalls++;
            }
            if (lit || inst) glUseProgram(progWorld);
        };
        auto drawWire = [&]() {
            glUniformMatrix4fv(locModel, 1, GL_FALSE, Mat4f::identity().m);
            glUniform1f(locWire, 1.f);
            for (GpuBatch& b : batches) {
                if (!b.lineIbo && b.src) {
                    std::vector<uint32_t> li;
                    li.reserve(b.src->idx.size() * 2);
                    for (size_t k = 0; k + 2 < b.src->idx.size(); k += 3) {
                        uint32_t a = b.src->idx[k], bb = b.src->idx[k + 1], c = b.src->idx[k + 2];
                        li.insert(li.end(), {a, bb, bb, c, c, a});
                    }
                    b.lineCount = (int)li.size();
                    glGenBuffers(1, &b.lineIbo);
                    glBindVertexArray(b.vao);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.lineIbo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, li.size() * 4, li.data(), GL_STATIC_DRAW);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.ibo);
                }
                glBindVertexArray(b.vao);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.lineIbo);
                glDrawElements(GL_LINES, b.lineCount, GL_UNSIGNED_INT, nullptr);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.ibo);
            }
            glUniform1f(locWire, 0.f);
        };
        // See-through strength also covers the in-game powerup meshes: a faint copy where the level hides them. It is drawn
        // before the solid meshes, so it is tested against the level alone: it never shows through the powerup itself.
        auto drawPowerupGhosts = [&]() {
            // back faces are culled here whatever the game does for its level (Re-Volt draws double sided): they would show
            // as a second, mirrored layer inside the faint copy
            const bool levelCull = cull;
            cull = !opt.backFaces && !opt.wireframe;
            if (cull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CW); }
            setFade(opt.xrayStrength);
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_GREATER);
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < scene->movers.size(); i++)
                    if (shown[i] && inView[i] && scene->movers[i].powerupMesh) {
                        setExposure(scene->movers[i].billboard ? opt.haloExposure : opt.exposure);
                        drawSet(geomGpu[scene->movers[i].geom], pass, models[i], scene->movers[i].lit);
                    }
            glDepthFunc(GL_LEQUAL);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            setFade(1.0f);
            cull = levelCull;
            if (!cull) glDisable(GL_CULL_FACE);
        };
        // The movers that are drawn: those in view; the ones of one geometry (that are not lit) together, in one instanced call.
        struct Group { int geom; bool external, halo, lit; std::vector<int> movers; };
        std::vector<Group> groups;
        {
            std::unordered_map<long long, int> groupOf;
            for (size_t i = 0; i < scene->movers.size(); i++) {
                const Mover& m = scene->movers[i];
                if (!shown[i] || !inView[i] || m.alwaysOnTop) continue;
                const long long key = ((long long)m.geom << 3) | (m.lit ? 4 : 0) | (m.external ? 2 : 0) | (m.billboard ? 1 : 0);
                auto found = groupOf.find(key);
                if (found == groupOf.end()) {
                    found = groupOf.emplace(key, (int)groups.size()).first;
                    groups.push_back({m.geom, m.external, m.billboard, m.lit, {}});
                }
                groups[(size_t)found->second].movers.push_back((int)i);
            }
        }
        // which: 0 all of them, 1 the external ones only, 2 all but those
        auto drawMovers = [&](int pass, int which) {
            for (const Group& group : groups) {
                if ((which == 1 && !group.external) || (which == 2 && group.external)) continue;
                setExposure(group.halo ? opt.haloExposure : opt.exposure);
                if (group.movers.size() >= 2) {
                    uploadInstances(group.geom, group.movers);
                    drawSet(geomGpu[group.geom], pass, Mat4f::identity(), group.lit, (int)group.movers.size());
                } else {
                    drawSet(geomGpu[group.geom], pass, models[(size_t)group.movers[0]], group.lit);
                }
            }
        };
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
            }
            // The minigame car's see-through parts (windows) go in before the level's, writing depth: the level's blended surfaces
            // (water) then blend over them where they are nearer, and are rejected where they are behind. Drawn after the
            // water instead, the windows would paint over it as if nothing was in between.
            if (pass == 1) {
                glDepthMask(GL_TRUE);
                drawMovers(pass, 1);
                glDepthMask(GL_FALSE);
            }
            setExposure(opt.exposure);  // the movers below change it: reset for the level in every pass
            if (opt.wireframe) { if (pass == 0) drawWire(); }
            else {
                prof::Scope timer(prof::RENDER_LEVEL);
                drawSet(batches, pass, Mat4f::identity());
            }
            if (pass == 0 && opt.xrayEnabled() && opt.powerupMeshes) drawPowerupGhosts();
            prof::Scope moversTimer(prof::RENDER_MOVERS);
            drawMovers(pass, pass == 1 ? 2 : 0);   // (the external ones of the blended pass went in before the level's blended surfaces)
        }
        setExposure(opt.exposure);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_CULL_FACE);
        // debug markers: over everything, whatever is in front of them (no depth test, so they show through the car's hull)
        glDisable(GL_DEPTH_TEST);
        for (size_t i = 0; i < scene->movers.size(); i++)
            if (shown[i] && scene->movers[i].alwaysOnTop) drawSet(geomGpu[scene->movers[i].geom], 0, models[i], scene->movers[i].lit);
        glEnable(GL_BLEND);  // then the translucent ones (the collision spheres)
        glDepthMask(GL_FALSE);
        for (size_t i = 0; i < scene->movers.size(); i++)
            if (shown[i] && scene->movers[i].alwaysOnTop) drawSet(geomGpu[scene->movers[i].geom], 1, models[i], scene->movers[i].lit);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // the game's collision mesh: a translucent overlay, nudged towards the camera so it wins against the surfaces it coincides with
    if (opt.showCollision && !collisionGpu.empty()) {
        glUseProgram(progWorld);
        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uModel"), 1, GL_FALSE, Mat4f::identity().m);
        glUniform1f(glGetUniformLocation(progWorld, "uTextured"), 0.f);
        glUniform1f(glGetUniformLocation(progWorld, "uCutout"), 0.f);
        glUniform1f(glGetUniformLocation(progWorld, "uBlendAdd"), 0.f);
        glUniform1f(glGetUniformLocation(progWorld, "uExposure"), 1.0f);
        glUniform1f(glGetUniformLocation(progWorld, "uFade"), 1.0f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        for (const GpuBatch& b : collisionGpu) {
            glBindVertexArray(b.vao);
            glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // annotations: solid pass, then a faint see-through pass where occluded
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (opt.xrayEnabled()) {
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        drawLines(vp, opt, true);
        drawMarkers(vp, opt, true, -1);
    }
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    {
        prof::Profiler& profiler = prof::Profiler::get();
        profiler.sample(prof::COUNT_DRAWS, drawCalls);
        profiler.sample(prof::COUNT_INSTANCED, instancedDraws);
        profiler.sample(prof::COUNT_MOVERS_DRAWN, moversDrawn);
        profiler.sample(prof::COUNT_MOVERS_CULLED, moversCulled);
    }
    prof::Scope marksTimer(prof::RENDER_MARKS);
    drawLines(vp, opt, false);
    drawMarkers(vp, opt, false, highlightMarker);
    glDisable(GL_BLEND);
    if (vp.showPivot) drawPivot(vp, opt);
    gpuEnd(1);
}

// Bright marker at the orbit / pan pivot, drawn on top of everything while the mouse button is held.
void Renderer::drawPivot(const ViewParams& vp, const RenderOptions& opt) {
    glUseProgram(progMarker);
    glUniformMatrix4fv(glGetUniformLocation(progMarker, "uVP"), 1, GL_FALSE, vp.viewProj.m);
    glUniform3f(glGetUniformLocation(progMarker, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
    float pixelWorld = 2.0f * std::tan(vp.fovY * 0.5f) / (float)vp.height;
    glUniform1f(glGetUniformLocation(progMarker, "uBase"), markerBase * 0.2f);
    glUniform1f(glGetUniformLocation(progMarker, "uPix"), pixelWorld * 9.0f);  // ~9 px radius: bigger the farther away
    glUniform1f(glGetUniformLocation(progMarker, "uSize"), 1.0f);
    glUniform1f(glGetUniformLocation(progMarker, "uLift"), 0.0f);  // centred on the pivot, not floating above it
    float inst[4] = {vp.pivot.x, vp.pivot.y, vp.pivot.z, 0.0f};
    glBindBuffer(GL_ARRAY_BUFFER, pivotBuf);
    glBufferSubData(GL_ARRAY_BUFFER, 0, 16, inst);
    glBindVertexArray(shapes[SHAPE_SPHERE].vao);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 16, (void*)0);
    // solid where visible; with see-through strength above zero a faint copy shows where the scene hides it
    GLint locColor = glGetUniformLocation(progMarker, "uColor");
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (opt.xrayEnabled()) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        glUniform4f(locColor, 1.0f, 0.25f, 0.9f, opt.xrayStrength);
        glDrawArraysInstanced(GL_TRIANGLES, 0, shapes[SHAPE_SPHERE].count, 1);
        glDisable(GL_BLEND);
    }
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUniform4f(locColor, 1.0f, 0.25f, 0.9f, 1.0f);
    glDrawArraysInstanced(GL_TRIANGLES, 0, shapes[SHAPE_SPHERE].count, 1);
    glBindVertexArray(0);
}
