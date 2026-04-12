/**
 * MMDRenderer.cpp — OpenGL ES 3.0 renderer for PMX/MMD models (Saba library).
 *
 * Interaction modes
 * ─────────────────
 *   None     finger just landed, waiting to classify the gesture
 *   Dragging finger moved > DRAG_THRESHOLD_PX before 1 s elapsed →
 *            the model follows the finger in screen-space (NDC offset)
 *   Rotating finger held still for ≥ 1 s → subsequent drag rotates the
 *            model around its own X/Y axes without moving it on screen
 *
 * Drag position
 * ─────────────
 *   The model's screen position is the sum of:
 *     m_posX/Y — set by Java via setTransform() (anchor / initial pos)
 *     m_nativeDragX/Y — accumulated NDC offset from native drag gestures
 *   This means drag works without any Java-side changes.
 *
 * Drag velocity → physics inertia
 * ────────────────────────────────
 *   getDragVelX/Y() exposes smoothed pixel velocity (px/s) to VMDManager,
 *   which uses it to tilt the Bullet physics gravity so hair/clothes lag
 *   behind proportionally to how fast the model is being dragged.
 */

#include "MMDRenderer.h"

#include <android/log.h>
#include <GLES3/gl3.h>

#include <Saba/Model/MMD/PMXModel.h>
#include <Saba/Model/MMD/MMDModel.h>
#include <Saba/Model/MMD/MMDMaterial.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>

#define LOG_TAG "MMDRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Shaders ─────────────────────────────────────────────────────────────────

static const char* TOON_VERT = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

out vec3 v_normal;
out vec2 v_uv;
out vec3 v_worldPos;
out vec3 v_viewNormal;   // view-space normal for sphere-map UV

uniform mat4  u_mvp;
uniform mat4  u_model;
uniform mat4  u_view;
uniform vec2  u_positionOffset;
uniform float u_scale;

void main() {
    vec4 worldPos = u_model * vec4(a_position, 1.0);
    v_worldPos  = worldPos.xyz;
    v_normal    = normalize(mat3(transpose(inverse(u_model))) * a_normal);
    v_uv        = a_uv;

    // View-space normal: only the rotation part of the view matrix
    v_viewNormal = normalize(mat3(u_view) * v_normal);

    vec4 clipPos  = u_mvp * vec4(a_position, 1.0);
    clipPos.x    += u_positionOffset.x * clipPos.w;
    clipPos.y    += u_positionOffset.y * clipPos.w;
    clipPos.xyz  *= u_scale;
    gl_Position   = clipPos;
}
)GLSL";

static const char* TOON_FRAG = R"GLSL(#version 300 es
precision highp float;

in vec3 v_normal;
in vec2 v_uv;
in vec3 v_worldPos;
in vec3 v_viewNormal;

out vec4 fragColor;

uniform sampler2D u_texDiffuse;
uniform int       u_hasTexture;
uniform vec4      u_diffuse;
uniform vec3      u_specular;
uniform vec3      u_ambient;
uniform float     u_globalAlpha;

// Sphere (matcap) map — adds the MMD characteristic sheen
// u_spMode: 0 = off, 1 = multiply, 2 = add
uniform sampler2D u_spTex;
uniform int       u_spMode;

// Key light: front-facing and slightly from upper-right, warm tone
const vec3 LIGHT_DIR  = normalize(vec3(0.4, 1.0, 1.5));
const vec3 LIGHT_COL  = vec3(1.0, 0.97, 0.93);
// Fill light from opposite side — prevents pitch-black shadows
const vec3 RIM_DIR    = normalize(vec3(-0.6, 0.3, -1.0));
const vec3 RIM_COL    = vec3(0.18, 0.20, 0.28);
const vec3 CAMERA_POS = vec3(0.0, 10.0, 40.0);
// Brighter ambient — shadow areas stay readable, not black
const vec3 SCENE_AMB  = vec3(0.30, 0.27, 0.25);

