/**
 * MMDRenderer.cpp
 *
 * OpenGL ES 3.0 renderer for PMX models loaded via the Saba library.
 *
 * Key architectural points that match the actual Saba API:
 *  - Saba exposes vertices as separate arrays: GetPositions()/GetNormals()/GetUVs()
 *    and their animated counterparts GetUpdatePositions()/GetUpdateNormals()/GetUpdateUVs().
 *    There is NO unified MMDVertex struct — we use 3 separate VBOs.
 *  - Rendering by material is done via GetSubMeshes() which gives (m_beginIndex,
 *    m_vertexCount, m_materialID).  NOT via a field on MMDMaterial.
 *  - MMDMaterial::m_diffuse is vec3; alpha is the separate field m_alpha.
 *  - Physics is owned internally by PMXModel via MMDPhysicsManager.
 *    No external MMDPhysics pointer is needed.
 *  - Morph weight is set via GetMorphManager()->GetMorph(name)->SetWeight(w).
 *  - stb_image.h lives in saba/external/stb/include/  (on include path already).
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

// stb_image — bundled inside saba's external/stb/include/
// STB_IMAGE_IMPLEMENTATION is defined by Saba internally; we only need the header.
#include <stb_image.h>

#define LOG_TAG "MMDRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Shader sources ───────────────────────────────────────────────────────────

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
uniform vec4      u_diffuse;       // xyz = diffuse rgb, w = alpha
uniform vec3      u_specular;
uniform vec3      u_ambient;
uniform float     u_globalAlpha;

const vec3 LIGHT_DIR = normalize(vec3(0.5, 1.0, 0.8));
const vec3 LIGHT_COL = vec3(1.0, 0.98, 0.95);

void main() {
    vec4 baseColor = u_diffuse;
    if (u_hasTexture == 1) {
        baseColor *= texture(u_texDiffuse, v_uv);
    }
    if (baseColor.a < 0.05) discard;

    vec3  N     = normalize(v_normal);
    float NdotL = max(dot(N, LIGHT_DIR), 0.0);
    float toon  = (NdotL > 0.75) ? 1.0 : (NdotL > 0.35) ? 0.65 : 0.35;

    vec3 ambient  = u_ambient * 0.3;
    vec3 diffuse  = baseColor.rgb * LIGHT_COL * toon;

    vec3  viewDir = normalize(-v_worldPos);
    vec3  halfDir = normalize(LIGHT_DIR + viewDir);
    float spec    = pow(max(dot(N, halfDir), 0.0), 32.0) * toon;
    vec3  specular = u_specular * spec * 0.5;

    float finalA  = baseColor.a * u_globalAlpha;
    fragColor = vec4(ambient + diffuse + specular, finalA);
}
)GLSL";

static const char* OUTLINE_VERT = R"GLSL(#version 300 es
precision highp float;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4  u_mvp;
uniform float u_outlineWidth;
uniform vec2  u_positionOffset;
uniform float u_scale;

void main() {
    vec3 expandedPos = a_position + a_normal * u_outlineWidth;
    vec4 clipPos     = u_mvp * vec4(expandedPos, 1.0);
    clipPos.x       += u_positionOffset.x * clipPos.w;
    clipPos.y       += u_positionOffset.y * clipPos.w;
    clipPos.xyz     *= u_scale;
    gl_Position      = clipPos;
}
)GLSL";

static const char* OUTLINE_FRAG = R"GLSL(#version 300 es
precision mediump float;
out vec4 fragColor;
uniform vec4  u_outlineColor;
uniform float u_globalAlpha;
void main() {
    fragColor = vec4(u_outlineColor.rgb, u_outlineColor.a * u_globalAlpha);
}
)GLSL";

// ─── Lifecycle ────────────────────────────────────────────────────────────────

MMDRenderer::MMDRenderer()  = default;
MMDRenderer::~MMDRenderer() { shutdown(); }

bool MMDRenderer::initialize(int width, int height) {
    m_width  = width;
    m_height = height;

    m_toonShader    = std::make_unique<ShaderProgram>();
    m_outlineShader = std::make_unique<ShaderProgram>();

    if (!m_toonShader->build(TOON_VERT, TOON_FRAG)) {
        LOGE("Toon shader build failed");
        return false;
    }
    if (!m_outlineShader->build(OUTLINE_VERT, OUTLINE_FRAG)) {
        LOGE("Outline shader build failed");
        return false;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.f, 0.f, 0.f, 0.f);

    LOGI("initialize OK (%dx%d)", width, height);
    return true;
}

void MMDRenderer::shutdown() {
    for (GLuint tid : m_textures) {
        if (tid) glDeleteTextures(1, &tid);
    }
    m_textures.clear();

    if (m_vboPos)  { glDeleteBuffers(1, &m_vboPos);  m_vboPos  = 0; }
    if (m_vboNorm) { glDeleteBuffers(1, &m_vboNorm); m_vboNorm = 0; }
    if (m_vboUV)   { glDeleteBuffers(1, &m_vboUV);   m_vboUV   = 0; }
    if (m_ibo)     { glDeleteBuffers(1, &m_ibo);     m_ibo     = 0; }
    if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao    = 0; }

    if (m_toonShader)    m_toonShader->destroy();
    if (m_outlineShader) m_outlineShader->destroy();

    m_model.reset();
    m_modelLoaded = false;
    LOGI("shutdown complete");
}

// ─── Model loading ────────────────────────────────────────────────────────────

bool MMDRenderer::loadPMXModel(const std::string& pmxPath) {
    // Clean up previous model if any
    if (m_modelLoaded) {
        for (GLuint tid : m_textures) glDeleteTextures(1, &tid);
        m_textures.clear();
        if (m_vboPos)  { glDeleteBuffers(1, &m_vboPos);  m_vboPos  = 0; }
        if (m_vboNorm) { glDeleteBuffers(1, &m_vboNorm); m_vboNorm = 0; }
        if (m_vboUV)   { glDeleteBuffers(1, &m_vboUV);   m_vboUV   = 0; }
        if (m_ibo)     { glDeleteBuffers(1, &m_ibo);     m_ibo     = 0; }
        if (m_vao)     { glDeleteVertexArrays(1, &m_vao); m_vao    = 0; }
        m_model.reset();
        m_modelLoaded = false;
    }

    m_model = std::make_unique<saba::PMXModel>();
    std::string texBaseDir = pmxPath.substr(0, pmxPath.find_last_of("/\\"));

    if (!m_model->Load(pmxPath, texBaseDir)) {
        LOGE("PMXModel::Load failed: %s", pmxPath.c_str());
        m_model.reset();
        return false;
    }

    // Initialize animation subsystem (required before any UpdateAllAnimation calls)
    m_model->InitializeAnimation();

    // Physics is managed internally by PMXModel's MMDPhysicsManager.
    // No external MMDPhysics object is needed.

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

// ─── VAO + VBO setup ──────────────────────────────────────────────────────────
//
// Saba stores vertex data as parallel arrays, not an interleaved struct:
//   GetPositions()  -> const glm::vec3*   (base/bind pose)
//   GetNormals()    -> const glm::vec3*
//   GetUVs()        -> const glm::vec2*   (static)
// After UpdateAllAnimation():
//   GetUpdatePositions() -> animated positions uploaded each frame
//   GetUpdateNormals()   -> animated normals uploaded each frame
//
// We create one VAO with 3 attribute bindings pointing at 3 separate VBOs.

void MMDRenderer::buildVAO() {
    if (!m_model) return;

    size_t vCount = m_model->GetVertexCount();
    size_t iCount = m_model->GetIndexCount();

    // Indices — Saba may use 1-, 2-, or 4-byte indices depending on vertex count.
    // GetIndexElementSize() returns the byte size per index.
    // For simplicity with glDrawElements we need uint32; we convert if needed.
    size_t idxElemSize = m_model->GetIndexElementSize();
    const void* rawIdx = m_model->GetIndices();

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    // -- Positions VBO (dynamic — updated every frame) --
    glGenBuffers(1, &m_vboPos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vCount * sizeof(glm::vec3)),
                 m_model->GetPositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    // -- Normals VBO (dynamic — updated every frame) --
    glGenBuffers(1, &m_vboNorm);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vCount * sizeof(glm::vec3)),
                 m_model->GetNormals(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);

    // -- UV VBO (static — UVs don't animate) --
    glGenBuffers(1, &m_vboUV);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboUV);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vCount * sizeof(glm::vec2)),
                 m_model->GetUVs(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);

    // -- Index buffer --
    // Convert to uint32 if Saba uses smaller element size.
    glGenBuffers(1, &m_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
    if (idxElemSize == 4) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(iCount * sizeof(uint32_t)),
                     rawIdx, GL_STATIC_DRAW);
    } else {
        // Expand 1-byte or 2-byte indices to uint32
        std::vector<uint32_t> expanded(iCount);
        if (idxElemSize == 1) {
            const uint8_t* src = static_cast<const uint8_t*>(rawIdx);
            for (size_t i = 0; i < iCount; ++i) expanded[i] = src[i];
        } else { // 2
            const uint16_t* src = static_cast<const uint16_t*>(rawIdx);
            for (size_t i = 0; i < iCount; ++i) expanded[i] = src[i];
        }
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(iCount * sizeof(uint32_t)),
                     expanded.data(), GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
}

void MMDRenderer::loadTextures() {
    if (!m_model) return;
    size_t matCount = m_model->GetMaterialCount();
    m_textures.assign(matCount, 0);

    for (size_t i = 0; i < matCount; ++i) {
        const auto& mat = m_model->GetMaterials()[i];
        if (mat.m_texture.empty()) continue;

        glGenTextures(1, &m_textures[i]);
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(mat.m_texture.c_str(), &w, &h, &comp, 4);
        if (data) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            stbi_image_free(data);
        } else {
            LOGE("stbi_load failed: %s", mat.m_texture.c_str());
            glDeleteTextures(1, &m_textures[i]);
            m_textures[i] = 0;
        }
    }
}

// ─── Per-frame vertex upload ──────────────────────────────────────────────────

void MMDRenderer::uploadVertices() {
    if (!m_model) return;
    size_t vCount = m_model->GetVertexCount();

    // Upload animated (post-physics) positions
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(vCount * sizeof(glm::vec3)),
                    m_model->GetUpdatePositions());

    // Upload animated normals
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(vCount * sizeof(glm::vec3)),
                    m_model->GetUpdateNormals());
}

// ─── Surface / transform ──────────────────────────────────────────────────────

void MMDRenderer::onSurfaceChanged(int width, int height) {
    m_width  = width;
    m_height = height;
    glViewport(0, 0, width, height);
}

void MMDRenderer::onTouchDown(float x, float y) {
    LOGI("onTouchDown (%.1f, %.1f)", x, y);
}

void MMDRenderer::setTransform(float x, float y, float scale, float alpha) {
    m_posX  = x;
    m_posY  = y;
    m_scale = scale;
    m_alpha = alpha;
}

// ─── Morph weight ─────────────────────────────────────────────────────────────
// Correct API: GetMorphManager()->GetMorph(name)->SetWeight(w)

void MMDRenderer::setMorphWeight(const std::string& morphName, float weight) {
    if (!m_model || !m_modelLoaded) return;
    auto* mgr   = m_model->GetMorphManager();
    auto* morph = mgr ? mgr->GetMorph(morphName) : nullptr;
    if (morph) morph->SetWeight(weight);
}

// ─── Main render ──────────────────────────────────────────────────────────────

void MMDRenderer::render(float /*deltaTime*/) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_modelLoaded || !m_model) return;

    // Upload animated vertex data produced by VMDManager::update()
    // (which called model->UpdateAllAnimation() already)
    uploadVertices();

    float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    glm::mat4 proj  = glm::perspective(glm::radians(30.f), aspect, 0.1f, 100.f);
    glm::mat4 view  = glm::lookAt(glm::vec3(0, 10, 30),
                                  glm::vec3(0, 10,  0),
                                  glm::vec3(0,  1,  0));
    glm::mat4 model = glm::mat4(1.f);
    glm::mat4 mvp   = proj * view * model;

    // Outline pass — expand along normals, cull front faces
    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    m_outlineShader->use();
    m_outlineShader->setUniformMat4("u_mvp",          glm::value_ptr(mvp));
    m_outlineShader->setUniform1f  ("u_outlineWidth",  0.015f);
    m_outlineShader->setUniform2f  ("u_positionOffset", m_posX, m_posY);
    m_outlineShader->setUniform1f  ("u_scale",          m_scale);
    m_outlineShader->setUniform4f  ("u_outlineColor",   0.1f, 0.1f, 0.1f, 1.f);
    m_outlineShader->setUniform1f  ("u_globalAlpha",    m_alpha);
    drawOutline();

    // Toon pass — cull back faces
    glCullFace(GL_BACK);
    m_toonShader->use();
    m_toonShader->setUniformMat4("u_mvp",           glm::value_ptr(mvp));
    m_toonShader->setUniformMat4("u_model",         glm::value_ptr(model));
    m_toonShader->setUniform2f  ("u_positionOffset", m_posX, m_posY);
    m_toonShader->setUniform1f  ("u_scale",          m_scale);
    m_toonShader->setUniform1f  ("u_globalAlpha",    m_alpha);
    drawModel();

    glDisable(GL_CULL_FACE);
}

