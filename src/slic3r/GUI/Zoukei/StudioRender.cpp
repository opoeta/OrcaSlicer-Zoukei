// Orca Zoukei: Photo Studio renderer, see StudioRender.hpp.
#include "StudioRender.hpp"

#include <glad/gl.h>

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace Slic3r {
namespace GUI {
namespace Zoukei {

namespace {

constexpr double kDeg = PI / 180.0;

// Texture units the studio borrows while it draws; put back to nothing afterwards.
constexpr int kUnitKey     = 4; // also what the toolpaths sample their self shadows from
constexpr int kUnitContact = 5;
constexpr int kUnitReflCol = 6;
constexpr int kUnitReflDep = 7;

// ---- matrices -------------------------------------------------------------------------------------

Matrix4d look_at(const Vec3d& eye, const Vec3d& target, const Vec3d& up)
{
    const Vec3d z = (eye - target).normalized();
    const Vec3d x = up.cross(z).normalized();
    const Vec3d y = z.cross(x).normalized();
    Matrix4d m = Matrix4d::Identity();
    m.block<1, 3>(0, 0) = x.transpose();
    m.block<1, 3>(1, 0) = y.transpose();
    m.block<1, 3>(2, 0) = z.transpose();
    m(0, 3) = -x.dot(eye);
    m(1, 3) = -y.dot(eye);
    m(2, 3) = -z.dot(eye);
    return m;
}

Matrix4d frustum(double l, double r, double b, double t, double n, double f)
{
    Matrix4d m = Matrix4d::Zero();
    m(0, 0) = 2.0 * n / (r - l);
    m(0, 2) = (r + l) / (r - l);
    m(1, 1) = 2.0 * n / (t - b);
    m(1, 2) = (t + b) / (t - b);
    m(2, 2) = -(f + n) / (f - n);
    m(2, 3) = -2.0 * f * n / (f - n);
    m(3, 2) = -1.0;
    return m;
}

Matrix4d ortho(double l, double r, double b, double t, double n, double f)
{
    Matrix4d m = Matrix4d::Identity();
    m(0, 0) = 2.0 / (r - l);
    m(0, 3) = -(r + l) / (r - l);
    m(1, 1) = 2.0 / (t - b);
    m(1, 3) = -(t + b) / (t - b);
    m(2, 2) = -2.0 / (f - n);
    m(2, 3) = -(f + n) / (f - n);
    return m;
}

libvgcode::Mat4x4 to_vgcode_mat(const Matrix4d& m)
{
    const Matrix4f f = m.cast<float>();
    libvgcode::Mat4x4 out;
    std::memcpy(out.data(), f.data(), sizeof(float) * 16); // both column major
    return out;
}

libvgcode::Vec3 to_vgcode_vec(const Vec3d& v)
{
    return { float(v.x()), float(v.y()), float(v.z()) };
}

void set_mat4(GLint loc, const Matrix4d& m)
{
    if (loc < 0)
        return;
    const Matrix4f f = m.cast<float>();
    glsafe(::glUniformMatrix4fv(loc, 1, GL_FALSE, f.data()));
}

// The view and projection of a camera for a window of its frame.
struct ViewProj
{
    Matrix4d view{ Matrix4d::Identity() };
    Matrix4d proj{ Matrix4d::Identity() };
    Vec3d    eye{ Vec3d::Zero() };
};

ViewProj make_view_proj(const StudioCamera& sc, double aspect, const FrameWindow& w, double radius)
{
    ViewProj vp;
    const Vec3d  dir   = (sc.eye() - sc.target).normalized();
    const double tan_v = std::tan(0.5 * std::clamp(sc.fov_deg, 5.0, 90.0) * kDeg);
    // An orthographic camera sits far enough back for nothing to be cut; its size follows the distance.
    const double eye_dist = sc.ortho ? std::max(sc.distance, 4.0 * radius) : sc.distance;
    vp.eye  = sc.target + dir * eye_dist;
    vp.view = look_at(vp.eye, sc.target, sc.up());
    double near_z, far_z, half_h;
    if (sc.ortho) {
        near_z = eye_dist - 3.5 * radius;
        far_z  = eye_dist + 3.5 * radius;
        half_h = sc.distance * tan_v;
    }
    else {
        near_z = std::max(0.01 * eye_dist, eye_dist - 3.5 * radius);
        far_z  = eye_dist + 3.5 * radius;
        half_h = near_z * tan_v;
    }
    const double half_w = half_h * aspect;
    vp.proj = sc.ortho ? ortho(w.x0 * half_w, w.x1 * half_w, w.y0 * half_h, w.y1 * half_h, near_z, far_z) :
                         frustum(w.x0 * half_w, w.x1 * half_w, w.y0 * half_h, w.y1 * half_h, near_z, far_z);
    return vp;
}

// Direction to a light, world space.
Vec3d light_dir(const StudioLight& l, const StudioCamera& sc, bool follow)
{
    Vec3d h(0.0, -1.0, 0.0); // horizontal, from the subject toward the viewer
    if (follow) {
        const Vec3d to_eye = sc.eye() - sc.target;
        Vec3d       hz(to_eye.x(), to_eye.y(), 0.0);
        if (hz.norm() < 1e-6 * std::max(1.0, to_eye.norm())) {
            const double y = sc.yaw_deg * kDeg;
            hz = Vec3d(std::sin(y), -std::cos(y), 0.0);
        }
        h = hz.normalized();
    }
    const Vec3d  r(-h.y(), h.x(), 0.0); // right of the picture
    const double az = double(l.azimuth) * kDeg;
    const double el = std::clamp(double(l.elevation), -10.0, 90.0) * kDeg;
    return (std::cos(el) * (std::cos(az) * h + std::sin(az) * r) + std::sin(el) * Vec3d::UnitZ()).normalized();
}

// Material presets by family. id: what the app config keeps (stable); family and name: shown, translated.
// roughness, f0, specular, sheen, metallic, transmission, emission, grain, grain_mm, speckle, tint
struct MaterialPreset
{
    StudioMaterialInfo     info;
    libvgcode::StudioShading p;
};

libvgcode::StudioShading mat(float roughness, float f0, float specular, float sheen, float metallic = 0.f, float transmission = 0.f,
                             float emission = 0.f, float grain = 0.f, float grain_mm = 0.2f, float speckle = 0.f,
                             libvgcode::Vec3 tint = { 1.f, 1.f, 1.f })
{
    libvgcode::StudioShading s;
    s.roughness    = roughness;
    s.f0           = f0;
    s.specular     = specular;
    s.sheen        = sheen;
    s.metallic     = metallic;
    s.transmission = transmission;
    s.emission     = emission;
    s.grain        = grain;
    s.grain_mm     = grain_mm;
    s.speckle      = speckle;
    s.tint         = tint;
    return s;
}

// The order is the one of the list in the studio; append only where it fits, ids never change.
const std::vector<MaterialPreset>& material_presets()
{
    static const std::vector<MaterialPreset> presets = {
        { { "preview", "Preview", "Preview shading" }, mat(0.50f, 0.04f, 1.00f, 0.0f) },
        { { "pla_matte", "PLA", "Matte" }, mat(0.72f, 0.04f, 0.55f, 0.0f) },
        { { "pla_glossy", "PLA", "Glossy" }, mat(0.34f, 0.045f, 1.10f, 0.0f) },
        { { "pla_silk", "PLA", "Silk" }, mat(0.30f, 0.06f, 0.90f, 1.0f) },
        { { "pla_silk_metal", "PLA", "Silk metal" }, mat(0.26f, 0.08f, 1.00f, 0.7f, 0.65f) },
        { { "pla_marble", "PLA", "Marble / granite" }, mat(0.62f, 0.04f, 0.60f, 0.0f, 0.f, 0.f, 0.f, 0.06f, 0.6f, 1.0f) },
        { { "pla_wood", "PLA", "Wood" }, mat(0.80f, 0.04f, 0.35f, 0.0f, 0.f, 0.f, 0.f, 0.18f, 0.45f, 0.f, { 1.05f, 0.95f, 0.84f }) },
        { { "petg_gloss", "PETG", "Glossy" }, mat(0.15f, 0.05f, 1.50f, 0.0f) },
        { { "petg_matte", "PETG", "Matte" }, mat(0.60f, 0.045f, 0.70f, 0.0f) },
        { { "petg_translucent", "PETG", "Translucent" }, mat(0.12f, 0.05f, 1.50f, 0.0f, 0.f, 0.6f) },
        { { "abs", "ABS / ASA", "ABS" }, mat(0.48f, 0.04f, 0.80f, 0.0f) },
        { { "asa", "ABS / ASA", "ASA" }, mat(0.68f, 0.04f, 0.55f, 0.0f) },
        { { "pc", "Engineering", "PC" }, mat(0.12f, 0.05f, 1.60f, 0.0f) },
        { { "pa", "Engineering", "PA (nylon)" }, mat(0.62f, 0.04f, 0.60f, 0.0f, 0.f, 0.1f) },
        { { "cf", "Engineering", "Carbon fibre (PLA-CF, PETG-CF, PA-CF)" },
          mat(0.86f, 0.035f, 0.22f, 0.0f, 0.f, 0.f, 0.f, 0.35f, 0.12f, 0.f, { 0.90f, 0.90f, 0.92f }) },
        { { "tpu", "Engineering", "TPU" }, mat(0.66f, 0.04f, 0.35f, 0.0f, 0.f, 0.f, 0.f, 0.f, 0.2f, 0.f, { 0.96f, 0.96f, 0.96f }) },
        { { "glow", "Specials", "Glow in the dark" }, mat(0.60f, 0.04f, 0.60f, 0.0f, 0.f, 0.15f, 0.35f) },
        { { "clear", "Specials", "Clear / resin" }, mat(0.06f, 0.05f, 1.80f, 0.0f, 0.f, 1.0f) },
    };
    return presets;
}

// ---- shaders --------------------------------------------------------------------------------------

const char* kFullscreenVS =
    "#version 150\n"
    "out vec2 ndc;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;\n"
    "  ndc = p;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

// The stage: the background, and where the view ray meets the floor (z = 0) the cyclorama floor, the key light's
// cast shadow (wide PCF over its depth map), the contact shadow (how close the lowest part of the print is above
// each floor point, from a map rendered from below) and the reflection (the mirrored print, faded with its height).
// Output premultiplied; the toolpaths are drawn over it.
const char* kStageFS =
    "#version 150\n"
    "in vec2 ndc;\n"
    "out vec4 out_color;\n"
    "uniform mat4 inv_view_proj;\n"
    "uniform int eye_above;\n"
    "uniform int bg_mode;\n"
    "uniform vec3 col_a;\n"
    "uniform vec3 col_b;\n"
    "uniform vec4 window;\n"
    "uniform vec3 center;\n"
    "uniform float radius;\n"
    "uniform int checker;\n"
    "uniform vec2 viewport;\n"
    "uniform int shadow_on;\n"
    "uniform sampler2D shadow_map;\n"
    "uniform mat4 shadow_vp;\n"
    "uniform float shadow_texel;\n"
    "uniform float shadow_strength;\n"
    "uniform int contact_on;\n"
    "uniform sampler2D contact_map;\n"
    "uniform mat4 contact_vp;\n"
    "uniform vec2 contact_depth;\n"
    "uniform float contact_radius;\n"
    "uniform float contact_height;\n"
    "uniform float contact_strength;\n"
    "uniform int refl_on;\n"
    "uniform sampler2D refl_color;\n"
    "uniform sampler2D refl_depth;\n"
    "uniform vec2 refl_scale;\n"
    "uniform mat4 refl_inv_view_proj;\n"
    "uniform float refl_strength;\n"
    "uniform float refl_fade;\n"
    // 16 tap Poisson disk, turned per pixel (interleaved gradient noise): grain the supersampling averages away
    "const vec2 PD[16] = vec2[16](vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),\n"
    "  vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845),\n"
    "  vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023),\n"
    "  vec2(0.79197514, 0.19090188), vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),\n"
    "  vec2(0.14383161, -0.14100790));\n"
    "mat2 disk_rotation() {\n"
    "  float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
    "  return mat2(cos(a), sin(a), -sin(a), cos(a));\n"
    "}\n"
    // Contact hardening soft shadow (PCSS): the farther the blocker above the floor, the wider the penumbra, as a
    // softbox of that angular size would draw it.
    "uniform float shadow_depth_mm;\n"
    "uniform float shadow_texel_mm;\n"
    "uniform float light_size;\n"
    "float key_shadow(vec3 p) {\n"
    "  vec4 lp = shadow_vp * vec4(p, 1.0);\n"
    "  vec3 q = lp.xyz / lp.w * 0.5 + 0.5;\n"
    "  if (q.z > 1.0)\n"
    "    return 0.0;\n"
    "  mat2 rot = disk_rotation();\n"
    "  float bias = 0.0008;\n"
    "  float search = clamp(light_size * 0.5 * shadow_depth_mm / shadow_texel_mm, 2.0, 96.0);\n"
    "  float blockers = 0.0;\n"
    "  float n_block = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    float d = texture(shadow_map, q.xy + rot * PD[i] * search * shadow_texel).r;\n"
    "    if (d < q.z - bias) {\n"
    "      blockers += d;\n"
    "      n_block += 1.0;\n"
    "    }\n"
    "  }\n"
    "  if (n_block < 0.5)\n"
    "    return 0.0;\n"
    "  float height_mm = (q.z - blockers / n_block) * shadow_depth_mm;\n"
    "  float radius = clamp(light_size * height_mm / shadow_texel_mm, 1.0, 96.0);\n"
    "  float sum = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    float d = texture(shadow_map, q.xy + rot * PD[i] * radius * shadow_texel).r;\n"
    "    sum += (q.z - bias > d) ? 1.0 : 0.0;\n"
    "  }\n"
    "  return sum / 16.0;\n"
    "}\n"
    "float contact(vec3 p) {\n"
    "  mat2 rot = disk_rotation();\n"
    "  float occ = 0.0;\n"
    "  float n = 0.0;\n"
    "  for (int i = 0; i < 16; ++i) {\n"
    "    vec2 o = rot * PD[i];\n"
    "    float r2 = dot(o, o);\n"
    "    vec4 lp = contact_vp * vec4(p.xy + o * contact_radius, 0.0, 1.0);\n"
    "    vec2 uv = lp.xy / lp.w * 0.5 + 0.5;\n"
    // nothing above this point (the cleared far plane, or outside the map): no contact at all, however flat the print
    "    float d = texture(contact_map, uv).r;\n"
    "    float h = d >= 0.99999 ? 1.0e6 : contact_depth.x + d * contact_depth.y;\n"
    "    float w = 1.0 - 0.6 * r2;\n"
    "    occ += w * (1.0 - smoothstep(0.0, contact_height, h));\n"
    "    n += w;\n"
    "  }\n"
    "  return occ / n;\n"
    "}\n"
    "vec4 reflection(vec2 frag) {\n"
    "  float d = texture(refl_depth, frag * refl_scale).r;\n"
    "  if (d >= 1.0)\n"
    "    return vec4(0.0);\n"
    "  vec4 wp = refl_inv_view_proj * vec4(frag / viewport * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "  float h = max(wp.z / wp.w, 0.0);\n"
    "  float fade = 1.0 - smoothstep(0.0, refl_fade, h);\n"
    "  float r = 1.0 + 5.0 * clamp(h / refl_fade, 0.0, 1.0);\n"
    "  vec4 acc = vec4(0.0);\n"
    "  for (int i = -1; i <= 1; ++i)\n"
    "    for (int j = -1; j <= 1; ++j)\n"
    "      acc += texture(refl_color, (frag + vec2(float(i), float(j)) * r) * refl_scale);\n"
    "  return acc / 9.0 * fade;\n"
    "}\n"
    "void main() {\n"
    "  vec4 pn = inv_view_proj * vec4(ndc, -1.0, 1.0);\n"
    "  vec4 pf = inv_view_proj * vec4(ndc, 1.0, 1.0);\n"
    "  vec3 o = pn.xyz / pn.w;\n"
    "  vec3 dir = normalize(pf.xyz / pf.w - o);\n"
    "  bool hit = eye_above != 0 && dir.z < -1e-5;\n"
    "  vec3 p = hit ? o + dir * (-o.z / dir.z) : vec3(0.0);\n"
    "  vec2 f = mix(window.xy, window.zw, ndc * 0.5 + 0.5);\n"
    "  vec3 bg = vec3(0.0);\n"
    "  float alpha = 1.0;\n"
    "  if (bg_mode == 0) {\n"
    "    if (hit) {\n"
    "      float k = smoothstep(radius * 0.8, radius * 8.0, length(p.xy - center.xy));\n"
    "      bg = mix(col_a, col_a * 0.80, k);\n"
    "    }\n"
    "    else\n"
    "      bg = mix(col_a * 0.80, col_a * 0.64, smoothstep(0.0, 0.7, dir.z));\n"
    "  }\n"
    "  else if (bg_mode == 1)\n"
    "    bg = mix(col_b, col_a, clamp(f.y * 0.5 + 0.5, 0.0, 1.0));\n"
    "  else if (bg_mode == 2)\n"
    "    bg = col_a;\n"
    "  else\n"
    "    alpha = 0.0;\n"
    "  float s = 0.0;\n"
    "  vec4 r = vec4(0.0);\n"
    "  if (hit) {\n"
    "    float ks = shadow_on != 0 ? key_shadow(p) * shadow_strength : 0.0;\n"
    "    float cs = contact_on != 0 ? contact(p) * contact_strength : 0.0;\n"
    "    s = 1.0 - (1.0 - ks) * (1.0 - cs);\n"
    "    if (refl_on != 0)\n"
    "      r = reflection(gl_FragCoord.xy) * refl_strength;\n"
    "  }\n"
    "  vec4 c;\n"
    "  if (alpha > 0.0)\n"
    "    c = vec4((bg * (1.0 - r.a) + r.rgb) * (1.0 - s), 1.0);\n"
    "  else {\n"
    "    c = vec4(r.rgb * (1.0 - s), r.a * (1.0 - s) + s);\n"
    "    if (checker != 0) {\n"
    "      vec2 cell = floor(gl_FragCoord.xy / 12.0);\n"
    "      float ch = mod(cell.x + cell.y, 2.0) < 1.0 ? 0.80 : 0.93;\n"
    "      c = vec4(c.rgb + vec3(ch) * (1.0 - c.a), 1.0);\n"
    "    }\n"
    "  }\n"
    // half a step of noise against the banding of 8 bit gradients (and of the GIF palette)
    "  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + vec2(17.0, 31.0), vec2(0.06711056, 0.00583715))));\n"
    "  c.rgb = max(c.rgb + vec3((noise - 0.5) / 255.0) * c.a, vec3(0.0));\n"
    "  out_color = c;\n"
    "}\n";

// Live view guides: shade outside the frame, its border, the rule of thirds.
const char* kGuidesFS =
    "#version 150\n"
    "in vec2 ndc;\n"
    "out vec4 out_color;\n"
    "uniform vec4 frame;\n"
    "uniform int thirds;\n"
    "uniform vec3 accent;\n"
    "uniform float shade;\n"
    "void main() {\n"
    "  vec2 p = gl_FragCoord.xy;\n"
    "  if (p.x < frame.x || p.x > frame.z || p.y < frame.y || p.y > frame.w) {\n"
    "    out_color = vec4(0.0, 0.0, 0.0, shade);\n"
    "    return;\n"
    "  }\n"
    "  float edge = min(min(p.x - frame.x, frame.z - p.x), min(p.y - frame.y, frame.w - p.y));\n"
    "  if (edge < 1.5) {\n"
    "    out_color = vec4(accent, 0.95);\n"
    "    return;\n"
    "  }\n"
    "  if (thirds != 0) {\n"
    "    vec2 sz = frame.zw - frame.xy;\n"
    "    vec2 rel = (p - frame.xy) / sz;\n"
    "    float lx = min(abs(rel.x - 1.0 / 3.0), abs(rel.x - 2.0 / 3.0)) * sz.x;\n"
    "    float ly = min(abs(rel.y - 1.0 / 3.0), abs(rel.y - 2.0 / 3.0)) * sz.y;\n"
    "    if (min(lx, ly) < 0.7) {\n"
    "      out_color = vec4(1.0, 1.0, 1.0, 0.35);\n"
    "      return;\n"
    "    }\n"
    "  }\n"
    "  discard;\n"
    "}\n";

unsigned int compile_program(const char* vs_src, const char* fs_src, std::string& error)
{
    auto compile = [&error](GLenum type, const char* src) -> GLuint {
        const GLuint id = ::glCreateShader(type);
        ::glShaderSource(id, 1, &src, nullptr);
        ::glCompileShader(id);
        GLint ok = 0;
        ::glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = { 0 };
            ::glGetShaderInfoLog(id, sizeof(log) - 1, nullptr, log);
            error = std::string("Photo Studio shader: ") + log;
            ::glDeleteShader(id);
            return 0;
        }
        return id;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    if (vs == 0)
        return 0;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (fs == 0) {
        ::glDeleteShader(vs);
        return 0;
    }
    const GLuint prog = ::glCreateProgram();
    ::glAttachShader(prog, vs);
    ::glAttachShader(prog, fs);
    ::glBindFragDataLocation(prog, 0, "out_color");
    ::glLinkProgram(prog);
    ::glDeleteShader(vs);
    ::glDeleteShader(fs);
    GLint ok = 0;
    ::glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = { 0 };
        ::glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        error = std::string("Photo Studio program: ") + log;
        ::glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLint uni(unsigned int prog, const char* name)
{
    return ::glGetUniformLocation(prog, name);
}

void bind_unit(int unit, unsigned int tex)
{
    glsafe(::glActiveTexture(GL_TEXTURE0 + unit));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
    glsafe(::glActiveTexture(GL_TEXTURE0));
}

// Box merged corner by corner: a single layer print has no Z extent, which BoundingBoxf3(min, max) refuses.
BoundingBoxf3 safe_box(const BoundingBoxf3& b)
{
    BoundingBoxf3 out;
    if ((b.min.array() <= b.max.array()).all()) {
        out.merge(b.min);
        out.merge(b.max + Vec3d(0.0, 0.0, 1e-3));
    }
    return out;
}

} // namespace

// ---- presets, colour, camera ----------------------------------------------------------------------

std::array<float, 3> kelvin_to_rgb(float kelvin)
{
    // Tanner Helland's fit of the black body colour, then linear and normalised to unit luminance.
    const double t = std::clamp(double(kelvin), 1500.0, 15000.0) / 100.0;
    double r, g, b;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    }
    else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
        b = 255.0;
    }
    std::array<float, 3> c;
    const double v[3] = { r, g, b };
    for (int i = 0; i < 3; ++i)
        c[i] = float(std::pow(std::clamp(v[i] / 255.0, 0.0, 1.0), 2.2));
    const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    if (lum > 1e-4f)
        for (float& x : c)
            x /= lum;
    return c;
}

void apply_light_preset(StudioSettings& s, StudioLightPreset preset)
{
    switch (preset) {
    case StudioLightPreset::Dramatic:
        s.key  = { true, 1.35f, -70.0f, 30.0f, 4800.0f };
        s.fill = { true, 0.12f, 70.0f, 10.0f, 6500.0f };
        s.rim  = { true, 1.10f, 170.0f, 25.0f, 9000.0f };
        s.ambient = 0.22f;
        break;
    case StudioLightPreset::HighKey:
        s.key  = { true, 0.90f, -30.0f, 55.0f, 6000.0f };
        s.fill = { true, 0.60f, 50.0f, 30.0f, 6500.0f };
        s.rim  = { true, 0.40f, 180.0f, 40.0f, 6500.0f };
        s.ambient = 0.70f;
        break;
    case StudioLightPreset::PreviewLike:
        s.key  = { true, 1.0f, -45.0f, 50.0f, 6500.0f };
        s.fill = { true, 0.375f, 75.0f, 40.0f, 6500.0f };
        s.rim  = { false, 0.5f, 180.0f, 30.0f, 6500.0f };
        s.ambient = 0.5f;
        break;
    default:
        s.key  = { true, 1.0f, -45.0f, 40.0f, 5600.0f };
        s.fill = { true, 0.35f, 60.0f, 15.0f, 6500.0f };
        s.rim  = { true, 0.70f, 165.0f, 35.0f, 8000.0f };
        s.ambient = 0.45f;
        break;
    }
}

size_t studio_material_count() { return material_presets().size(); }

const StudioMaterialInfo& studio_material(size_t index)
{
    return material_presets()[std::min(index, material_presets().size() - 1)].info;
}

int studio_material_index(const std::string& id)
{
    for (size_t i = 0; i < material_presets().size(); ++i)
        if (id == material_presets()[i].info.id)
            return int(i);
    return -1;
}

int studio_material_for_filament(const std::string& type_in, const std::string& name_in)
{
    std::string type = type_in, name = name_in;
    for (char& c : type) c = char(std::tolower((unsigned char) c));
    for (char& c : name) c = char(std::tolower((unsigned char) c));
    auto has = [&name](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (name.find(w) != std::string::npos)
                return true;
        return false;
    };
    auto is = [&type](std::initializer_list<const char*> types) {
        for (const char* t : types)
            if (type == t)
                return true;
        return false;
    };
    // the look named in the preset first: it says more than the base polymer
    if (has({ "glow", "luminous", "fosfor", "phosph" }))
        return studio_material_index("glow");
    const bool metal = has({ "metal" });
    if (has({ "silk", "seda" }))
        return studio_material_index(metal ? "pla_silk_metal" : "pla_silk");
    if (metal)
        return studio_material_index("pla_silk_metal");
    if (has({ "marble", "rmore", "granite", "granito", "stone", "pedra" }))
        return studio_material_index("pla_marble");
    if (has({ "wood", "madeira" }))
        return studio_material_index("pla_wood");
    if (type.find("cf") != std::string::npos || type.find("gf") != std::string::npos || has({ "carbon", "-cf", " cf" }))
        return studio_material_index("cf");
    if (has({ "translucent", "transparent", "transl", "transp", "clear", "cristal", "crystal" }))
        return studio_material_index(is({ "resin" }) ? "clear" : "petg_translucent");
    const bool matte = has({ "matte", "matt", "fosco" });
    if (is({ "tpu", "tpe", "tpu-ams" }) || type.find("tpu") != std::string::npos)
        return studio_material_index("tpu");
    if (is({ "pc", "pc-abs", "pcabs" }))
        return studio_material_index("pc");
    if (is({ "pa", "pa6", "pa12", "paht", "ppa", "nylon" }) || type.find("pa") == 0)
        return studio_material_index("pa");
    if (is({ "asa" }))
        return studio_material_index("asa");
    if (is({ "abs", "hips" }))
        return studio_material_index("abs");
    if (is({ "petg", "pet", "pctg", "hdpe", "pp" }))
        return studio_material_index(matte ? "petg_matte" : "petg_gloss");
    if (type.find("pla") != std::string::npos)
        return studio_material_index(matte ? "pla_matte" : "pla_glossy");
    return -1;
}

Vec3d StudioCamera::eye() const
{
    const double y = yaw_deg * kDeg;
    const double p = std::clamp(pitch_deg, -60.0, 90.0) * kDeg;
    return target + distance * Vec3d(std::cos(p) * std::sin(y), -std::cos(p) * std::cos(y), std::sin(p));
}

Vec3d StudioCamera::up() const
{
    // up = forward-to-eye x right, well defined straight from above as well
    const double y     = yaw_deg * kDeg;
    const Vec3d  right(std::cos(y), std::sin(y), 0.0);
    const Vec3d  d = (eye() - target).normalized();
    return d.cross(right).normalized();
}

void fit_camera(StudioCamera& camera, const BoundingBoxf3& box_in, double aspect, double margin)
{
    const BoundingBoxf3 box = safe_box(box_in);
    if (!box.defined)
        return;
    aspect = std::max(0.05, aspect);
    camera.target = box.center();
    const double tan_v = std::tan(0.5 * std::clamp(camera.fov_deg, 5.0, 90.0) * kDeg);
    const double tan_h = tan_v * aspect;
    for (int iter = 0; iter < 3; ++iter) {
        const Vec3d d     = (camera.eye() - camera.target).normalized();
        const Vec3d up    = camera.up();
        const Vec3d right = up.cross(d).normalized();
        // corners in the camera basis around the target: a right, b up, c toward the camera
        double need = 0.0;
        double ortho_h = 0.0;
        double min_x = 1e30, max_x = -1e30, min_y = 1e30, max_y = -1e30;
        for (int i = 0; i < 8; ++i) {
            const Vec3d corner((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(), (i & 4) ? box.max.z() : box.min.z());
            const Vec3d v = corner - camera.target;
            const double a = v.dot(right), b = v.dot(up), c = v.dot(d);
            need    = std::max(need, std::max(c + margin * std::abs(a) / tan_h, c + margin * std::abs(b) / tan_v));
            ortho_h = std::max(ortho_h, std::max(margin * std::abs(b), margin * std::abs(a) / aspect));
            // where it lands on the picture, in units of the half frame at the target's depth
            const double depth = camera.ortho ? 1.0 : std::max(1e-3, camera.distance - c) / camera.distance;
            min_x = std::min(min_x, a / depth);
            max_x = std::max(max_x, a / depth);
            min_y = std::min(min_y, b / depth);
            max_y = std::max(max_y, b / depth);
        }
        camera.distance = camera.ortho ? std::max(1.0, ortho_h / tan_v) : std::max(1.0, need);
        // centre what the picture shows
        if (iter < 2)
            camera.target += right * (0.5 * (min_x + max_x)) + up * (0.5 * (min_y + max_y));
    }
}

// ---- renderer -------------------------------------------------------------------------------------

StudioRenderer::~StudioRenderer()
{
    // The owner calls release() with the context current; nothing GL can be done here safely.
}

void StudioRenderer::release()
{
    auto del_tex = [](unsigned int& t) {
        if (t != 0)
            ::glDeleteTextures(1, &t);
        t = 0;
    };
    auto del_fbo = [](unsigned int& f) {
        if (f != 0)
            ::glDeleteFramebuffers(1, &f);
        f = 0;
    };
    del_tex(m_key_tex);
    del_fbo(m_key_fbo);
    del_tex(m_contact_tex);
    del_fbo(m_contact_fbo);
    del_tex(m_refl_color);
    del_tex(m_refl_depth);
    del_fbo(m_refl_fbo);
    m_key_size = m_contact_size = m_refl_w = m_refl_h = 0;
    m_key_cache = m_contact_cache = 0;
    if (m_stage_program != 0)
        ::glDeleteProgram(m_stage_program);
    if (m_guides_program != 0)
        ::glDeleteProgram(m_guides_program);
    m_stage_program = m_guides_program = 0;
    if (m_vao != 0)
        ::glDeleteVertexArrays(1, &m_vao);
    m_vao = 0;
    m_programs_failed = false;
}

GCodeViewer* StudioRenderer::gcode_viewer()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return nullptr;
    GLCanvas3D* canvas = plater->get_preview_canvas3D();
    return canvas != nullptr ? &canvas->get_gcode_viewer() : nullptr;
}

bool StudioRenderer::has_print()
{
    GCodeViewer* gv = gcode_viewer();
    return gv != nullptr && gv->has_data() && safe_box(gv->get_paths_bounding_box()).defined;
}

BoundingBoxf3 StudioRenderer::print_box()
{
    GCodeViewer* gv = gcode_viewer();
    return gv != nullptr ? safe_box(gv->get_paths_bounding_box()) : BoundingBoxf3();
}

size_t StudioRenderer::layers_count()
{
    GCodeViewer* gv = gcode_viewer();
    return gv != nullptr ? gv->get_layers_count() : 0;
}

bool StudioRenderer::init_programs()
{
    if (m_stage_program != 0 && m_guides_program != 0)
        return true;
    if (m_programs_failed)
        return false;
    m_stage_program = compile_program(kFullscreenVS, kStageFS, m_error);
    if (m_stage_program != 0)
        m_guides_program = compile_program(kFullscreenVS, kGuidesFS, m_error);
    if (m_stage_program == 0 || m_guides_program == 0) {
        m_programs_failed = true;
        BOOST_LOG_TRIVIAL(error) << "[studio] " << m_error;
        return false;
    }
    glsafe(::glGenVertexArrays(1, &m_vao));
    return true;
}

bool StudioRenderer::ensure_depth_target(unsigned int& tex, unsigned int& fbo, int size)
{
    if (tex == 0) {
        glsafe(::glGenTextures(1, &tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));
        const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glsafe(::glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    }
    if (fbo == 0) {
        glsafe(::glGenFramebuffers(1, &fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0));
        glsafe(::glDrawBuffer(GL_NONE));
        glsafe(::glReadBuffer(GL_NONE));
    }
    else
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    return ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool StudioRenderer::ensure_reflection_target(int w, int h)
{
    if (m_refl_fbo != 0 && m_refl_w >= w && m_refl_h >= h)
        return true;
    if (m_refl_color != 0)
        ::glDeleteTextures(1, &m_refl_color);
    if (m_refl_depth != 0)
        ::glDeleteTextures(1, &m_refl_depth);
    if (m_refl_fbo != 0)
        ::glDeleteFramebuffers(1, &m_refl_fbo);
    m_refl_color = m_refl_depth = m_refl_fbo = 0;
    m_refl_w = std::max(w, 16);
    m_refl_h = std::max(h, 16);

    glsafe(::glGenTextures(1, &m_refl_color));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_refl_color));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_refl_w, m_refl_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    glsafe(::glGenTextures(1, &m_refl_depth));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_refl_depth));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_refl_w, m_refl_h, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    glsafe(::glGenFramebuffers(1, &m_refl_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_refl_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_refl_color, 0));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_refl_depth, 0));
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, bufs));
    return ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Depth-only pass of the toolpaths into the bound depth target, as the light sees them.
