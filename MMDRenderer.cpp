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

// Реализация конструктора (исправляет ошибку линковки)
MMDRenderer::MMDRenderer() : 
    m_glReady(false), 
    m_vao(0), 
    m_vboPos(0), 
    m_vboNorm(0), 
    m_vboUV(0), 
    m_vboIBO(0),
    m_texturesLoaded(false) 
{
    LOGI("MMDRenderer instance created");
}

// Реализация деструктора
MMDRenderer::~MMDRenderer() {
    LOGI("MMDRenderer instance destroying...");
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    if (m_vboPos != 0) {
        GLuint vbos[] = {m_vboPos, m_vboNorm, m_vboUV, m_vboIBO};
        glDeleteBuffers(4, vbos);
    }
    for (auto tex : m_textures) {
        if (tex != 0) glDeleteTextures(1, &tex);
    }
}

bool MMDRenderer::initialize(int width, int height) {
    LOGI("nativeInit w=%d h=%d", width, height);

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // Прозрачный фон
    glEnable(GL_DEPTH_TEST);

    // Создаем шейдер
    m_toonShader = std::make_unique<ShaderProgram>();
    const char* vs = R"(#version 300 es
        layout(location = 0) in vec3 a_pos;
        layout(location = 1) in vec3 a_norm;
        layout(location = 2) in vec2 a_uv;
        uniform mat4 u_mvp;
        out vec2 v_uv;
        out vec3 v_norm;
        void main() {
            v_uv = a_uv;
            v_norm = a_norm;
            gl_Position = u_mvp * vec4(a_pos, 1.0);
        }
    )";

    const char* fs = R"(#version 300 es
        precision highp float;
        in vec2 v_uv;
        in vec3 v_norm;
        uniform vec4 u_diffuse;
        uniform sampler2D u_texDiffuse;
        uniform int u_hasTexture;
        out vec4 fragColor;
        void main() {
            vec4 col = u_diffuse;
            if (u_hasTexture != 0) col *= texture(u_texDiffuse, v_uv);
            float light = max(dot(normalize(v_norm), normalize(vec3(0.5, 1.0, 0.5))), 0.3);
            fragColor = vec4(col.rgb * light, col.a);
        }
    )";

    if (!m_toonShader->build(vs, fs)) {
        LOGE("Shader build failed!");
        return false;
    }

    float aspect = (float)width / (float)height;
    m_proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    // Камера MMD (отодвинута назад на 20 единиц)
    m_view = glm::lookAt(glm::vec3(0, 10, 20), glm::vec3(0, 10, 0), glm::vec3(0, 1, 0));

    m_glReady = true;
    return true;
}

bool MMDRenderer::loadPMXModel(const std::string& path) {
    if (!m_glReady) {
        LOGE("Cannot load model: GL not ready");
        return false;
    }

    LOGI("Loading PMX: %s", path.c_str());
    auto pmxModel = std::make_unique<saba::PMXModel>();
    if (!pmxModel->Load(path)) {
        LOGE("Saba failed to load PMX");
        return false;
    }

    m_model = std::make_unique<saba::MMDModel>();
    if (!m_model->Initialize(pmxModel.get())) {
        LOGE("Saba MMDModel init failed");
        return false;
    }

    // Загрузка текстур
    std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    size_t matCount = m_model->GetMaterialCount();
    m_textures.clear();
    for (size_t i = 0; i < matCount; ++i) {
        const auto& mat = m_model->GetPMXModel()->GetMaterials()[i];
        std::string texPath = dir + mat.m_texture;
        
        // Исправляем слеши для Android
        for (char &c : texPath) if (c == '\\') c = '/';

        GLuint texId = 0;
        int w, h, comp;
        unsigned char* data = stbi_load(texPath.c_str(), &w, &h, &comp, 4);
        if (data) {
            glGenTextures(1, &texId);
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            stbi_image_free(data);
            LOGI("Texture loaded: %s (ID=%u)", texPath.c_str(), texId);
        } else {
            LOGE("Failed to load texture: %s", texPath.c_str());
        }
        m_textures.push_back(texId);
    }

    buildVAO();
    m_texturesLoaded = true;
    LOGI("Model loaded: %zu verts", m_model->GetVertexCount());
    return true;
}

void MMDRenderer::buildVAO() {
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    size_t vCount = m_model->GetVertexCount();

    // Positions
    glGenBuffers(1, &m_vboPos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(glm::vec3), m_model->GetPositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

    // Normals
    glGenBuffers(1, &m_vboNorm);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(glm::vec3), m_model->GetNormals(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, 0);

    // UVs
    glGenBuffers(1, &m_vboUV);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboUV);
    glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(glm::vec2), m_model->GetUVs(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // Indices
    glGenBuffers(1, &m_vboIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_vboIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_model->GetIndexCount() * 4, m_model->GetIndices(), GL_STATIC_DRAW);

    glBindVertexArray(0);
}

void MMDRenderer::render(float deltaTime) {
    if (!m_glReady || !m_model) return;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_TEST);

    // Если есть анимация - обновляем вершины
    m_model->Update();
    uploadVertices();

    m_toonShader->use();
    glm::mat4 mvp = m_proj * m_view;
    m_toonShader->setUniformMatrix4fv("u_mvp", glm::value_ptr(mvp));

    glBindVertexArray(m_vao);

    const auto* sms = m_model->GetSubMeshes();
    const auto* mats = m_model->GetPMXModel()->GetMaterials();
    size_t smCount = m_model->GetSubMeshCount();

    for (size_t i = 0; i < smCount; ++i) {
        const auto& sm = sms[i];
        const auto& mat = mats[sm.m_materialID];

        m_toonShader->setUniform4f("u_diffuse", mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        
        GLuint texId = (sm.m_materialID < m_textures.size()) ? m_textures[sm.m_materialID] : 0;
        m_toonShader->setUniform1i("u_hasTexture", texId != 0 ? 1 : 0);
        if (texId != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texId);
            m_toonShader->setUniform1i("u_texDiffuse", 0);
        }

        glDrawElements(GL_TRIANGLES, sm.m_vertexCount, GL_UNSIGNED_INT, (void*)(sm.m_beginIndex * 4));
    }

    glBindVertexArray(0);
}

void MMDRenderer::uploadVertices() {
    size_t vCount = m_model->GetVertexCount();
    
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vCount * sizeof(glm::vec3), m_model->GetUpdatePositions());
    
    glBindBuffer(GL_ARRAY_BUFFER, m_vboNorm);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vCount * sizeof(glm::vec3), m_model->GetUpdateNormals());
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

