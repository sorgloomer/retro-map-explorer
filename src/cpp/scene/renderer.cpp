#include "./renderer.h"
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
out vec4 vCol; out vec2 vUV;
void main(){ vCol=aCol; vUV=aUV; gl_Position=uVP*(uModel*vec4(aPos,1.0)); }
)";

const char* kWorldFS = R"(#version 300 es
precision highp float;
in vec4 vCol; in vec2 vUV;
uniform sampler2D uTex;
uniform float uTextured, uCutout, uExposure, uWire, uFade;
uniform float uBlendAdd;
out vec4 o;
void main(){
  vec4 t = uTextured > 0.5 ? texture(uTex, vUV) : vec4(0.8,0.8,0.8,1.0);
  if (uCutout > 0.5 && t.a < 0.02) discard;  // partial alpha is handled by alpha-to-coverage
  if (uBlendAdd > 0.5) {
    o = vec4(t.rgb * vCol.rgb * (uExposure * vCol.a * t.a * uFade), 1.0);
  } else {
    o = vec4(t.rgb * vCol.rgb * uExposure, vCol.a * t.a * uFade);
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
uniform vec4 uLightPos[MAX_LIGHTS];             // xyz, outer radius
uniform vec4 uLightCol[MAX_LIGHTS];             // rgb, inner radius
out vec4 o;
void main(){
  vec4 t = uTextured > 0.5 ? texture(uTex, vUV) : vec4(0.8,0.8,0.8,1.0);
  if (uCutout > 0.5 && t.a < 0.02) discard;
  // the face looks at the viewer: windows and thin parts are lit from the side that is seen
  vec3 n = length(vN) > 1e-6 ? normalize(vN) : vec3(0.0, 1.0, 0.0);
  if (dot(n, uCam - vWorld) < 0.0) n = -n;
  vec3 light = uAmbient + uSunColor * max(dot(n, uSunDir), 0.0);
  for (int i = 0; i < MAX_LIGHTS; i++) {
    if (i >= uNumLights) break;
    vec3 L = uLightPos[i].xyz - vWorld;
    float d = length(L);
    float fall = 1.0 - smoothstep(uLightCol[i].w, uLightPos[i].w, d);
    light += uLightCol[i].rgb * fall * max(dot(n, L / max(d, 1e-4)), 0.0);
  }
  if (uBlendAdd > 0.5) {  // glowing parts (lamps) are not shaded
    o = vec4(t.rgb * vCol.rgb * (uExposure * vCol.a * t.a * uFade), 1.0);
  } else {
    o = vec4(t.rgb * vCol.rgb * light * uExposure, vCol.a * t.a * uFade);
  }
}
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
    progWorld = compile(kWorldVS, kWorldFS);
    progLit = compile(kLitVS, kLitFS);
    progMarker = compile(kMarkerVS, kMarkerFS);
    progLine = compile(kLineVS, kLineFS);
    buildShapes();
    return true;
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
    for (size_t i = 0; i < s->moverGeoms.size(); i++) uploadSet(s->moverGeoms[i].batches, geomGpu[i], s->moverGeoms[i].lit);
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

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    bool cull = scene->cullBackfaces && !opt.backFaces && !opt.wireframe;

    if (opt.showGeometry) {
        glUseProgram(progWorld);

        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniform1i(glGetUniformLocation(progWorld, "uTex"), 0);

        GLint locExposure = glGetUniformLocation(progWorld, "uExposure");
        GLint locBlendAdd = glGetUniformLocation(progWorld, "uBlendAdd");
        GLint locTextured = glGetUniformLocation(progWorld, "uTextured");
        GLint locCutout = glGetUniformLocation(progWorld, "uCutout");
        GLint locModel = glGetUniformLocation(progWorld, "uModel");
        GLint locWire = glGetUniformLocation(progWorld, "uWire");
        GLint locFade = glGetUniformLocation(progWorld, "uFade");

        // the lit program (the car): its own uniforms, kept in step with the world program's exposure and fade
        struct WorldLocs { GLint exposure, blendAdd, textured, cutout, model, wire, fade; };
        const WorldLocs worldLocs{locExposure, locBlendAdd, locTextured, locCutout, locModel, locWire, locFade};
        const WorldLocs litLocs{glGetUniformLocation(progLit, "uExposure"), glGetUniformLocation(progLit, "uBlendAdd"),
                                glGetUniformLocation(progLit, "uTextured"), glGetUniformLocation(progLit, "uCutout"),
                                glGetUniformLocation(progLit, "uModel"), glGetUniformLocation(progLit, "uWire"),
                                glGetUniformLocation(progLit, "uFade")};
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
        // model matrices of the moving objects at the current time
        std::vector<Mat4> models(scene->movers.size(), Mat4::identity());
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
                Vec3f off;
                Mat4 R = Mat4::identity();
                float start = 0;
                for (const AnimKey& k : m.keys) {
                    float f = k.dur > 0 ? std::min(1.0f, std::max(0.0f, (tt - start) / k.dur)) : 1.0f;
                    off = off + k.trans * f;
                    if (k.rotDeg != 0) {
                        float a = k.rotDeg * f * 0.0174532925f, cs = std::cos(a), sn = std::sin(a), t = 1 - cs;
                        Vec3f ax = normalize(k.axis);
                        Mat4 Q = Mat4::identity();
                        Q.m[0] = cs + ax.x * ax.x * t;      Q.m[4] = ax.x * ax.y * t - ax.z * sn; Q.m[8] = ax.x * ax.z * t + ax.y * sn;
                        Q.m[1] = ax.y * ax.x * t + ax.z * sn; Q.m[5] = cs + ax.y * ax.y * t;      Q.m[9] = ax.y * ax.z * t - ax.x * sn;
                        Q.m[2] = ax.z * ax.x * t - ax.y * sn; Q.m[6] = ax.z * ax.y * t + ax.x * sn; Q.m[10] = cs + ax.z * ax.z * t;
                        R = Mat4::mul(Q, R);
                    }
                    start += k.dur;
                }
                const Vec3f& c = m.pivot;
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
                float sc = iconScale(m.path[0], vp, opt);  // grows from the icon's bottom, which stays where it is
                M.m[0] = std::cos(a) * sc; M.m[2] = -std::sin(a) * sc;
                M.m[5] = sc;
                M.m[8] = std::sin(a) * sc; M.m[10] = std::cos(a) * sc;
                M.m[12] = m.path[0].x; M.m[13] = m.path[0].y + scene->pickupBottomY * (1.0f - sc); M.m[14] = m.path[0].z;
                shown[i] = 1;
                continue;
            }
            if (m.length <= 0) continue;
            float d = std::fmod(m.speed * vp.time + m.phase * m.length, m.length);
            if (d < 0) d += m.length;
            Vec3f pos, dir;
            if (!m.sample(d, pos, dir)) continue;
            // The game's followers (helicopter noses, energy arches) are modelled along local +X.
            Vec3f f = normalize(dir);
            Vec3f up = std::fabs(f.y) > 0.99f ? Vec3f(0, 0, 1) : Vec3f(0, 1, 0);
            Vec3f z = normalize(cross(f, up));
            Vec3f u = cross(z, f);
            Mat4& M = models[i];
            M.m[0] = f.x; M.m[1] = f.y; M.m[2] = f.z;
            M.m[4] = u.x; M.m[5] = u.y; M.m[6] = u.z;
            M.m[8] = z.x; M.m[9] = z.y; M.m[10] = z.z;
            M.m[12] = pos.x; M.m[13] = pos.y; M.m[14] = pos.z;
            shown[i] = 1;
        }

        // Lighting of the lit meshes (the car): the sun and ambient light, and the point lights that reach it, nearest first.
        {
            const Mover* car = nullptr;
            Vec3f carPos;
            for (size_t i = 0; i < scene->movers.size(); i++)
                if (shown[i] && scene->movers[i].lit) {
                    car = &scene->movers[i];
                    carPos = Vec3f(models[i].m[12], models[i].m[13], models[i].m[14]);
                    break;
                }
            if (car) {
                const Lighting& li = scene->lighting;
                std::vector<std::pair<float, int>> near;  // (distance beyond the light's reach, index): negative = in reach
                for (size_t k = 0; k < li.lights.size(); k++) {
                    float s = length(li.lights[k].pos - carPos) - li.lights[k].outer;
                    if (s < 0) near.push_back({s, (int)k});
                }
                const int n = (int)std::min<size_t>(near.size(), 8);
                std::partial_sort(near.begin(), near.begin() + n, near.end());
                float pos[8 * 4] = {}, col[8 * 4] = {};
                for (int k = 0; k < n; k++) {
                    const PointLight& pl = li.lights[near[k].second];
                    pos[k * 4] = pl.pos.x; pos[k * 4 + 1] = pl.pos.y; pos[k * 4 + 2] = pl.pos.z; pos[k * 4 + 3] = pl.outer;
                    col[k * 4] = pl.color.x; col[k * 4 + 1] = pl.color.y; col[k * 4 + 2] = pl.color.z; col[k * 4 + 3] = pl.inner;
                }
                glUseProgram(progLit);
                glUniformMatrix4fv(glGetUniformLocation(progLit, "uVP"), 1, GL_FALSE, vp.viewProj.m);
                glUniform1i(glGetUniformLocation(progLit, "uTex"), 0);
                glUniform3f(glGetUniformLocation(progLit, "uCam"), vp.eye.x, vp.eye.y, vp.eye.z);
                glUniform3f(glGetUniformLocation(progLit, "uAmbient"), li.ambient.x, li.ambient.y, li.ambient.z);
                glUniform3f(glGetUniformLocation(progLit, "uSunDir"), li.sunDir.x, li.sunDir.y, li.sunDir.z);
                glUniform3f(glGetUniformLocation(progLit, "uSunColor"), li.sunColor.x, li.sunColor.y, li.sunColor.z);
                glUniform1i(glGetUniformLocation(progLit, "uNumLights"), n);
                glUniform4fv(glGetUniformLocation(progLit, "uLightPos"), 8, pos);
                glUniform4fv(glGetUniformLocation(progLit, "uLightCol"), 8, col);
                glUseProgram(progWorld);
            }
        }

        auto drawSet = [&](const std::vector<GpuBatch>& set, int pass, const Mat4& model, bool lit = false) {
            const WorldLocs& L = lit ? litLocs : worldLocs;
            if (lit) {
                glUseProgram(progLit);
                glUniform1f(L.exposure, curExposure);
                glUniform1f(L.fade, curFade);
            }
            glUniformMatrix4fv(L.model, 1, GL_FALSE, model.m);
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
                glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, nullptr);
            }
            if (lit) glUseProgram(progWorld);
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
                    if (shown[i] && scene->movers[i].powerupMesh) {
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
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
            }
            // The minigame car's see-through parts (windows) go in before the level's, writing depth: the level's blended surfaces
            // (water) then blend over them where they are nearer, and are rejected where they are behind. Drawn after the
            // water instead, the windows would paint over it as if nothing was in between.
            if (pass == 1)
                for (size_t i = 0; i < scene->movers.size(); i++) {
                    if (!shown[i] || !scene->movers[i].external || scene->movers[i].alwaysOnTop) continue;
                    setExposure(opt.exposure);
                    glDepthMask(GL_TRUE);
                    drawSet(geomGpu[scene->movers[i].geom], pass, models[i], scene->movers[i].lit);
                    glDepthMask(GL_FALSE);
                }
            setExposure(opt.exposure);  // the movers below change it: reset for the level in every pass
            if (opt.wireframe) { if (pass == 0) drawWire(); }
            else drawSet(batches, pass, Mat4::identity());
            if (pass == 0 && opt.xrayEnabled() && opt.powerupMeshes) drawPowerupGhosts();
            for (size_t i = 0; i < scene->movers.size(); i++) {
                if (!shown[i] || scene->movers[i].alwaysOnTop) continue;  // those are drawn last, below
                if (pass == 1 && scene->movers[i].external) continue;  // drawn before the level's blended surfaces
                setExposure(scene->movers[i].billboard ? opt.haloExposure : opt.exposure);
                drawSet(geomGpu[scene->movers[i].geom], pass, models[i], scene->movers[i].lit);
            }
        }
        setExposure(opt.exposure);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        glDisable(GL_CULL_FACE);
        // debug markers: over everything, whatever is in front of them (no depth test, so they show through the car's hull)
        glDisable(GL_DEPTH_TEST);
        for (size_t i = 0; i < scene->movers.size(); i++)
            if (shown[i] && scene->movers[i].alwaysOnTop) drawSet(geomGpu[scene->movers[i].geom], 0, models[i], false);
        glEnable(GL_BLEND);  // then the translucent ones (the collision spheres)
        glDepthMask(GL_FALSE);
        for (size_t i = 0; i < scene->movers.size(); i++)
            if (shown[i] && scene->movers[i].alwaysOnTop) drawSet(geomGpu[scene->movers[i].geom], 1, models[i], false);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    // the game's collision mesh: a translucent overlay, nudged towards the camera so it wins against the surfaces it coincides with
    if (opt.showCollision && !collisionGpu.empty()) {
        glUseProgram(progWorld);
        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uVP"), 1, GL_FALSE, vp.viewProj.m);
        glUniformMatrix4fv(glGetUniformLocation(progWorld, "uModel"), 1, GL_FALSE, Mat4::identity().m);
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
