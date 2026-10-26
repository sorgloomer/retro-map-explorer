#include "renderer.h"
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <cstdio>
#include <cstring>

namespace {

const char* kWorldVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aCol;
layout(location=2) in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec4 vCol; out vec2 vUV; out vec3 vWorld;
void main(){ vCol=aCol; vUV=aUV; vec4 w = uModel*vec4(aPos,1.0); vWorld=w.xyz; gl_Position=uVP*w; }
)";

const char* kWorldFS = R"(#version 300 es
precision highp float;
in vec4 vCol; in vec2 vUV; in vec3 vWorld;
uniform sampler2D uTex;
uniform float uTextured, uCutout, uShade, uExposure, uWire, uFade;
uniform vec3 uSun;
uniform float uLighting, uAmbient, uSunOn;
uniform vec3 uSunDir, uSunColor, uCam;
uniform int uNumLights;
uniform highp sampler2D uLights;
out vec4 o;
void main(){
  vec4 t = uTextured > 0.5 ? texture(uTex, vUV) : vec4(0.8,0.8,0.8,1.0);
  if (uWire > 0.5) { o = vec4(0.78, 0.9, 1.0, 1.0); return; }
  if (uCutout > 0.5 && t.a < 0.02) discard;  // partial alpha is handled by alpha-to-coverage
  vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
  if (uLighting > 0.5) {
    if (dot(n, uCam - vWorld) < 0.0) n = -n;  // face the viewer
    vec3 lit = uSunOn > 0.5 ? vec3(uAmbient) + (1.0 - uAmbient) * max(dot(n, uSunDir), 0.0) * uSunColor : vec3(1.0);
    vec3 add = vec3(0.0);
    for (int i = 0; i < uNumLights; i++) {
      vec4 a = texelFetch(uLights, ivec2(i, 0), 0);
      vec4 b = texelFetch(uLights, ivec2(i, 1), 0);
      vec3 d = a.xyz - vWorld;
      float dist = length(d);
      if (dist < a.w) {
        float f = clamp(1.0 - dist / a.w, 0.0, 1.0);
        f *= f;
        add += b.rgb * (0.55 * f) * (0.35 + 0.65 * max(dot(n, d / max(dist, 1e-3)), 0.0));
      }
    }
    o = vec4(t.rgb * (vCol.rgb * lit + add) * uExposure, vCol.a * t.a * uFade);
  } else {
    float l = abs(dot(n, uSun));
    float shade = mix(1.0, 0.4 + 0.6 * l, uShade);
    o = vec4(t.rgb * vCol.rgb * uExposure * shade, vCol.a * t.a * uFade);
  }
}
)";

