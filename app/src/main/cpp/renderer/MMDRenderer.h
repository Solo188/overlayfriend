#pragma once

#include <string>
#include <memory>
#include <vector>

#include <GLES3/gl3.h>

#include <glm/glm.hpp>

#include <Saba/Model/MMD/MMDModel.h>
#include <Saba/Model/MMD/PMXModel.h>

#include "ShaderProgram.h"

class MMDRenderer {
public:
    MMDRenderer();
    ~MMDRenderer();

    bool initialize(int width, int height);
    void shutdown();

    bool loadPMXModel(const std::string& pmxPath);
    void onSurfaceChanged(int width, int height);

    void render(float deltaTime);

    void onTouchDown(float x, float y);
    void onTouchMove(float x, float y);
    void onTouchUp();

    void setTransform(float x, float y, float scale, float alpha);
    void setMorphWeight(const std::string& morphName, float weight);

    saba::MMDModel* getModel() const { return m_model.get(); }

    int surfaceWidth()  const { return m_width; }
    int surfaceHeight() const { return m_height; }

private:
    void buildVAO();
    void loadTextures();
    void uploadVertices();
    void drawModel();
    void drawOutline(const glm::mat4& mvp);

    std::unique_ptr<saba::PMXModel> m_model;

    std::unique_ptr<ShaderProgram> m_toonShader;
    std::unique_ptr<ShaderProgram> m_outlineShader;

    GLuint m_vao     = 0;
    GLuint m_vboPos  = 0;
    GLuint m_vboNorm = 0;
    GLuint m_vboUV   = 0;
    GLuint m_ibo     = 0;

    std::vector<GLuint> m_textures;

    std::string m_modelDir;

    float m_posX   = 0.f;
    float m_posY   = 0.f;
    float m_scale  = 1.f;
    float m_alpha  = 1.f;

    // ── Rotation state (drag-to-rotate) ───────────────────────────────────
    // m_rotX — pitch (vertical drag, rotation around X axis), clamped ±90°
    // m_rotY — yaw   (horizontal drag, rotation around Y axis), free
    float m_rotX = 0.f;
    float m_rotY = 0.f;

    float m_lastTouchX   = 0.f;
    float m_lastTouchY   = 0.f;
    bool  m_isDragging   = false;

    // Sensitivity: degrees per pixel → converted to radians on use
    static constexpr float ROT_SENSITIVITY = 0.45f;

    int   m_width  = 0;
    int   m_height = 0;

    bool  m_modelLoaded = false;
};