void main() {
    vec3  albedo = u_diffuse.rgb;
    float alpha  = u_diffuse.a;

    if (u_hasTexture == 1) {
        vec4 texColor = texture(u_texDiffuse, v_uv);
        albedo = texColor.rgb;
        alpha  = texColor.a * u_diffuse.a;
    }
    // No texture: diffuse.a is used as-is. Do not force to 1.0 -- shadow
    // materials (eye shadow, hair shadow) have alpha=0.3 in PMX by design.

    if (alpha * u_globalAlpha < 0.01) discard;

    vec3  N     = normalize(v_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);

    // Three-band cel-shading: bright / mid / shadow
    float toon = NdotL > 0.65 ? 0.90 : NdotL > 0.25 ? 0.62 : 0.38;

    // Fill light (rim)
    float rimDot = max(dot(N, RIM_DIR), 0.0);
    float rim    = rimDot > 0.5 ? 0.18 : 0.0;

    vec3 ambientLight = max(u_ambient, SCENE_AMB);
    vec3 litColor = albedo * ambientLight
                  + albedo * LIGHT_COL * toon
                  + albedo * RIM_COL   * rim;

    // Specular highlight
    vec3  viewDir = normalize(CAMERA_POS - v_worldPos);
    vec3  halfDir = normalize(LIGHT_DIR + viewDir);
    float spec    = pow(max(dot(N, halfDir), 0.0), 56.0) * (toon / 0.90);
    litColor     += u_specular * spec * 0.45;

    // ── Sphere / matcap map ───────────────────────────────────────────────
    // The view-space normal XY maps to UV on the sphere texture.
    // This is the standard MMD sphere-map technique and provides most of
    // the characteristic anime-model sheen that plain Phong lighting lacks.
    if (u_spMode != 0) {
        vec3  vn    = normalize(v_viewNormal);
        vec2  spUV  = vn.xy * 0.5 + 0.5;
        vec4  spCol = texture(u_spTex, spUV);

        if (u_spMode == 1) {
            // Multiply: tints the surface (typical for clothing sheen, hair gloss)
            litColor *= spCol.rgb;
        } else {
            // Add: brightest highlights (eyes, metallic parts)
            litColor += spCol.rgb * spCol.a;
        }
    }

    // Saturation boost — vivid colors like the reference render
    float lum  = dot(litColor, vec3(0.299, 0.587, 0.114));
    litColor   = mix(vec3(lum), litColor, 1.0);

    // Subtle contrast micro-curve: lifts midtones, deepens darks
    litColor = litColor * (litColor * 0.10 + 0.95);

    // Darken model by 15% to match desired appearance
    litColor *= 0.75;

    fragColor = vec4(litColor, alpha * u_globalAlpha);
}
)GLSL";

static const char* OUTLINE_VERT = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

uniform mat4  u_mvp;
uniform float u_outlineWidth;
uniform vec2  u_positionOffset;
uniform float u_scale;

void main() {
    vec3 expanded = a_position + a_normal * u_outlineWidth;
    vec4 clipPos  = u_mvp * vec4(expanded, 1.0);
    clipPos.x    += u_positionOffset.x * clipPos.w;
    clipPos.y    += u_positionOffset.y * clipPos.w;
    clipPos.xyz  *= u_scale;
    gl_Position   = clipPos;
}
)GLSL";

static const char* OUTLINE_FRAG = R"GLSL(#version 300 es
precision highp float;

out vec4 fragColor;

uniform vec4  u_outlineColor;
uniform float u_globalAlpha;

void main() {
    fragColor = vec4(u_outlineColor.rgb, u_outlineColor.a * u_globalAlpha);
}
)GLSL";

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::string normalizePath(const std::string& p) {
    std::string out = p;
    for (char& c : out) if (c == '\\') c = '/';
    return out;
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

MMDRenderer::MMDRenderer()  = default;
MMDRenderer::~MMDRenderer() { shutdown(); }

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool MMDRenderer::initialize(int width, int height) {
    m_width  = width;
    m_height = height;

    m_toonShader    = std::make_unique<ShaderProgram>();
    m_outlineShader = std::make_unique<ShaderProgram>();

    if (!m_toonShader->build(TOON_VERT, TOON_FRAG)) {
        LOGE("Toon shader failed"); return false;
    }
    if (!m_outlineShader->build(OUTLINE_VERT, OUTLINE_FRAG)) {
        LOGE("Outline shader failed"); return false;
    }

    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);

    LOGI("initialize OK (%dx%d)", width, height);
    return true;
}