const char* kMarkerVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec4 aInst;
uniform mat4 uVP;
uniform vec3 uCam;
uniform float uBase, uPix, uSize, uLift;
out vec3 vN;
void main(){
  float d = distance(aInst.xyz, uCam);
  float s = max(uBase, d * uPix) * uSize;
  float c = cos(aInst.w), si = sin(aInst.w);
  vec3 p = vec3(c*aPos.x + si*aPos.z, aPos.y, -si*aPos.x + c*aPos.z);
  vN = vec3(c*aNrm.x + si*aNrm.z, aNrm.y, -si*aNrm.x + c*aNrm.z);
  gl_Position = uVP * vec4(aInst.xyz + p * s + vec3(0.0, s * uLift, 0.0), 1.0);
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

void pushV(std::vector<float>& v, Vec3 p, Vec3 n) {
    v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
}
void pushTri(std::vector<float>& v, Vec3 a, Vec3 b, Vec3 c) {
    Vec3 n = normalize(cross(b - a, c - a));
    // keep normals pointing away from the origin of the unit shape
    Vec3 centre = (a + b + c) * (1.0f / 3.0f);
    if (dot(n, centre) < 0) n = n * -1.0f;
    pushV(v, a, n); pushV(v, b, n); pushV(v, c, n);
}
void pushQuad(std::vector<float>& v, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
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
    progWorld = compile(kWorldVS, kWorldFS);
    progMarker = compile(kMarkerVS, kMarkerFS);
    progLine = compile(kLineVS, kLineFS);
    buildShapes();
    return true;
}

void Renderer::buildShapes() {
    std::vector<float> d, a, c;
    // diamond (octahedron)
    {
        Vec3 px(1, 0, 0), nx(-1, 0, 0), py(0, 1.4f, 0), ny(0, -1.4f, 0), pz(0, 0, 1), nz(0, 0, -1);
        pushTri(d, px, py, pz); pushTri(d, pz, py, nx); pushTri(d, nx, py, nz); pushTri(d, nz, py, px);
        pushTri(d, pz, ny, px); pushTri(d, nx, ny, pz); pushTri(d, nz, ny, nx); pushTri(d, px, ny, nz);
    }
    // arrow pointing +Z (flat prism)
    {
        float t = 0.35f;
        Vec3 tip(0, 0, 1.8f), l(-1.0f, 0, 0.2f), r(1.0f, 0, 0.2f);
        Vec3 sl(-0.35f, 0, 0.2f), sr(0.35f, 0, 0.2f), bl(-0.35f, 0, -1.0f), br(0.35f, 0, -1.0f);
        auto up = [&](Vec3 p) { return Vec3(p.x, t, p.z); };
        auto dn = [&](Vec3 p) { return Vec3(p.x, -t, p.z); };
        pushTri(a, up(tip), up(l), up(r)); pushTri(a, dn(tip), dn(l), dn(r));
        pushQuad(a, up(sl), up(sr), up(br), up(bl)); pushQuad(a, dn(sl), dn(sr), dn(br), dn(bl));
        pushQuad(a, up(tip), up(l), dn(l), dn(tip)); pushQuad(a, up(tip), up(r), dn(r), dn(tip));
        pushQuad(a, up(l), up(sl), dn(sl), dn(l)); pushQuad(a, up(r), up(sr), dn(sr), dn(r));
        pushQuad(a, up(sl), up(bl), dn(bl), dn(sl)); pushQuad(a, up(sr), up(br), dn(br), dn(sr));
        pushQuad(a, up(bl), up(br), dn(br), dn(bl));
    }
    // cube
    {
        Vec3 p[8];
        for (int i = 0; i < 8; i++) p[i] = Vec3((i & 1) ? 1.f : -1.f, (i & 2) ? 1.f : -1.f, (i & 4) ? 1.f : -1.f);
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
            return Vec3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
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
    freeSet(batches);
    for (auto& set : geomGpu) freeSet(set);
    geomGpu.clear();
    for (unsigned t : textures) glDeleteTextures(1, &t);
    textures.clear();
    texFrame.clear();
    catRanges.clear();
    if (lightTex) glDeleteTextures(1, &lightTex);
    lightTex = 0;
    numLights = 0;
}

void Renderer::freeSet(std::vector<GpuBatch>& set) {
    for (auto& b : set) {
        if (b.lineIbo) glDeleteBuffers(1, &b.lineIbo);
        glDeleteVertexArrays(1, &b.vao);
        glDeleteBuffers(1, &b.vbo);
        glDeleteBuffers(1, &b.ibo);
    }
    set.clear();
}

void Renderer::uploadSet(const std::vector<Batch>& src, std::vector<GpuBatch>& dst) {
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
    // point lights as a 2-row float texture: row 0 = position + outer radius, row 1 = colour + inner radius
    {
        std::vector<float> ld;
        int n = std::min<int>((int)s->lights.size(), 4096);
        for (int i = 0; i < n; i++) ld.insert(ld.end(), {s->lights[i].pos.x, s->lights[i].pos.y, s->lights[i].pos.z, s->lights[i].outer});
        for (int i = 0; i < n; i++) ld.insert(ld.end(), {s->lights[i].color.x, s->lights[i].color.y, s->lights[i].color.z, s->lights[i].inner});
        numLights = n;
        if (n == 0) ld.assign(8, 0.0f);
        glGenTextures(1, &lightTex);
        glBindTexture(GL_TEXTURE_2D, lightTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, std::max(n, 1), 2, 0, GL_RGBA, GL_FLOAT, ld.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    // geometry batches (level + moving objects)
    uploadSet(s->batches, batches);
    geomGpu.resize(s->moverGeoms.size());
    geomRadius.assign(s->moverGeoms.size(), 1.0f);
    for (size_t i = 0; i < s->moverGeoms.size(); i++) {
        uploadSet(s->moverGeoms[i].batches, geomGpu[i]);
        float r2 = 0;
        for (const Batch& b : s->moverGeoms[i].batches)
            for (const SceneVertex& v : b.verts) r2 = std::max(r2, v.x * v.x + v.y * v.y + v.z * v.z);
        geomRadius[i] = std::max(std::sqrt(r2), 0.01f);
    }
    glBindVertexArray(0);

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

void Renderer::drawMarkers(const ViewParams& vp, const RenderOptions& opt, bool xrayPass, int highlight) {
    glUseProgram(progMarker);
    glUniformMatrix4fv(glGetUniformLocation(progMarker, "uVP"), 1, GL_FALSE, vp.viewProj.m);
    glUniform3f(glGetUniformLocation(progMarker, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
    glUniform1f(glGetUniformLocation(progMarker, "uBase"), markerBase * opt.markerScale);
    float pixelWorld = 2.0f * std::tan(vp.fovY * 0.5f) / (float)vp.height;  // world units per pixel at distance 1
    glUniform1f(glGetUniformLocation(progMarker, "uPix"), pixelWorld * 7.0f * opt.markerScale);
    GLint locSize = glGetUniformLocation(progMarker, "uSize");
    GLint locColor = glGetUniformLocation(progMarker, "uColor");
    glUniform1f(glGetUniformLocation(progMarker, "uLift"), 0.6f);
    glBindBuffer(GL_ARRAY_BUFFER, markerInst);
    for (size_t c = 0; c < scene->cats.size(); c++) {
        const Category& cat = scene->cats[c];
        if (!cat.visible) continue;
        if (cat.hasMesh && opt.powerupMeshes) continue;  // drawn as in-game meshes instead
        const CatLines& r = catRanges[c];
        if (r.markerCount == 0) continue;
        float rr = ((cat.color >> 16) & 255) / 255.f, gg = ((cat.color >> 8) & 255) / 255.f, bb = (cat.color & 255) / 255.f;
        glUniform4f(locColor, rr, gg, bb, xrayPass ? 0.3f : 1.0f);
        glUniform1f(locSize, cat.size * (cat.isLine ? 0.5f : 1.0f));
        Shape& s = shapes[cat.shape];
        glBindVertexArray(s.vao);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 16, (void*)((size_t)r.firstMarker * 16));
        glDrawArraysInstanced(GL_TRIANGLES, 0, s.count, r.markerCount);
    }
    if (!xrayPass && highlight >= 0 && highlight < (int)markerSlot.size() && markerSlot[highlight] >= 0) {
        const Marker& m = scene->markers[highlight];
        const Category& cat = scene->cats[m.cat];
        glUniform4f(locColor, 1, 1, 1, 1);
        glUniform1f(locSize, cat.size * 1.6f * (cat.isLine ? 0.5f : 1.0f));
        // next to an in-game powerup mesh the highlight may shrink much closer
        if (cat.hasMesh && opt.powerupMeshes) glUniform1f(glGetUniformLocation(progMarker, "uBase"), markerBase * opt.markerScale * 0.33f);
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
        float rr = ((cat.color >> 16) & 255) / 255.f, gg = ((cat.color >> 8) & 255) / 255.f, bb = (cat.color & 255) / 255.f;
        glUniform4f(locColor, rr, gg, bb, xrayPass ? 0.3f : 1.0f);
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
    Vec3 bg = scene ? scene->background : Vec3(0.08f, 0.1f, 0.14f);
    glClearColor(bg.x, bg.y, bg.z, 1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!scene) return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    bool cull = scene->cullBackfaces && !opt.backFaces && !opt.wireframe;

    if (opt.showGeometry) {
        glUseProgram(progWorld);
        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniform1f(glGetUniformLocation(progWorld, "uShade"), opt.shade);
        glUniform1f(glGetUniformLocation(progWorld, "uExposure"), opt.exposure);
        Vec3 sun = normalize(Vec3(0.35f, 0.85f, 0.4f));
        glUniform3f(glGetUniformLocation(progWorld, "uSun"), sun.x, sun.y, sun.z);
        glUniform1i(glGetUniformLocation(progWorld, "uTex"), 0);
        glUniform1f(glGetUniformLocation(progWorld, "uLighting"), ((opt.sunLight || opt.pointLights) && scene->hasLighting) ? 1.f : 0.f);
        glUniform1f(glGetUniformLocation(progWorld, "uSunOn"), opt.sunLight ? 1.f : 0.f);
        glUniform1f(glGetUniformLocation(progWorld, "uAmbient"), scene->ambient);
        glUniform3f(glGetUniformLocation(progWorld, "uSunDir"), scene->sunDir.x, scene->sunDir.y, scene->sunDir.z);
        glUniform3f(glGetUniformLocation(progWorld, "uSunColor"), scene->sunColor.x, scene->sunColor.y, scene->sunColor.z);
        glUniform3f(glGetUniformLocation(progWorld, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
        glUniform1i(glGetUniformLocation(progWorld, "uNumLights"), opt.pointLights ? numLights : 0);
        glUniform1i(glGetUniformLocation(progWorld, "uLights"), 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lightTex);
        glActiveTexture(GL_TEXTURE0);
        GLint locTextured = glGetUniformLocation(progWorld, "uTextured");
        GLint locCutout = glGetUniformLocation(progWorld, "uCutout");
        GLint locModel = glGetUniformLocation(progWorld, "uModel");
        GLint locLighting = glGetUniformLocation(progWorld, "uLighting");
        GLint locWire = glGetUniformLocation(progWorld, "uWire");
        GLint locFade = glGetUniformLocation(progWorld, "uFade");
        glUniform1f(locFade, 1.0f);
        if (cull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CW); }
        float lightOn = ((opt.sunLight || opt.pointLights) && scene->hasLighting) ? 1.f : 0.f;

        float pixelWorld = 2.0f * std::tan(vp.fovY * 0.5f) / (float)vp.height;  // world units per pixel at distance 1
        // exact in-game scale up close; far away an icon grows so that it keeps a minimum size on screen
        auto iconScale = [&](const Mover& m) {
            int g = m.scaleGeom >= 0 ? m.scaleGeom : m.geom;
            if (!m.powerupMesh || g >= (int)geomRadius.size()) return 1.0f;
            float dist = length(m.path[0] - vp.eye);
            return std::max(1.0f, 16.0f * opt.markerScale * dist * pixelWorld / geomRadius[g]);
        };
        for (const TexAnim& a : scene->texAnims)
            if (a.texture >= 0 && a.texture < (int)texFrame.size() && !a.frames.empty() && a.fps > 0)
                texFrame[a.texture] = a.frames[(size_t)std::floor(vp.time * a.fps) % a.frames.size()];
        // model matrices of the moving objects at the current time
        std::vector<Mat4> models(scene->movers.size(), Mat4::identity());
        std::vector<char> shown(scene->movers.size(), 0);
        for (size_t i = 0; i < scene->movers.size(); i++) {
            const Mover& m = scene->movers[i];
            if (!scene->cats[m.cat].visible || m.geom >= (int)geomGpu.size()) continue;
            if (m.powerupMesh && !opt.powerupMeshes) continue;
            if (m.staticModel) { shown[i] = 1; continue; }
            if (m.billboard) {  // the halo of a powerup icon turns towards the camera, as in the game
                float sc = iconScale(m);
                Vec3 c = m.path[0] + m.anchor * sc;
                Vec3 z = normalize(vp.eye - c);
                Vec3 upv = std::fabs(z.y) > 0.999f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
                Vec3 r = normalize(cross(upv, z));
                Vec3 u = cross(z, r);
                Mat4& M = models[i];
                M.m[0] = r.x * sc; M.m[1] = r.y * sc; M.m[2] = r.z * sc;
                M.m[4] = u.x * sc; M.m[5] = u.y * sc; M.m[6] = u.z * sc;
                M.m[8] = z.x * sc; M.m[9] = z.y * sc; M.m[10] = z.z * sc;
                M.m[12] = c.x; M.m[13] = c.y; M.m[14] = c.z;
                shown[i] = 1;
                continue;
            }
            if (!m.keys.empty()) {  // animated prop: looping translate / rotate steps
                float total = 0;
                for (const AnimKey& k : m.keys) total += k.dur;
                float tt = total > 0 ? std::fmod(vp.time, total) : 0;
                if (tt < 0) tt += total;
                Vec3 off;
                Mat4 R = Mat4::identity();
                float start = 0;
                for (const AnimKey& k : m.keys) {
                    float f = k.dur > 0 ? std::min(1.0f, std::max(0.0f, (tt - start) / k.dur)) : 1.0f;
                    off = off + k.trans * f;
                    if (k.rotDeg != 0) {
                        float a = k.rotDeg * f * 0.0174532925f, cs = std::cos(a), sn = std::sin(a), t = 1 - cs;
                        Vec3 ax = normalize(k.axis);
                        Mat4 Q = Mat4::identity();
                        Q.m[0] = cs + ax.x * ax.x * t;      Q.m[4] = ax.x * ax.y * t - ax.z * sn; Q.m[8] = ax.x * ax.z * t + ax.y * sn;
                        Q.m[1] = ax.y * ax.x * t + ax.z * sn; Q.m[5] = cs + ax.y * ax.y * t;      Q.m[9] = ax.y * ax.z * t - ax.x * sn;
                        Q.m[2] = ax.z * ax.x * t - ax.y * sn; Q.m[6] = ax.z * ax.y * t + ax.x * sn; Q.m[10] = cs + ax.z * ax.z * t;
                        R = Mat4::mul(Q, R);
                    }
                    start += k.dur;
                }
                const Vec3& c = m.pivot;
                R.m[12] = c.x - (R.m[0] * c.x + R.m[4] * c.y + R.m[8] * c.z) + off.x;
                R.m[13] = c.y - (R.m[1] * c.x + R.m[5] * c.y + R.m[9] * c.z) + off.y;
                R.m[14] = c.z - (R.m[2] * c.x + R.m[6] * c.y + R.m[10] * c.z) + off.z;
                models[i] = R;
                shown[i] = 1;
                continue;
            }
            if (m.fixed) {  // powerup icon: spin on the spot
                float a = vp.time * 1.4f + m.phase;
                Mat4& M = models[i];
                // exact in-game scale up close; far away it grows so that it keeps a minimum size on screen
                float sc = iconScale(m);
                M.m[0] = std::cos(a) * sc; M.m[2] = -std::sin(a) * sc;
                M.m[5] = sc;
                M.m[8] = std::sin(a) * sc; M.m[10] = std::cos(a) * sc;
                M.m[12] = m.path[0].x; M.m[13] = m.path[0].y; M.m[14] = m.path[0].z;
                shown[i] = 1;
                continue;
            }
            if (m.length <= 0) continue;
            float d = std::fmod(m.speed * vp.time + m.phase * m.length, m.length);
            if (d < 0) d += m.length;
            Vec3 pos, dir;
            if (!m.sample(d, pos, dir)) continue;
            // The game's followers (helicopter noses, energy arches) are modelled along local +X.
            Vec3 f = normalize(dir);
            Vec3 up = std::fabs(f.y) > 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
            Vec3 z = normalize(cross(f, up));
            Vec3 u = cross(z, f);
            Mat4& M = models[i];
            M.m[0] = f.x; M.m[1] = f.y; M.m[2] = f.z;
            M.m[4] = u.x; M.m[5] = u.y; M.m[6] = u.z;
            M.m[8] = z.x; M.m[9] = z.y; M.m[10] = z.z;
            M.m[12] = pos.x; M.m[13] = pos.y; M.m[14] = pos.z;
            shown[i] = 1;
        }

        auto drawSet = [&](const std::vector<GpuBatch>& set, int pass, const Mat4& model) {
            glUniformMatrix4fv(locModel, 1, GL_FALSE, model.m);
            glUniform1f(locWire, 0.f);
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
                glUniform1f(locTextured, hasTex ? 1.f : 0.f);
                glUniform1f(locCutout, b.blend == BLEND_CUTOUT ? 1.f : 0.f);
                // Alpha-tested textures use alpha-to-coverage: far away, the small holes average out to partial transparency.
                if (b.blend == BLEND_CUTOUT) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                glUniform1f(locLighting, b.blend == BLEND_ADD ? 0.f : lightOn);  // emissive surfaces are not lit
                if (hasTex) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, textures[texFrame[b.tex]]);
                }
                glBindVertexArray(b.vao);
                glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
            }
        };
        auto drawWire = [&]() {
            glUniformMatrix4fv(locModel, 1, GL_FALSE, Mat4::identity().m);
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
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
            }
            if (opt.wireframe) { if (pass == 0) drawWire(); }
            else drawSet(batches, pass, Mat4::identity());
            for (size_t i = 0; i < scene->movers.size(); i++) {
                if (!shown[i]) continue;
                drawSet(geomGpu[scene->movers[i].geom], pass, models[i]);
            }
        }
        // "See-through markers" also covers the in-game powerup meshes: a faint copy where the level hides them
        if (opt.xray && opt.powerupMeshes) {
            glUniform1f(locFade, 0.15f);
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_GREATER);
            for (int pass = 0; pass < 2; pass++)
                for (size_t i = 0; i < scene->movers.size(); i++)
                    if (shown[i] && scene->movers[i].powerupMesh) drawSet(geomGpu[scene->movers[i].geom], pass, models[i]);
            glDepthFunc(GL_LEQUAL);
            glUniform1f(locFade, 1.0f);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_CULL_FACE);
    }

    // annotations: solid pass, then a faint see-through pass where occluded
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (opt.xray) {
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        drawLines(vp, opt, true);
        drawMarkers(vp, opt, true, -1);
    }
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    drawLines(vp, opt, false);
    drawMarkers(vp, opt, false, highlightMarker);
    glDisable(GL_BLEND);
    if (vp.showPivot) drawPivot(vp, opt);
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
    // solid where visible; with "see-through markers" a faint copy shows where the scene hides it
    GLint locColor = glGetUniformLocation(progMarker, "uColor");
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    if (opt.xray) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_FALSE);
        glUniform4f(locColor, 1.0f, 0.25f, 0.9f, 0.3f);
        glDrawArraysInstanced(GL_TRIANGLES, 0, shapes[SHAPE_SPHERE].count, 1);
        glDisable(GL_BLEND);
    }
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glUniform4f(locColor, 1.0f, 0.25f, 0.9f, 1.0f);
    glDrawArraysInstanced(GL_TRIANGLES, 0, shapes[SHAPE_SPHERE].count, 1);
    glBindVertexArray(0);
}