static void render_casters(GCodeViewer& gv, int size, const Matrix4d& view, const Matrix4d& proj, const Vec3d& eye)
{
    glsafe(::glViewport(0, 0, size, size));
    glsafe(::glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(4.0f, 4.0f));
    glsafe(::glDisable(GL_CULL_FACE));
    gv.render_shadow_casters(Transform3d(view), Transform3d(proj), eye);
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
}

bool StudioRenderer::build_key_shadow(GCodeViewer& gv, const Vec3d& L, const BoundingBoxf3& box, double reach, int size)
{
    const Vec3d up     = std::abs(L.z()) > 0.99 ? Vec3d::UnitY() : Vec3d::UnitZ();
    const Vec3d z_axis = L;
    const Vec3d x_axis = up.cross(z_axis).normalized();
    const Vec3d y_axis = z_axis.cross(x_axis).normalized();
    const Vec3d ray    = -L;
    const Vec3d center = box.center();

    Vec3d lmin(DBL_MAX, DBL_MAX, DBL_MAX), lmax(-DBL_MAX, -DBL_MAX, -DBL_MAX);
    auto enclose = [&](const Vec3d& p) {
        const Vec3d lp(x_axis.dot(p), y_axis.dot(p), z_axis.dot(p));
        lmin = lmin.cwiseMin(lp);
        lmax = lmax.cwiseMax(lp);
    };
    for (int i = 0; i < 8; ++i) {
        const Vec3d corner((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(), (i & 4) ? box.max.z() : box.min.z());
        enclose(corner);
        if (ray.z() < -1e-6) {
            // where its shadow lands, kept within reach of the print so a grazing light stays bounded
            Vec3d s = corner + (-corner.z() / ray.z()) * ray;
            Vec2d off(s.x() - center.x(), s.y() - center.y());
            if (off.norm() > reach)
                off *= reach / off.norm();
            enclose(Vec3d(center.x() + off.x(), center.y() + off.y(), 0.0));
        }
    }
    const double range  = lmax.z() - lmin.z();
    const double margin = std::max(1.0, 0.05 * range);
    const double cx = 0.5 * (lmin.x() + lmax.x()), cy = 0.5 * (lmin.y() + lmax.y());
    const Vec3d  eye = x_axis * cx + y_axis * cy + z_axis * (lmax.z() + margin);

    Matrix4d view = Matrix4d::Identity();
    view.block<1, 3>(0, 0) = x_axis.transpose();
    view.block<1, 3>(1, 0) = y_axis.transpose();
    view.block<1, 3>(2, 0) = z_axis.transpose();
    view(0, 3) = -x_axis.dot(eye);
    view(1, 3) = -y_axis.dot(eye);
    view(2, 3) = -z_axis.dot(eye);
    const double halfx = std::max(0.5 * (lmax.x() - lmin.x()), 1.0) * 1.03;
    const double halfy = std::max(0.5 * (lmax.y() - lmin.y()), 1.0) * 1.03;
    const Matrix4d proj = ortho(-halfx, halfx, -halfy, halfy, margin * 0.5, range + margin * 1.5);
    m_key_vp       = Transform3d(proj * view);
    m_key_texel_mm = 2.0 * std::max(halfx, halfy) / double(size);
    m_key_depth_mm = (range + margin * 1.5) - margin * 0.5;

    if (m_key_size != size && m_key_tex != 0) {
        ::glDeleteTextures(1, &m_key_tex);
        m_key_tex = 0;
        if (m_key_fbo != 0)
            ::glDeleteFramebuffers(1, &m_key_fbo);
        m_key_fbo = 0;
    }
    m_key_size = size;
    if (!ensure_depth_target(m_key_tex, m_key_fbo, size)) {
        BOOST_LOG_TRIVIAL(error) << "[studio] key shadow framebuffer incomplete, size " << size;
        return false;
    }
    render_casters(gv, size, view, proj, eye);
    return true;
}

bool StudioRenderer::build_contact_map(GCodeViewer& gv, const BoundingBoxf3& box, double reach, int size)
{
    // Looking straight up from just under the floor: the depth is how high the lowest part of the print is.
    const Vec3d  c    = box.center();
    const double half = 0.5 * std::max(box.size().x(), box.size().y()) + reach;
    const Vec3d  eye(c.x(), c.y(), -1.0);
    const Matrix4d view = look_at(eye, eye + Vec3d::UnitZ(), Vec3d::UnitY());
    const double near_z = 0.5, far_z = std::max(2.0, box.max.z() + 2.0);
    const Matrix4d proj = ortho(-half, half, -half, half, near_z, far_z);
    m_contact_vp    = Transform3d(proj * view);
    m_contact_z0    = eye.z() + near_z;
    m_contact_range = far_z - near_z;

    if (m_contact_size != size && m_contact_tex != 0) {
        ::glDeleteTextures(1, &m_contact_tex);
        m_contact_tex = 0;
        if (m_contact_fbo != 0)
            ::glDeleteFramebuffers(1, &m_contact_fbo);
        m_contact_fbo = 0;
    }
    m_contact_size = size;
    if (!ensure_depth_target(m_contact_tex, m_contact_fbo, size)) {
        BOOST_LOG_TRIVIAL(error) << "[studio] contact map framebuffer incomplete, size " << size;
        return false;
    }
    render_casters(gv, size, view, proj, eye);
    return true;
}

bool StudioRenderer::render(const StudioSettings& st, const StudioCamera& sc, double aspect, const FrameWindow& window, int width,
                            int height, bool checker, int layer_top)
{
    if (width <= 0 || height <= 0)
        return false;
    m_error.clear();
    if (!init_programs())
        return false;

    GLint target_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target_fbo));
    GLint prev_viewport[4] = { 0, 0, 0, 0 };
    glsafe(::glGetIntegerv(GL_VIEWPORT, prev_viewport));
    GLfloat prev_clear[4] = { 0.f, 0.f, 0.f, 0.f };
    glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear));
    const GLboolean prev_blend = ::glIsEnabled(GL_BLEND);
    const GLboolean prev_depth = ::glIsEnabled(GL_DEPTH_TEST);
    const GLboolean prev_cull  = ::glIsEnabled(GL_CULL_FACE);
    // The context is shared with the scene canvases, which can leave a scissor rectangle, the stencil test or a
    // masked colour behind (see UVEditorCanvas::render): any of them would clip the clear and every draw here.
    const GLboolean prev_scissor = ::glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prev_stencil = ::glIsEnabled(GL_STENCIL_TEST);
    GLboolean prev_mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glsafe(::glGetBooleanv(GL_COLOR_WRITEMASK, prev_mask));
    glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glDisable(GL_STENCIL_TEST));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
    glsafe(::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    glsafe(::glClearDepth(1.0));

    auto restore_gl = [&]() {
        if (prev_scissor) glsafe(::glEnable(GL_SCISSOR_TEST));
        if (prev_stencil) glsafe(::glEnable(GL_STENCIL_TEST));
        glsafe(::glColorMask(prev_mask[0], prev_mask[1], prev_mask[2], prev_mask[3]));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) target_fbo));
        glsafe(::glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]));
        glsafe(::glClearColor(prev_clear[0], prev_clear[1], prev_clear[2], prev_clear[3]));
        if (prev_blend) glsafe(::glEnable(GL_BLEND)); else glsafe(::glDisable(GL_BLEND));
        if (prev_depth) glsafe(::glEnable(GL_DEPTH_TEST)); else glsafe(::glDisable(GL_DEPTH_TEST));
        if (prev_cull) glsafe(::glEnable(GL_CULL_FACE)); else glsafe(::glDisable(GL_CULL_FACE));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glUseProgram(0));
        for (int unit : { kUnitKey, kUnitContact, kUnitReflCol, kUnitReflDep })
            bind_unit(unit, 0);
    };

    GCodeViewer* gv = gcode_viewer();
    const BoundingBoxf3 box = print_box();
    if (gv == nullptr || !gv->has_data() || !box.defined) {
        // nothing sliced: just the background
        glsafe(::glViewport(0, 0, width, height));
        const bool dark = st.background == StudioBackground::CycloramaDark;
        glsafe(::glClearColor(dark ? 0.12f : 0.85f, dark ? 0.12f : 0.85f, dark ? 0.13f : 0.86f, 1.0f));
        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
        restore_gl();
        m_error = "no print";
        if (m_log_key != "no print") {
            m_log_key = "no print";
            BOOST_LOG_TRIVIAL(info) << "[studio] nothing to render: viewer " << (gv != nullptr ? "present" : "missing") << ", data "
                                    << (gv != nullptr && gv->has_data() ? "yes" : "no") << ", box " << (box.defined ? "defined" : "empty");
        }
        return false;
    }

    const double radius    = std::max(1.0, 0.5 * box.size().norm());
    const double radius_xy = std::max(1.0, 0.5 * Vec2d(box.size().x(), box.size().y()).norm());
    const ViewProj vp      = make_view_proj(sc, aspect, window, radius);
    const Matrix4d mirror  = Vec4d(1.0, 1.0, -1.0, 1.0).asDiagonal();

    // ---- the viewer state the studio changes, put back when this returns ----
    struct ViewerState
    {
        GCodeViewer&          gv;
        bool                  dim;
        std::array<size_t, 2> range;
        libvgcode::Interval   visible;
        bool                  range_changed{ false };
        ~ViewerState()
        {
            gv.set_studio_shading(libvgcode::StudioShading());
            gv.set_shadow_map(kUnitKey, Transform3d::Identity(), 0.0f, 0.0f);
            gv.set_tone(1.0f, 1.15f);
            gv.set_light_top_dir(Vec3d(-0.4574957, 0.4574957, 0.7624929));
            gv.set_dim_previous_layers(dim);
            if (range_changed) {
                // the layer range, then the moves the preview's horizontal slider was showing
                gv.set_layers_z_range({ (unsigned int) range[0], (unsigned int) range[1] });
                gv.update_sequential_view_current((unsigned int) visible[0], (unsigned int) visible[1]);
                gv.update_moves_slider(false);
            }
        }
    } state{ *gv, gv->is_dim_previous_layers(), gv->get_layers_z_range(), gv->get_gcode_view_visible_range() };

    gv->set_clipping_plane(ClippingPlane::ClipsNothing());
    gv->set_dim_previous_layers(false);
    const size_t layers = gv->get_layers_count();
    if (layer_top >= 0 && layers > 0) {
        const unsigned int top = (unsigned int) std::min<size_t>((size_t) layer_top, layers - 1);
        if (state.range[0] != 0 || state.range[1] != top) {
            gv->set_layers_z_range({ 0u, top });
            state.range_changed = true;
        }
    }
    else if (st.whole_print && layers > 0 && (state.range[0] != 0 || state.range[1] + 1 < layers)) {
        gv->set_layers_z_range({ 0u, (unsigned int) (layers - 1) });
        state.range_changed = true;
    }

    // ---- lights ----
    const Vec3d key_w  = light_dir(st.key, sc, st.lights_follow_camera);
    const Vec3d fill_w = light_dir(st.fill, sc, st.lights_follow_camera);
    const Vec3d rim_w  = light_dir(st.rim, sc, st.lights_follow_camera);
    auto radiance = [](const StudioLight& l) -> libvgcode::Vec3 {
        if (!l.on)
            return { 0.0f, 0.0f, 0.0f };
        const std::array<float, 3> t = kelvin_to_rgb(l.kelvin);
        const float k = 9.6f * std::max(0.0f, l.intensity);
        return { t[0] * k, t[1] * k, t[2] * k };
    };
    const libvgcode::StudioShading& mat = material_presets()[size_t(std::clamp(st.material, 0, int(material_presets().size()) - 1))].p;
    auto shading_for = [&](const Matrix4d& view) {
        const Matrix3d rot = view.block<3, 3>(0, 0);
        libvgcode::StudioShading sh;
        sh.enabled    = true;
        sh.key_color  = radiance(st.key);
        sh.fill_dir   = to_vgcode_vec(Vec3d(rot * fill_w).normalized());
        sh.fill_color = radiance(st.fill);
        sh.rim_dir    = to_vgcode_vec(Vec3d(rot * rim_w).normalized());
        sh.rim_color  = radiance(st.rim);
        const float amb = std::clamp(st.ambient, 0.0f, 2.0f);
        sh.ambient   = { amb, amb, amb };
        sh.roughness    = mat.roughness;
        sh.f0           = mat.f0;
        sh.specular     = mat.specular;
        sh.sheen        = mat.sheen;
        sh.metallic     = mat.metallic;
        sh.transmission = mat.transmission;
        sh.emission     = mat.emission;
        sh.grain        = mat.grain;
        sh.grain_mm     = mat.grain_mm;
        sh.speckle      = mat.speckle;
        sh.tint         = mat.tint;
        gv->set_studio_shading(sh);
        gv->set_light_top_dir((rot * key_w).normalized());
    };

    // ---- shadow maps (kept while nothing they depend on changes) ----
    const bool key_casts = st.key.on && st.key.intensity > 0.0f && (st.floor_shadow || st.self_shadows);
    bool key_ok = false;
    if (key_casts) {
        size_t cache = gv->shadow_casters_signature();
        boost::hash_combine(cache, m_shadow_res);
        for (int i = 0; i < 3; ++i) {
            boost::hash_combine(cache, key_w[i]);
            boost::hash_combine(cache, box.min[i]);
            boost::hash_combine(cache, box.max[i]);
        }
        key_ok = cache == m_key_cache && m_key_tex != 0;
        if (!key_ok) {
            key_ok      = build_key_shadow(*gv, key_w, box, 4.0 * radius_xy, m_shadow_res);
            m_key_cache = key_ok ? cache : 0;
        }
    }
    const double contact_radius = std::clamp(0.06 * radius_xy, 1.5, 12.0);
    bool contact_ok = false;
    if (st.contact_shadow) {
        size_t cache = gv->shadow_casters_signature();
        for (int i = 0; i < 3; ++i) {
            boost::hash_combine(cache, box.min[i]);
            boost::hash_combine(cache, box.max[i]);
        }
        contact_ok = cache == m_contact_cache && m_contact_tex != 0;
        if (!contact_ok) {
            contact_ok      = build_contact_map(*gv, box, 2.0 * contact_radius, 1024);
            m_contact_cache = contact_ok ? cache : 0;
        }
    }
    const float self_shadow = (key_ok && st.self_shadows) ? 0.45f * std::min(1.0f, st.key.intensity) : 0.0f;
    gv->set_tone(std::clamp(st.exposure, 0.2f, 3.0f), std::clamp(st.saturation, 0.0f, 2.0f));

    auto set_self_shadows = [&]() {
        if (self_shadow > 0.0f) {
            bind_unit(kUnitKey, m_key_tex);
            gv->set_shadow_map(kUnitKey, m_key_vp, self_shadow, 1.0f / float(std::max(1, m_key_size)));
        }
        else
            gv->set_shadow_map(kUnitKey, Transform3d::Identity(), 0.0f, 0.0f);
    };

    // ---- reflection: the print mirrored under the floor, same camera ----
    const bool eye_above = vp.eye.z() > 0.0;
    bool refl_ok = false;
    if (st.reflection && st.reflection_strength > 0.0f && eye_above && ensure_reflection_target(width, height)) {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_refl_fbo));
        glsafe(::glViewport(0, 0, width, height));
        glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glDepthFunc(GL_LESS));
        glsafe(::glDisable(GL_BLEND));
        const Matrix4d mview = vp.view * mirror;
        shading_for(mview);
        set_self_shadows();
        gv->render_toolpaths_with_matrices(to_vgcode_mat(mview), to_vgcode_mat(vp.proj));
        refl_ok = true;
    }

    // ---- stage ----
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) target_fbo));
    glsafe(::glViewport(0, 0, width, height));
    glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDisable(GL_CULL_FACE));

    const GLuint p = m_stage_program;
    glsafe(::glUseProgram(p));
    set_mat4(uni(p, "inv_view_proj"), (vp.proj * vp.view).inverse());
    glsafe(::glUniform1i(uni(p, "eye_above"), eye_above ? 1 : 0));
    int mode = 0;
    const float* col_a = st.cyclorama_color.data();
    const float* col_b = st.gradient_bottom.data();
    // the dark cyclorama is the same paint, in a dim room
    const float dark_cyc[3] = { st.cyclorama_color[0] * 0.21f, st.cyclorama_color[1] * 0.21f, st.cyclorama_color[2] * 0.22f };
    switch (st.background) {
    case StudioBackground::CycloramaWhite: mode = 0; break;
    case StudioBackground::CycloramaDark: mode = 0; col_a = dark_cyc; break;
    case StudioBackground::Gradient: mode = 1; col_a = st.gradient_top.data(); break;
    case StudioBackground::Solid: mode = 2; col_a = st.solid_color.data(); break;
    default: mode = 3; break;
    }
    glsafe(::glUniform1i(uni(p, "bg_mode"), mode));
    glsafe(::glUniform3fv(uni(p, "col_a"), 1, col_a));
    glsafe(::glUniform3fv(uni(p, "col_b"), 1, col_b));
    glsafe(::glUniform4f(uni(p, "window"), float(window.x0), float(window.y0), float(window.x1), float(window.y1)));
    const Vec3d c = box.center();
    glsafe(::glUniform3f(uni(p, "center"), float(c.x()), float(c.y()), 0.0f));
    glsafe(::glUniform1f(uni(p, "radius"), float(radius_xy)));
    glsafe(::glUniform1i(uni(p, "checker"), checker ? 1 : 0));
    glsafe(::glUniform2f(uni(p, "viewport"), float(width), float(height)));

    const bool floor_key = key_ok && st.floor_shadow;
    glsafe(::glUniform1i(uni(p, "shadow_on"), floor_key ? 1 : 0));
    glsafe(::glUniform1i(uni(p, "shadow_map"), kUnitKey));
    glsafe(::glUniform1i(uni(p, "contact_map"), kUnitContact));
    glsafe(::glUniform1i(uni(p, "refl_color"), kUnitReflCol));
    glsafe(::glUniform1i(uni(p, "refl_depth"), kUnitReflDep));
    if (floor_key) {
        bind_unit(kUnitKey, m_key_tex);
        set_mat4(uni(p, "shadow_vp"), m_key_vp.matrix());
        glsafe(::glUniform1f(uni(p, "shadow_texel"), 1.0f / float(std::max(1, m_key_size))));
        glsafe(::glUniform1f(uni(p, "shadow_texel_mm"), float(std::max(1e-4, m_key_texel_mm))));
        glsafe(::glUniform1f(uni(p, "shadow_depth_mm"), float(m_key_depth_mm)));
        // angular radius of the key light: a bare bulb at 0, a large softbox at 1
        const float soft = std::clamp(st.shadow_softness, 0.0f, 1.0f);
        glsafe(::glUniform1f(uni(p, "light_size"), float(std::tan((0.4 + 16.0 * soft) * kDeg))));
        glsafe(::glUniform1f(uni(p, "shadow_strength"), std::clamp(st.shadow_strength, 0.0f, 1.0f) * std::min(1.0f, st.key.intensity)));
    }
    glsafe(::glUniform1i(uni(p, "contact_on"), contact_ok ? 1 : 0));
    if (contact_ok) {
        bind_unit(kUnitContact, m_contact_tex);
        set_mat4(uni(p, "contact_vp"), m_contact_vp.matrix());
        glsafe(::glUniform2f(uni(p, "contact_depth"), float(m_contact_z0), float(m_contact_range)));
        glsafe(::glUniform1f(uni(p, "contact_radius"), float(contact_radius)));
        glsafe(::glUniform1f(uni(p, "contact_height"), float(2.5 * contact_radius)));
        glsafe(::glUniform1f(uni(p, "contact_strength"), std::clamp(st.contact_strength, 0.0f, 1.0f)));
    }
    glsafe(::glUniform1i(uni(p, "refl_on"), refl_ok ? 1 : 0));
    if (refl_ok) {
        bind_unit(kUnitReflCol, m_refl_color);
        bind_unit(kUnitReflDep, m_refl_depth);
        glsafe(::glUniform2f(uni(p, "refl_scale"), 1.0f / float(m_refl_w), 1.0f / float(m_refl_h)));
        set_mat4(uni(p, "refl_inv_view_proj"), (vp.proj * vp.view * mirror).inverse());
        glsafe(::glUniform1f(uni(p, "refl_strength"), std::clamp(st.reflection_strength, 0.0f, 1.0f)));
        glsafe(::glUniform1f(uni(p, "refl_fade"), float(std::max(2.0, 0.45 * box.size().z()))));
    }
    glsafe(::glBindVertexArray(m_vao));
    glsafe(::glDrawArrays(GL_TRIANGLES, 0, 3));
    glsafe(::glBindVertexArray(0));
    glsafe(::glUseProgram(0));
    bind_unit(kUnitContact, 0);
    bind_unit(kUnitReflCol, 0);
    bind_unit(kUnitReflDep, 0);

    // ---- the print ----
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDepthFunc(GL_LESS));
    shading_for(vp.view);
    set_self_shadows();
    gv->render_toolpaths_with_matrices(to_vgcode_mat(vp.view), to_vgcode_mat(vp.proj));

    {
        // one line per change of what is drawn, for the debug log
        GLint bound = 0;
        ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        const GLenum status = ::glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum gl_err = ::glGetError();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[studio] render %dx%d fbo=%d status=0x%04X gl_error=0x%04X box=(%.1f %.1f %.1f)-(%.1f %.1f %.1f) eye=(%.1f %.1f %.1f) "
                      "layers=%zu range=%zu-%zu key=%d contact=%d refl=%d bg=%d",
                      width, height, (int) bound, (unsigned) status, (unsigned) gl_err, box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                      box.max.y(), box.max.z(), vp.eye.x(), vp.eye.y(), vp.eye.z(), layers, gv->get_layers_z_range()[0],
                      gv->get_layers_z_range()[1], key_ok ? 1 : 0, contact_ok ? 1 : 0, refl_ok ? 1 : 0, int(st.background));
        // the camera moves all the time: keep it out of the key
        const std::string key = std::to_string(width) + "x" + std::to_string(height) + "/" + std::to_string(bound) + "/" +
                                std::to_string(status) + "/" + std::to_string(gl_err) + "/" + std::to_string(layers) + "/" +
                                std::to_string(key_ok) + std::to_string(contact_ok) + std::to_string(refl_ok) + "/" +
                                std::to_string(int(st.background));
        if (key != m_log_key) {
            m_log_key = key;
            BOOST_LOG_TRIVIAL(info) << line;
        }
    }
    restore_gl();
    return true;
}