void MMDRenderer::shutdown() {
    for (GLuint t : m_textures)   if (t) glDeleteTextures(1, &t);
    for (GLuint t : m_spTextures) if (t) glDeleteTextures(1, &t);
    m_textures.clear();
    m_spTextures.clear();
    m_spModes.clear();
    if (m_vboPos)  { glDeleteBuffers(1, &m_vboPos);   m_vboPos  = 0; }
    if (m_vboNorm) { glDeleteBuffers(1, &m_vboNorm);  m_vboNorm = 0; }
    if (m_vboUV)   { glDeleteBuffers(1, &m_vboUV);    m_vboUV   = 0; }
    if (m_ibo)     { glDeleteBuffers(1, &m_ibo);      m_ibo     = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao     = 0; }
    if (m_toonShader)    m_toonShader->destroy();
    if (m_outlineShader) m_outlineShader->destroy();
    m_model.reset();
    m_modelLoaded = false;
    LOGI("shutdown complete");
}

// ─── Model loading ────────────────────────────────────────────────────────────

bool MMDRenderer::loadPMXModel(const std::string& pmxPath) {
    LOGI("loadPMXModel: %s", pmxPath.c_str());

    if (m_modelLoaded) {
        for (GLuint t : m_textures)   glDeleteTextures(1, &t);
        for (GLuint t : m_spTextures) glDeleteTextures(1, &t);
        m_textures.clear();
        m_spTextures.clear();
        m_spModes.clear();
        if (m_vboPos)  { glDeleteBuffers(1, &m_vboPos);   m_vboPos  = 0; }
        if (m_vboNorm) { glDeleteBuffers(1, &m_vboNorm);  m_vboNorm = 0; }
        if (m_vboUV)   { glDeleteBuffers(1, &m_vboUV);    m_vboUV   = 0; }
        if (m_ibo)     { glDeleteBuffers(1, &m_ibo);      m_ibo     = 0; }
        if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao     = 0; }
        m_model.reset();
        m_modelLoaded = false;
    }

    m_model = std::make_unique<saba::PMXModel>();
    std::string dir = pmxPath.substr(0, pmxPath.find_last_of("/\\"));
    m_modelDir = dir;

    if (!m_model->Load(pmxPath, dir)) {
        LOGE("PMXModel::Load failed"); m_model.reset(); return false;
    }
    m_model->InitializeAnimation();

    buildVAO();
    loadTextures();

    m_modelLoaded = true;
    LOGI("PMX loaded: %s  verts=%zu  mats=%zu  subMeshes=%zu",
         pmxPath.c_str(),
         m_model->GetVertexCount(),
         m_model->GetMaterialCount(),
         m_model->GetSubMeshCount());
    return true;
}

// ─── VAO ─────────────────────────────────────────────────────────────────────

void MMDRenderer::buildVAO() {
    if (!m_model) return;
    size_t vCount      = m_model->GetVertexCount();
    size_t iCount      = m_model->GetIndexCount();
    size_t idxElemSize = m_model->GetIndexElementSize();
    const void* rawIdx = m_model->GetIndices();

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vboPos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec3)),
                 m_model->GetPositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    glGenBuffers(1, &m_vboNorm);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec3)),
                 m_model->GetNormals(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    glGenBuffers(1, &m_vboUV);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboUV);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec2)),
                 m_model->GetUVs(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);

    glGenBuffers(1, &m_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    if (idxElemSize == 4) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(iCount * sizeof(uint32_t)), rawIdx, GL_STATIC_DRAW);
    } else {
        std::vector<uint32_t> expanded(iCount);
        if (idxElemSize == 1) {
            const uint8_t* s = (const uint8_t*)rawIdx;
            for (size_t i = 0; i < iCount; ++i) expanded[i] = s[i];
        } else {
            const uint16_t* s = (const uint16_t*)rawIdx;
            for (size_t i = 0; i < iCount; ++i) expanded[i] = s[i];
        }
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(iCount * sizeof(uint32_t)),
                     expanded.data(), GL_STATIC_DRAW);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// ─── Textures ─────────────────────────────────────────────────────────────────

