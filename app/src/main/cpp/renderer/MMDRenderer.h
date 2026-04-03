#pragma once

#include <string>
#include <memory>
#include <vector>

#include <GLES3/gl3.h>

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
    void setTransform(float x, float y, float scale, float alpha);
    void setMorphWeight(const std::string& morphName, float weight);

    // Returns the base MMDModel interface (used by VMDManager)
    saba::MMDModel* getModel() const { return m_model.get(); }

    int surfaceWidth()  const { return m_width; }
    int surfaceHeight() const { return m_height; }

private:
    void buildVAO();
    void uploadVertices();
    void drawModel();
    void drawOutline();

    std::unique_ptr<saba::PMXModel> m_model;

    std::unique_ptr<ShaderProgram> m_toonShader;
    std::unique_ptr<ShaderProgram> m_outlineShader;

    // Three separate VBOs: positions, normals, UVs  (Saba's vertex layout)
    GLuint m_vao     = 0;
    GLuint m_vboPos  = 0;   // vec3 positions  (updated every frame via GetUpdatePositions)
    GLuint m_vboNorm = 0;   // vec3 normals     (updated every frame via GetUpdateNormals)
    GLuint m_vboUV   = 0;   // vec2 UVs         (static after load)
    GLuint m_ibo     = 0;   // uint32 indices   (static after load)

    std::vector<GLuint> m_textures;   // one per material slot

    float m_posX   = 0.f;
    float m_posY   = 0.f;
    float m_scale  = 1.f;
    float m_alpha  = 1.f;

    int   m_width  = 0;
    int   m_height = 0;

    bool  m_modelLoaded = false;
};
