/**
 * MMDRenderer.cpp — OpenGL ES 3.0 renderer for PMX/MMD models (Saba library).
 *
 * Saba API summary used here:
 *  - Vertices: GetPositions()/GetNormals()/GetUVs() + GetUpdate*() after animation
 *  - SubMeshes: GetSubMeshes() → m_beginIndex, m_vertexCount, m_materialID
 *  - MMDMaterial: m_diffuse(vec3), m_alpha(float), m_specular(vec3), m_ambient(vec3)
 *  - Morphs: GetMorphManager()->GetMorph(name)->SetWeight(w)
 *  - No unified MMDVertex struct; use 3 separate VBOs.
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

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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

uniform mat4  u_mvp;
uniform mat4  u_model;
uniform vec2  u_positionOffset;
uniform float u_scale;

void main() {
    vec4 worldPos = u_model * vec4(a_position, 1.0);
    v_worldPos = worldPos.xyz;
    v_normal   = normalize(mat3(transpose(inverse(u_model))) * a_normal);
    v_uv       = a_uv;

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

out vec4 fragColor;

uniform sampler2D u_texDiffuse;
uniform int       u_hasTexture;
uniform vec4      u_diffuse;    // rgb = diffuse color, a = material alpha
uniform vec3      u_specular;
uniform vec3      u_ambient;
uniform float     u_globalAlpha;

const vec3 LIGHT_DIR  = normalize(vec3(0.5, 1.0, 0.8));
const vec3 LIGHT_COL  = vec3(1.0, 0.98, 0.95);
const vec3 CAMERA_POS = vec3(0.0, 10.0, 40.0);
// Scene ambient: low so it doesn't wash out bright textures.
// Total light budget = SCENE_AMB + LIGHT_COL*toon_max = 0.15 + 0.85 = 1.0
const vec3 SCENE_AMB  = vec3(0.15, 0.15, 0.15);

void main() {
    // --- Albedo ---------------------------------------------------------
    // Game-converted PMX: texture IS the color; diffuse/ambient = lighting
    // coefficients only. Multiplying tex * diffuse_rgb caused washout.
    vec3  albedo = u_diffuse.rgb;
    float alpha  = u_diffuse.a;

    if (u_hasTexture == 0) {
        alpha = max(alpha, 1.0);
    } else {
        vec4 texColor = texture(u_texDiffuse, v_uv);
        albedo = texColor.rgb;
        alpha  = texColor.a * u_diffuse.a;
    }

    if (alpha * u_globalAlpha < 0.01) discard;

    // --- Lighting -------------------------------------------------------
    vec3  N     = normalize(v_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);

    // Three toon bands. Values chosen so:
    //   bright : SCENE_AMB(0.15) + LIGHT*0.85 = 1.00  (full saturation)
    //   mid    : SCENE_AMB(0.15) + LIGHT*0.65 = 0.80  (slightly darker)
    //   shadow : SCENE_AMB(0.15) + LIGHT*0.40 = 0.55  (clearly shaded, not black)
    float toon = NdotL > 0.75 ? 0.85 : NdotL > 0.35 ? 0.65 : 0.40;

    // Correct Phong ambient + diffuse (separate terms, no double-counting):
    //   ambient component  = albedo * SCENE_AMB (fills shadow side)
    //   diffuse component  = albedo * LIGHT_COL * toon
    vec3 ambientLight = max(u_ambient, SCENE_AMB);
    vec3 litColor = albedo * ambientLight + albedo * LIGHT_COL * toon;

    // Specular — subtle, only on lit faces.
    vec3  viewDir  = normalize(CAMERA_POS - v_worldPos);
    vec3  halfDir  = normalize(LIGHT_DIR + viewDir);
    float spec     = pow(max(dot(N, halfDir), 0.0), 48.0) * (toon / 0.85);
    vec3  specular = u_specular * spec * 0.3;

    fragColor = vec4(litColor + specular, alpha * u_globalAlpha);
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

static unsigned char* tryLoadTexture(const std::string& path, int* w, int* h, int* comp) {
    // MMD/PMX uses DirectX UV convention: (0,0) = top-left.
    // OpenGL UV convention: (0,0) = bottom-left.
    // Flipping vertically on load corrects the mismatch so textures
    // land on the correct body parts instead of being mirrored/shifted.
    stbi_set_flip_vertically_on_load(1);
    unsigned char* data = stbi_load(path.c_str(), w, h, comp, STBI_rgb_alpha);
    return data;
}

// ─── Constructor / Destructor ───────────────────────────────────────────────
// These MUST be defined in the .cpp file because the class uses std::unique_ptr
// with incomplete types (forward-declared in the header). The compiler needs to
// know the full type at the point of destruction, which only the .cpp sees after
// all headers are included.
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
    for (GLuint t : m_textures) if (t) glDeleteTextures(1, &t);
    m_textures.clear();
    if (m_vboPos)  { glDeleteBuffers(1, &m_vboPos);       m_vboPos  = 0; }
    if (m_vboNorm) { glDeleteBuffers(1, &m_vboNorm);      m_vboNorm = 0; }
    if (m_vboUV)   { glDeleteBuffers(1, &m_vboUV);        m_vboUV   = 0; }
    if (m_ibo)     { glDeleteBuffers(1, &m_ibo);          m_ibo     = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao);     m_vao     = 0; }
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
        for (GLuint t : m_textures) glDeleteTextures(1, &t);
        m_textures.clear();
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
    // Store the model directory so loadTextures() can resolve relative paths.
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

    // Positions (dynamic — updated every frame via uploadVertices)
    glGenBuffers(1, &m_vboPos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    // Use GetPositions() (bind-pose) for the initial buffer allocation.
    // GetUpdatePositions() is only valid after the first UpdateAllAnimation call.
    // uploadVertices() overwrites this data with GetUpdatePositions() each frame.
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec3)),
                 m_model->GetPositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    // Normals (dynamic — updated every frame via uploadVertices)
    glGenBuffers(1, &m_vboNorm);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec3)),
                 m_model->GetNormals(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    // UVs (static)
    glGenBuffers(1, &m_vboUV);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboUV);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vCount * sizeof(glm::vec2)),
                 m_model->GetUVs(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);

    // Indices — expand to uint32 if needed
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

    int loaded = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < matCount; ++i) {
        const auto& mat = m_model->GetMaterials()[i];
        if (mat.m_texture.empty()) { skipped++; continue; }

        std::string path = normalizePath(mat.m_texture);

        // FIX: Saba may store texture paths as relative (e.g. "tex/body.png").
        // If the path is not absolute, prepend the model directory so that
        // stbi_load() can find the file on the filesystem.
        if (path.empty() || path[0] != '/') {
            path = m_modelDir + "/" + path;
        }
        LOGI("Texture[%zu]: %s", i, path.c_str());

        int w = 0, h = 0, comp = 0;
        unsigned char* data = tryLoadTexture(path, &w, &h, &comp);
        if (!data) {
            LOGE("Tex FAILED[%zu]: %s", i, path.c_str());
            failed++;
            continue;
        }

        glGenTextures(1, &m_textures[i]);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        stbi_image_free(data);
        loaded++;
    }
    LOGI("Textures: %d loaded, %d failed out of %zu materials", loaded, failed, matCount);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ─── Per-frame upload ─────────────────────────────────────────────────────────

void MMDRenderer::uploadVertices() {
    if (!m_model || !m_vboPos || !m_vboNorm) return;

    const glm::vec3* updPos  = m_model->GetUpdatePositions();
    const glm::vec3* updNorm = m_model->GetUpdateNormals();

    // Guard: GetUpdate* return nullptr if UpdateAllAnimation has never been
    // called.  Skip the GPU upload in that case — the bind-pose data
    // uploaded in buildVAO() will be used until the first animation frame.
    if (!updPos || !updNorm) return;

    size_t n = m_model->GetVertexCount();

    // FIX: Bind VBO explicitly before glBufferSubData.  The VAO stores
    // attribute format pointers but NOT which ARRAY_BUFFER is currently
    // bound — glBufferSubData operates on the bound ARRAY_BUFFER, not the VAO.
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * sizeof(glm::vec3)), updPos);

    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * sizeof(glm::vec3)), updNorm);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ─── Surface / transform ──────────────────────────────────────────────────────

void MMDRenderer::onSurfaceChanged(int w, int h) {
    m_width = w; m_height = h;
    glViewport(0, 0, w, h);
    LOGI("onSurfaceChanged %dx%d", w, h);
}

void MMDRenderer::onTouchDown(float x, float y) {
    LOGI("onTouchDown (%.1f, %.1f)", x, y);
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

// ─── Render ───────────────────────────────────────────────────────────────────

void MMDRenderer::render(float /*dt*/) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_modelLoaded || !m_model) return;

    // NOTE: Animation update (BeginAnimation/UpdateAllAnimation/etc.) is driven
    // by VMDManager::update() which is called from nativeRender() before this
    // function.  We only need to upload the already-updated vertex data here.
    uploadVertices();

    // Camera: MMD/Saba coordinate system.
    // A typical PMX character is ~20 units tall (center of mass ~10 units up).
    // eye at (0, 10, 40), looking at (0, 10, 0), FOV 45° frames the full body
    // with a small margin in a 400×600 overlay window.
    // near=0.1, far=500 covers the full Saba scene range.
    float aspect = (m_height > 0)
        ? static_cast<float>(m_width) / static_cast<float>(m_height)
        : 1.f;
    glm::mat4 proj  = glm::perspective(glm::radians(45.f), aspect, 0.1f, 500.f);
    glm::mat4 view  = glm::lookAt(glm::vec3(0.f, 10.f, 40.f),
                                  glm::vec3(0.f, 10.f,  0.f),
                                  glm::vec3(0.f,  1.f,  0.f));
    glm::mat4 model = glm::mat4(1.f);
    glm::mat4 mvp   = proj * view * model;

    // Outline pass
    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    m_outlineShader->use();
    m_outlineShader->setUniformMat4("u_mvp",            glm::value_ptr(mvp));
    m_outlineShader->setUniform1f  ("u_outlineWidth",   0.012f);
    m_outlineShader->setUniform2f  ("u_positionOffset", m_posX, m_posY);
    m_outlineShader->setUniform1f  ("u_scale",          m_scale);
    m_outlineShader->setUniform4f  ("u_outlineColor",   0.05f, 0.05f, 0.05f, 1.f);
    m_outlineShader->setUniform1f  ("u_globalAlpha",    m_alpha);
    drawOutline();

    // Toon pass
    glCullFace(GL_BACK);
    m_toonShader->use();
    m_toonShader->setUniformMat4("u_mvp",            glm::value_ptr(mvp));
    m_toonShader->setUniformMat4("u_model",          glm::value_ptr(model));
    m_toonShader->setUniform2f  ("u_positionOffset", m_posX, m_posY);
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

    for (size_t i = 0; i < smCount; ++i) {
        const saba::MMDSubMesh&  sm  = sms[i];
        const saba::MMDMaterial& mat = mats[sm.m_materialID];

        m_toonShader->setUniform4f("u_diffuse",
            mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        m_toonShader->setUniform3f("u_specular",
            mat.m_specular.r, mat.m_specular.g, mat.m_specular.b);
        m_toonShader->setUniform3f("u_ambient",
            mat.m_ambient.r, mat.m_ambient.g, mat.m_ambient.b);

        size_t matIdx = static_cast<size_t>(sm.m_materialID);
        GLuint texId  = (matIdx < m_textures.size()) ? m_textures[matIdx] : 0;
        bool hasTex   = (texId != 0);
        m_toonShader->setUniform1i("u_hasTexture", hasTex ? 1 : 0);
        if (hasTex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texId);
            m_toonShader->setUniform1i("u_texDiffuse", 0);
        }

        glDrawElements(GL_TRIANGLES,
                       (GLsizei)sm.m_vertexCount,
                       GL_UNSIGNED_INT,
                       (void*)((uintptr_t)sm.m_beginIndex * sizeof(uint32_t)));
    }
    glBindVertexArray(0);
}

void MMDRenderer::drawOutline() {
    if (!m_vao || !m_model) return;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES,
                   (GLsizei)m_model->GetIndexCount(),
                   GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}