void StudioRenderer::render_guides(int width, int height, const std::array<float, 4>& frame_px, bool thirds, bool dark_theme)
{
    if (!init_programs() || width <= 0 || height <= 0)
        return;
    glsafe(::glViewport(0, 0, width, height));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    const GLuint p = m_guides_program;
    glsafe(::glUseProgram(p));
    glsafe(::glUniform4f(uni(p, "frame"), frame_px[0], frame_px[1], frame_px[2], frame_px[3]));
    glsafe(::glUniform1i(uni(p, "thirds"), thirds ? 1 : 0));
    // Orca teal
    glsafe(::glUniform3f(uni(p, "accent"), 0.0f, dark_theme ? 0.651f : 0.588f, dark_theme ? 0.576f : 0.533f));
    glsafe(::glUniform1f(uni(p, "shade"), dark_theme ? 0.55f : 0.42f));
    glsafe(::glBindVertexArray(m_vao));
    glsafe(::glDrawArrays(GL_TRIANGLES, 0, 3));
    glsafe(::glBindVertexArray(0));
    glsafe(::glUseProgram(0));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

bool StudioRenderer::render_live_pixels(const StudioSettings& st, const StudioCamera& sc, double aspect, const FrameWindow& window, int width,
                                        int height, const std::array<float, 4>& frame_px, bool thirds, bool dark_theme, std::vector<uint8_t>& rgba)
{
    if (width <= 0 || height <= 0)
        return false;
    GLint prev_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
    GLuint fbo = 0, color_rb = 0, depth_rb = 0;
    glsafe(::glGenFramebuffers(1, &fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    glsafe(::glGenRenderbuffers(1, &color_rb));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, color_rb));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb));
    glsafe(::glGenRenderbuffers(1, &depth_rb));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth_rb));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb));
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, bufs));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));
    bool ok = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        const bool drawn = render(st, sc, aspect, window, width, height, true);
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        if (drawn || has_print())
            render_guides(width, height, frame_px, thirds, dark_theme);
        rgba.assign(size_t(width) * size_t(height) * 4, 0);
        glsafe(::glReadBuffer(GL_COLOR_ATTACHMENT0));
        glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 1));
        glsafe(::glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, (void*) rgba.data()));
    }
    else
        BOOST_LOG_TRIVIAL(error) << "[studio] live fallback framebuffer incomplete " << width << "x" << height;
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo));
    glsafe(::glDeleteRenderbuffers(1, &depth_rb));
    glsafe(::glDeleteRenderbuffers(1, &color_rb));
    glsafe(::glDeleteFramebuffers(1, &fbo));
    return ok;
}