// ─── Draw calls ───────────────────────────────────────────────────────────────
//
// Rendering per-material via GetSubMeshes():
//   MMDSubMesh::m_beginIndex  — first index in the IBO for this sub-mesh
//   MMDSubMesh::m_vertexCount — number of indices (triangles * 3)
//   MMDSubMesh::m_materialID  — index into GetMaterials()
//
// MMDMaterial fields used:
//   m_diffuse  (vec3)   +  m_alpha  (float)  — NOT m_diffuse.a
//   m_specular (vec3)
//   m_ambient  (vec3)
//   m_texture  (string) — path; texture pre-loaded into m_textures[matIdx]

void MMDRenderer::drawModel() {
    if (!m_vao || !m_model) return;
    glBindVertexArray(m_vao);

    const saba::MMDMaterial* mats     = m_model->GetMaterials();
    const saba::MMDSubMesh*  subMeshes = m_model->GetSubMeshes();
    size_t                   smCount  = m_model->GetSubMeshCount();

    for (size_t i = 0; i < smCount; ++i) {
        const saba::MMDSubMesh&  sm  = subMeshes[i];
        const saba::MMDMaterial& mat = mats[sm.m_materialID];

        // m_diffuse is vec3, m_alpha is separate float
        m_toonShader->setUniform4f("u_diffuse",
            mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        m_toonShader->setUniform3f("u_specular",
            mat.m_specular.r, mat.m_specular.g, mat.m_specular.b);
        m_toonShader->setUniform3f("u_ambient",
            mat.m_ambient.r, mat.m_ambient.g, mat.m_ambient.b);

        GLuint texId = (static_cast<size_t>(sm.m_materialID) < m_textures.size())
                       ? m_textures[sm.m_materialID] : 0;
        bool hasTex  = (texId != 0);
        m_toonShader->setUniform1i("u_hasTexture", hasTex ? 1 : 0);
        if (hasTex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texId);
            m_toonShader->setUniform1i("u_texDiffuse", 0);
        }

        glDrawElements(GL_TRIANGLES,
                       static_cast<GLsizei>(sm.m_vertexCount),
                       GL_UNSIGNED_INT,
                       reinterpret_cast<void*>(
                           static_cast<uintptr_t>(sm.m_beginIndex) * sizeof(uint32_t)));
    }

    glBindVertexArray(0);
}

void MMDRenderer::drawOutline() {
    if (!m_vao || !m_model) return;
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(m_model->GetIndexCount()),
                   GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}