void MMDRenderer::loadTextures() {
    if (!m_model) return;
    size_t matCount = m_model->GetMaterialCount();
    m_textures.assign(matCount, 0);

    stbi_set_flip_vertically_on_load(1);
    int loaded = 0, failed = 0;
    for (size_t i = 0; i < matCount; ++i) {
        const auto& mat = m_model->GetMaterials()[i];
        if (mat.m_texture.empty()) continue;

        std::string path = normalizePath(mat.m_texture);
        if (path.empty() || path[0] != '/') path = m_modelDir + "/" + path;

        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, STBI_rgb_alpha);
        if (!data) { failed++; continue; }

        glGenTextures(1, &m_textures[i]);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        // Anisotropic filtering — sharpens textures at oblique angles (hair, clothes).
        // Use GL_TEXTURE_MAX_ANISOTROPY_EXT if the device supports it.
        {
            float maxAniso = 1.f;
            glGetFloatv(0x84FF /*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &maxAniso);
            if (maxAniso > 1.f) {
                float aniso = (maxAniso > 8.f) ? 8.f : maxAniso;
                glTexParameterf(GL_TEXTURE_2D,
                                0x84FE /*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, aniso);
            }
        }
        stbi_image_free(data);
        loaded++;
    }
    LOGI("Textures: %d loaded, %d failed out of %zu materials", loaded, failed, matCount);

    // ── Sphere-map (sp) textures ──────────────────────────────────────────
    // These give the characteristic MMD model sheen: hair gloss, clothing
    // highlights, metallic reflections.  Missing = no effect (mode 0).
    m_spTextures.assign(matCount, 0);
    m_spModes.assign(matCount, 0);
    int spLoaded = 0;

    // Helper: load one texture, return GL id (0 on failure).
    auto loadTex = [&](const std::string& rawPath) -> GLuint {
        std::string p = normalizePath(rawPath);
        if (p.empty()) return 0;
        if (p[0] != '/') p = m_modelDir + "/" + p;
        int w2, h2, c2;
        unsigned char* d = stbi_load(p.c_str(), &w2, &h2, &c2, STBI_rgb_alpha);
        if (!d) return 0;
        GLuint tid = 0;
        glGenTextures(1, &tid);
        glBindTexture(GL_TEXTURE_2D, tid);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w2, h2, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
        glGenerateMipmap(GL_TEXTURE_2D);
        stbi_image_free(d);
        return tid;
    };

    for (size_t i = 0; i < matCount; ++i) {
        const auto& mat = m_model->GetMaterials()[i];
        if (mat.m_spTexture.empty()) continue;

        // MMD sphere mode: None=0, Mul=1, Add=2
        int mode = 0;
        switch (mat.m_spTextureMode) {
            case saba::MMDMaterial::SphereTextureMode::Mul: mode = 1; break;
            case saba::MMDMaterial::SphereTextureMode::Add: mode = 2; break;
            default: break;
        }
        if (mode == 0) continue;

        GLuint tid = loadTex(mat.m_spTexture);
        if (tid) {
            m_spTextures[i] = tid;
            m_spModes[i]    = mode;
            spLoaded++;
        }
    }
    LOGI("Sphere textures: %d loaded out of %zu materials", spLoaded, matCount);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ─── Per-frame vertex upload ──────────────────────────────────────────────────

void MMDRenderer::uploadVertices() {
    if (!m_model || !m_vboPos || !m_vboNorm) return;
    const glm::vec3* pos  = m_model->GetUpdatePositions();
    const glm::vec3* norm = m_model->GetUpdateNormals();
    if (!pos || !norm) return;
    size_t n = m_model->GetVertexCount();
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * sizeof(glm::vec3)), pos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * sizeof(glm::vec3)), norm);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─── Surface ─────────────────────────────────────────────────────────────────

void MMDRenderer::onSurfaceChanged(int w, int h) {
    m_width = w; m_height = h;
    glViewport(0, 0, w, h);
}

void MMDRenderer::setTransform(float x, float y, float scale, float alpha) {
    m_posX = x; m_posY = y; m_scale = scale; m_alpha = alpha;
}

void MMDRenderer::setMorphWeight(const std::string& name, float w) {
    if (!m_model || !m_modelLoaded) return;
    auto* mgr   = m_model->GetMorphManager();
    auto* morph = mgr ? mgr->GetMorph(name) : nullptr;
    if (morph) morph->SetWeight(w);
}

// ─── Touch ───────────────────────────────────────────────────────────────────

void MMDRenderer::onTouchDown(float x, float y) {
    m_touchDownX  = x;
    m_touchDownY  = y;
    m_lastTouchX  = x;
    m_lastTouchY  = y;
    m_fingerDown  = true;
    m_holdTimer   = 0.f;
    m_mode        = InteractMode::None;
    m_accumDragPxX = 0.f;
    m_accumDragPxY = 0.f;
}

void MMDRenderer::onTouchMove(float x, float y) {
    float dx = x - m_lastTouchX;
    float dy = y - m_lastTouchY;
    m_lastTouchX = x;
    m_lastTouchY = y;

    if (m_mode == InteractMode::None) {
        // Check if the gesture crosses the drag threshold
        float distFromDown = std::hypot(x - m_touchDownX, y - m_touchDownY);
        if (distFromDown > DRAG_THRESHOLD_PX) {
            m_mode = InteractMode::Dragging;
            LOGI("Gesture classified: Dragging");
        }
        // Don't move anything yet — wait for classification
        return;
    }

    if (m_mode == InteractMode::Dragging) {
        // The overlay window is moved entirely by Java (OverlayView ACTION_MOVE),
        // so we do NOT accumulate an NDC offset here — the model must stay fixed
        // at the centre of its GL surface at all times.
        // We still track raw pixel deltas so VMDManager can compute drag velocity
        // for the physics-inertia / jiggle systems.
        m_accumDragPxX += dx;
        m_accumDragPxY += dy;
        return;
    }

    if (m_mode == InteractMode::Rotating) {
        // Rotation: degrees per pixel → radians
        constexpr float RAD_PER_PX = ROT_SENSITIVITY * (glm::pi<float>() / 180.f);
        m_rotY += dx * RAD_PER_PX;
        m_rotX += dy * RAD_PER_PX;
        constexpr float HALF_PI = glm::half_pi<float>();
        m_rotX = std::max(-HALF_PI, std::min(HALF_PI, m_rotX));
    }
}

void MMDRenderer::onTouchUp() {
    m_fingerDown = false;
    m_holdTimer  = 0.f;
    if (m_mode == InteractMode::Rotating) {
        m_mode = InteractMode::None;
    }
    // Dragging: the window has already been repositioned by Java.
    // Reset accumulated drag pixels so velocity decays naturally after lift.
    m_accumDragPxX = 0.f;
    m_accumDragPxY = 0.f;
    // Ensure native drag offset is always zero — model stays centred in window.
    m_nativeDragX = 0.f;
    m_nativeDragY = 0.f;
}

// ─── Render ───────────────────────────────────────────────────────────────────

void MMDRenderer::render(float dt) {
    // ── Long-press timer ──────────────────────────────────────────────────
    // While the finger is held and the gesture hasn't been classified yet,
    // accumulate time.  After LONG_PRESS_THRESHOLD seconds → rotation mode.
    if (m_fingerDown && m_mode == InteractMode::None) {
        m_holdTimer += dt;
        if (m_holdTimer >= LONG_PRESS_THRESHOLD) {
            m_mode = InteractMode::Rotating;
            LOGI("Gesture classified: Rotating (long-press)");
        }
    }

    // ── Drag velocity for physics inertia ─────────────────────────────────
    // Compute velocity from accumulated pixel deltas since the last frame,
    // then exponentially smooth it so physics gravity changes feel organic.
    if (dt > 0.f) {
        float rawVx = m_accumDragPxX / dt;
        float rawVy = m_accumDragPxY / dt;
        m_accumDragPxX = 0.f;
        m_accumDragPxY = 0.f;

        // EMA α = 0.35: responsive enough to capture fast flicks but smooth
        // enough to suppress single-frame spikes from touch-event batching.
        constexpr float EMA = 0.35f;
        m_dragVelPxX = m_dragVelPxX * (1.f - EMA) + rawVx * EMA;
        m_dragVelPxY = m_dragVelPxY * (1.f - EMA) + rawVy * EMA;
    }

    // ── Decay velocity after finger lifts ─────────────────────────────────
    if (!m_fingerDown) {
        m_dragVelPxX *= 0.85f;
        m_dragVelPxY *= 0.85f;
    }

    // ── GL rendering ──────────────────────────────────────────────────────
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_modelLoaded || !m_model) return;

    uploadVertices();

    float aspect = (m_height > 0)
                   ? static_cast<float>(m_width) / static_cast<float>(m_height)
                   : 1.f;
    glm::mat4 proj = glm::perspective(glm::radians(45.f), aspect, 0.1f, 500.f);
    glm::mat4 view = glm::lookAt(glm::vec3(0.f, 10.f, 40.f),
                                 glm::vec3(0.f, 10.f,  0.f),
                                 glm::vec3(0.f,  1.f,  0.f));

    // Rotation matrix: model rotates around its own centre
    glm::mat4 model = glm::mat4(1.f);
    model = glm::rotate(model, m_rotY, glm::vec3(0.f, 1.f, 0.f));
    model = glm::rotate(model, m_rotX, glm::vec3(1.f, 0.f, 0.f));

    glm::mat4 mvp = proj * view * model;

    // Model is always centred in the GL surface (NDC 0,0).
    // The overlay window is repositioned by Java when the user drags.
    // m_nativeDragX/Y are kept at zero; m_posX/Y are also zero (set by Java).
    float totalOffsetX = m_posX + m_nativeDragX;   // always 0
    float totalOffsetY = m_posY + m_nativeDragY;   // always 0

    // Outline pass (front-face culled, slightly expanded)
    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    m_outlineShader->use();
    m_outlineShader->setUniformMat4("u_mvp",            glm::value_ptr(mvp));
    m_outlineShader->setUniform1f  ("u_outlineWidth",   0.012f);
    m_outlineShader->setUniform2f  ("u_positionOffset", totalOffsetX, totalOffsetY);
    m_outlineShader->setUniform1f  ("u_scale",          m_scale);
    m_outlineShader->setUniform4f  ("u_outlineColor",   0.05f, 0.05f, 0.05f, 1.f);
    m_outlineShader->setUniform1f  ("u_globalAlpha",    m_alpha);
    drawOutline(mvp);

    // Toon pass
    glCullFace(GL_BACK);
    m_toonShader->use();
    m_toonShader->setUniformMat4("u_mvp",            glm::value_ptr(mvp));
    m_toonShader->setUniformMat4("u_model",          glm::value_ptr(model));
    m_toonShader->setUniformMat4("u_view",           glm::value_ptr(view));
    m_toonShader->setUniform2f  ("u_positionOffset", totalOffsetX, totalOffsetY);
    m_toonShader->setUniform1f  ("u_scale",          m_scale);
    m_toonShader->setUniform1f  ("u_globalAlpha",    m_alpha);
    drawModel();

    glDisable(GL_CULL_FACE);
}

// ─── Draw calls ───────────────────────────────────────────────────────────────

void MMDRenderer::drawModel() {
    if (!m_vao || !m_model) return;
    glBindVertexArray(m_vao);

    const saba::MMDMaterial* mats  = m_model->GetMaterials();
    const saba::MMDSubMesh*  sms   = m_model->GetSubMeshes();
    size_t smCount = m_model->GetSubMeshCount();

    // ── Two-pass render: opaque first, transparent second ─────────────────
    // Without this, a transparent material (e.g. eye shadow, hair shadow,
    // alpha=0.3) drawn early in PMX order writes to the depth buffer and
    // blocks the opaque geometry behind it — causing "missing" face/hair layers.
    // Pass 0: opaque only  (mat.m_alpha >= 1.0) — writes depth normally
    // Pass 1: transparent  (mat.m_alpha <  1.0) — depth test ON, depth write OFF
    for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) glDepthMask(GL_FALSE);  // transparent pass: no depth write

    for (size_t i = 0; i < smCount; ++i) {
        const saba::MMDSubMesh&  sm  = sms[i];
        const saba::MMDMaterial& mat = mats[sm.m_materialID];

        bool isTransparent = (mat.m_alpha < 0.999f);
        if (pass == 0 &&  isTransparent) continue;
        if (pass == 1 && !isTransparent) continue;

        if (mat.m_bothFace) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }

        m_toonShader->setUniform4f("u_diffuse",
            mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        m_toonShader->setUniform3f("u_specular",
            mat.m_specular.r, mat.m_specular.g, mat.m_specular.b);
        m_toonShader->setUniform3f("u_ambient",
            mat.m_ambient.r, mat.m_ambient.g, mat.m_ambient.b);

        size_t matIdx = static_cast<size_t>(sm.m_materialID);

        // ── Diffuse texture (unit 0) ──────────────────────────────────────
        GLuint texId = (matIdx < m_textures.size()) ? m_textures[matIdx] : 0;
        bool hasTex  = (texId != 0);
        m_toonShader->setUniform1i("u_hasTexture", hasTex ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hasTex ? texId : 0);
        m_toonShader->setUniform1i("u_texDiffuse", 0);

        // ── Sphere-map texture (unit 1) ───────────────────────────────────
        GLuint spId   = (matIdx < m_spTextures.size()) ? m_spTextures[matIdx] : 0;
        int    spMode = (matIdx < m_spModes.size())    ? m_spModes[matIdx]    : 0;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, spId ? spId : 0);
        m_toonShader->setUniform1i("u_spTex",  1);
        m_toonShader->setUniform1i("u_spMode", spId ? spMode : 0);

        glDrawElements(GL_TRIANGLES,
                       (GLsizei)sm.m_vertexCount,
                       GL_UNSIGNED_INT,
                       (void*)((uintptr_t)sm.m_beginIndex * sizeof(uint32_t)));
    } // end submesh loop

    if (pass == 1) glDepthMask(GL_TRUE); // restore depth write
    } // end pass loop

    // Restore texture units to a clean state
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

void MMDRenderer::drawOutline(const glm::mat4& /*mvp*/) {
    if (!m_vao || !m_model) return;

    const saba::MMDMaterial* mats  = m_model->GetMaterials();
    const saba::MMDSubMesh*  sms   = m_model->GetSubMeshes();
    size_t smCount = m_model->GetSubMeshCount();

    glBindVertexArray(m_vao);
    for (size_t i = 0; i < smCount; ++i) {
        const saba::MMDSubMesh&  sm  = sms[i];
        const saba::MMDMaterial& mat = mats[sm.m_materialID];
        if (mat.m_bothFace || !mat.m_edgeFlag || mat.m_edgeSize <= 0.0f) continue;
        glDrawElements(GL_TRIANGLES,
                       (GLsizei)sm.m_vertexCount,
                       GL_UNSIGNED_INT,
                       (void*)((uintptr_t)sm.m_beginIndex * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}