bool StudioRenderer::render_image(const StudioSettings& st, const StudioCamera& sc, int out_w, int out_h, int ss,
                                  std::vector<uint8_t>& rgba, int layer_top, const std::function<bool(int, int)>& progress)
{
    m_error.clear();
    if (out_w <= 0 || out_h <= 0)
        return false;
    ss = std::clamp(ss, 1, 4);

    GLint max_tex = 0, max_rb = 0, max_vp[2] = { 0, 0 };
    glsafe(::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex));
    glsafe(::glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb));
    glsafe(::glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_vp));
    int limit = 4096;
    for (int v : { (int) max_tex, (int) max_rb, (int) max_vp[0], (int) max_vp[1] })
        if (v > 0)
            limit = std::min(limit, v);
    limit = std::max(limit, 256);
    const int tile_ss  = (limit / ss) * ss;
    const int tile_out = tile_ss / ss;
    const int nx = (out_w + tile_out - 1) / tile_out;
    const int ny = (out_h + tile_out - 1) / tile_out;
    const int fbo_w = std::min(tile_ss, out_w * ss);
    const int fbo_h = std::min(tile_ss, out_h * ss);

    GLint prev_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
    GLuint fbo = 0, color_rb = 0, depth_rb = 0;
    glsafe(::glGenFramebuffers(1, &fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    glsafe(::glGenRenderbuffers(1, &color_rb));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, color_rb));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, fbo_w, fbo_h));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb));
    glsafe(::glGenRenderbuffers(1, &depth_rb));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, depth_rb));
    glsafe(::glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fbo_w, fbo_h));
    glsafe(::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb));
    const GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glsafe(::glDrawBuffers(1, bufs));
    glsafe(::glBindRenderbuffer(GL_RENDERBUFFER, 0));

    bool ok = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok)
        m_error = "The off-screen buffer could not be created.";
    if (ok) {
        rgba.assign((size_t) out_w * (size_t) out_h * 4, 0);
        std::vector<uint8_t> px((size_t) fbo_w * (size_t) fbo_h * 4);
        const double aspect   = double(out_w) / double(out_h);
        const float  inv_n    = 1.0f / float(ss * ss);
        const int    total    = nx * ny;
        int          done     = 0;
        for (int ty = 0; ty < ny && ok; ++ty) {
            for (int tx = 0; tx < nx && ok; ++tx) {
                const int ox0 = tx * tile_out, ox1 = std::min(out_w, ox0 + tile_out);
                const int oy0 = ty * tile_out, oy1 = std::min(out_h, oy0 + tile_out); // bottom-up
                const int w = (ox1 - ox0) * ss, h = (oy1 - oy0) * ss;
                FrameWindow win;
                win.x0 = -1.0 + 2.0 * double(ox0) / double(out_w);
                win.x1 = -1.0 + 2.0 * double(ox1) / double(out_w);
                win.y0 = -1.0 + 2.0 * double(oy0) / double(out_h);
                win.y1 = -1.0 + 2.0 * double(oy1) / double(out_h);
                glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
                if (!render(st, sc, aspect, win, w, h, false, layer_top)) {
                    if (m_error.empty())
                        m_error = "Nothing to render.";
                    ok = false;
                    break;
                }
                glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, fbo));
                glsafe(::glPixelStorei(GL_PACK_ALIGNMENT, 1));
                glsafe(::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*) px.data()));
                // Average the ss x ss samples of each output pixel (premultiplied), then straight alpha.
                for (int y = 0; y < oy1 - oy0; ++y) {
                    uint8_t* row = &rgba[((size_t) (out_h - 1 - (oy0 + y)) * (size_t) out_w + (size_t) ox0) * 4];
                    for (int x = 0; x < ox1 - ox0; ++x) {
                        float sum[4] = { 0.f, 0.f, 0.f, 0.f };
                        for (int sy = 0; sy < ss; ++sy) {
                            const uint8_t* s = &px[((size_t) (y * ss + sy) * (size_t) w + (size_t) (x * ss)) * 4];
                            for (int sx = 0; sx < ss; ++sx, s += 4)
                                for (int k = 0; k < 4; ++k)
                                    sum[k] += float(s[k]);
                        }
                        const float a = sum[3] * inv_n;
                        uint8_t*    o = row + (size_t) x * 4;
                        for (int k = 0; k < 3; ++k) {
                            const float v = sum[3] > 0.5f ? sum[k] * 255.0f / sum[3] : 0.0f;
                            o[k] = (uint8_t) std::clamp(v + 0.5f, 0.0f, 255.0f);
                        }
                        o[3] = (uint8_t) std::clamp(a + 0.5f, 0.0f, 255.0f);
                    }
                }
                ++done;
                // the progress dialog lets the main window paint: it must find its own framebuffer bound
                glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo));
                if (progress && !progress(done, total)) {
                    m_error = "cancelled";
                    ok = false;
                }
            }
        }
    }

    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prev_fbo));
    if (depth_rb != 0) glsafe(::glDeleteRenderbuffers(1, &depth_rb));
    if (color_rb != 0) glsafe(::glDeleteRenderbuffers(1, &color_rb));
    if (fbo != 0) glsafe(::glDeleteFramebuffers(1, &fbo));
    return ok;
}

} // namespace Zoukei
} // namespace GUI
} // namespace Slic3r
