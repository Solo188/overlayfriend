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
    // Interaction state machine
    //   None     — finger is down but not yet classified
    //   Dragging — finger moved > DRAG_THRESHOLD before long-press fired
    //   Rotating — long-press (1 s) completed; drag now rotates the model
    enum class InteractMode { None, Dragging, Rotating };

    MMDRenderer();
    ~MMDRenderer();

    bool initialize(int width, int height);
    void shutdown();

    bool loadPMXModel(const std::string& pmxPath);
    void onSurfaceChanged(int width, int height);

    // Main per-frame call.  dt is actual elapsed time (already clamped at call site).
    void render(float dt);

    // Touch events — dispatched from the GL-thread touch queue in native-lib.cpp
    void onTouchDown(float x, float y);
    void onTouchMove(float x, float y);
    void onTouchUp();

    // Java-side anchor position/scale/alpha (unchanged by native drag)
    void setTransform(float x, float y, float scale, float alpha);
    void setMorphWeight(const std::string& morphName, float weight);

    saba::MMDModel* getModel() const { return m_model.get(); }

    int surfaceWidth()  const { return m_width; }
    int surfaceHeight() const { return m_height; }

    // Drag velocity in screen pixels/second, used by VMDManager for physics inertia.
    float getDragVelX() const { return m_dragVelPxX; }
    float getDragVelY() const { return m_dragVelPxY; }

    InteractMode getInteractMode() const { return m_mode; }

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
    // Sphere-map textures (one per material, 0 = not present).
    // MMD sphere maps add the characteristic model sheen.
    // Mode per material: 0 = off, 1 = multiply, 2 = add.
    std::vector<GLuint> m_spTextures;
    std::vector<int>    m_spModes;
    std::string m_modelDir;

    // ── Position / scale / alpha set by Java ─────────────────────────────
    float m_posX  = 0.f;
    float m_posY  = 0.f;
    float m_scale = 1.f;
    float m_alpha = 1.f;

    // ── Native drag offset (NDC units, additive over m_posX/Y) ───────────
    // While dragging the model stays in place relative to the finger.
    // When dragging ends the offset stays (model remains where it was left).
    float m_nativeDragX = 0.f;
    float m_nativeDragY = 0.f;

    // ── Rotation (activated by long-press hold) ────────────────────────
    float m_rotX = 0.f;   // pitch  ±90°
    float m_rotY = 0.f;   // yaw    free

    // ── Touch input state ─────────────────────────────────────────────────
    float        m_touchDownX = 0.f;   // position when finger first landed
    float        m_touchDownY = 0.f;
    float        m_lastTouchX = 0.f;   // position at last MOVE event
    float        m_lastTouchY = 0.f;
    bool         m_fingerDown = false;
    float        m_holdTimer  = 0.f;   // seconds since ACTION_DOWN with no move
    InteractMode m_mode       = InteractMode::None;

    // Accumulated pixel deltas since last render() — used to compute velocity
    float m_accumDragPxX = 0.f;
    float m_accumDragPxY = 0.f;

    // Smoothed drag velocity (pixels/second), exposed to VMDManager for physics
    float m_dragVelPxX = 0.f;
    float m_dragVelPxY = 0.f;

    // ── Constants ─────────────────────────────────────────────────────────
    // A finger must move more than this in pixels before we classify the
    // gesture as a drag (rather than a potential long-press).
    static constexpr float DRAG_THRESHOLD_PX   = 8.f;
    static constexpr float LONG_PRESS_THRESHOLD = 1.0f;   // seconds
    static constexpr float ROT_SENSITIVITY      = 0.45f;  // degrees per pixel

    int  m_width       = 0;
    int  m_height      = 0;
    bool m_modelLoaded = false;
};
