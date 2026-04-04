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

// Конструктор: инициализируем все нулями
MMDRenderer::MMDRenderer() : 
    m_glReady(false), m_vao(0), m_vboPos(0), m_vboNorm(0), 
    m_vboUV(0), m_vboIBO(0), m_texturesLoaded(false) {}

MMDRenderer::~MMDRenderer() {
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    if (m_vboPos != 0) {
        GLuint vbos[] = {m_vboPos, m_vboNorm, m_vboUV, m_vboIBO};
        glDeleteBuffers(4, vbos);
    }
}

bool MMDRenderer::initialize(int width, int height) {
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); 
    glEnable(GL_DEPTH_TEST);

    m_toonShader = std::make_unique<ShaderProgram>();
    // Простой шейдер, который рисует модель даже без текстур
    const char* vs = R"(#version 300 es
        layout(location = 0) in vec3 a_pos;
        layout(location = 1) in vec3 a_norm;
        layout(location = 2) in vec2 a_uv;
        uniform mat4 u_mvp;
        out vec2 v_uv;
        void main() {
            v_uv = a_uv;
            gl_Position = u_mvp * vec4(a_pos, 1.0);
        })";

    const char* fs = R"(#version 300 es
        precision highp float;
        in vec2 v_uv;
        uniform sampler2D u_texDiffuse;
        uniform int u_hasTexture;
        uniform vec4 u_diffuse;
        out vec4 fragColor;
        void main() {
            vec4 col = u_diffuse;
            if (u_hasTexture != 0) col *= texture(u_texDiffuse, v_uv);
            if (col.a < 0.1) discard; // Убираем совсем прозрачные части
            fragColor = col;
        })";

    m_toonShader->build(vs, fs);

    float aspect = (float)width / (float)height;
    m_proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 1000.0f);
    // Отодвигаем камеру на 40 единиц назад, чтобы видеть всю Yvonne
    m_view = glm::lookAt(glm::vec3(0, 10, 40), glm::vec3(0, 10, 0), glm::vec3(0, 1, 0));

    m_glReady = true;
    return true;
}

bool MMDRenderer::loadPMXModel(const std::string& path) {
    auto pmxModel = std::make_unique<saba::PMXModel>();
    if (!pmxModel->Load(path)) return false;

    m_model = std::make_unique<saba::MMDModel>();
    m_model->Initialize(pmxModel.get());

    // ЗАПОМИНАЕМ ПАПКУ МОДЕЛИ (Важно для текстур!)
    std::string modelDir = path.substr(0, path.find_last_of("/\\") + 1);

    m_textures.clear();
    for (const auto& mat : pmxModel->GetMaterials()) {
        // Склеиваем путь: Папка + Имя текстуры из PMX
        std::string fullTexPath = modelDir + mat.m_texture;
        // Исправляем обратные слеши из Windows-формата PMX
        for (char &c : fullTexPath) if (c == '\\') c = '/';

        GLuint texId = 0;
        int w, h, comp;
        unsigned char* data = stbi_load(fullTexPath.c_str(), &w, &h, &comp, 4);
        if (data) {
            glGenTextures(1, &texId);
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            stbi_image_free(data);
            LOGI("Texture Loaded: %s", fullTexPath.c_str());
        }
        m_textures.push_back(texId);
    }

    buildVAO();
    return true;
}

void MMDRenderer::buildVAO() {
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    auto vCount = m_model->GetVertexCount();
    
    glGenBuffers(1, &m_vboPos);
    glBindBuffer(GL_ARRAY_BUFFER, m_vboPos);
    glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(glm::vec3), m_model->GetPositions(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

    glGenBuffers(1, &m_vboIBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_vboIBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_model->GetIndexCount() * 4, m_model->GetIndices(), GL_STATIC_DRAW);

    glBindVertexArray(0);
}

void MMDRenderer::render(float deltaTime) {
    if (!m_glReady || !m_model) return;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_TEST);
    m_model->Update();

    m_toonShader->use();
    glm::mat4 mvp = m_proj * m_view;
    m_toonShader->setUniformMatrix4fv("u_mvp", glm::value_ptr(mvp));

    glBindVertexArray(m_vao);

    auto sms = m_model->GetSubMeshes();
    auto mats = m_model->GetPMXModel()->GetMaterials();

    for (size_t i = 0; i < m_model->GetSubMeshCount(); ++i) {
        const auto& sm = sms[i];
        const auto& mat = mats[sm.m_materialID];

        // Передаем цвет, даже если текстура не загрузится
        m_toonShader->setUniform4f("u_diffuse", mat.m_diffuse.r, mat.m_diffuse.g, mat.m_diffuse.b, mat.m_alpha);
        
        if (sm.m_materialID < m_textures.size() && m_textures[sm.m_materialID] != 0) {
            m_toonShader->setUniform1i("u_hasTexture", 1);
            glBindTexture(GL_TEXTURE_2D, m_textures[sm.m_materialID]);
        } else {
            m_toonShader->setUniform1i("u_hasTexture", 0);
        }

        glDrawElements(GL_TRIANGLES, sm.m_vertexCount, GL_UNSIGNED_INT, (void*)(sm.m_beginIndex * 4));
    }
}
