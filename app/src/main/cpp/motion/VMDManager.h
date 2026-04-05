#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <random>
#include <chrono>

class MMDRenderer;

namespace saba {
    class VMDAnimation;
}

class VMDManager {
public:
    static constexpr int TIER_STRANGER = 0;
    static constexpr int TIER_FRIEND   = 1;
    static constexpr int TIER_PARTNER  = 2;

    static constexpr float USER_LOOP_PAUSE = 10.f;

    VMDManager();
    ~VMDManager();

    void attachRenderer(MMDRenderer* renderer);

    bool loadMotion(const std::string& vmdPath, const std::string& category);
    void playCategory(const std::string& category);

    // Called every frame from nativeRender on the GL thread.
    // rawDeltaTime: actual elapsed seconds (caller should NOT pre-clamp it —
    // VMDManager applies its own clamping and smoothing internally).
    void update(float rawDeltaTime);

    void setAffinityTier(int tier);

    // Drag velocity in screen pixels/second, supplied by MMDRenderer.
    // Used to tilt Bullet physics gravity so hair/clothes exhibit inertia
    // proportional to how fast/sharply the model is being dragged.
    void setDragVelocity(float vx, float vy);

private:
    using Clock = std::chrono::steady_clock;

    struct MotionEntry {
        std::string                         path;
        std::shared_ptr<saba::VMDAnimation> anim;
    };

    struct BlendState {
        float elapsed  = 0.f;
        float duration = 0.5f;
        bool  active   = false;
    };

    MMDRenderer* m_renderer = nullptr;

    std::unordered_map<std::string, std::vector<MotionEntry>> m_motionPool;

    std::shared_ptr<saba::VMDAnimation> m_currentAnim;
    std::string                          m_currentCategory;
    float                                m_animTime     = 0.f;
    float                                m_animDuration = 0.f;

    std::shared_ptr<saba::VMDAnimation> m_prevAnim;
    float                                m_prevAnimTime = 0.f;
    BlendState                           m_blend;

    bool  m_pauseActive = false;
    float m_pauseTimer  = 0.f;

    // Running average of physics deltaTime.
    // Used only for the Bullet substep — keeps physics simulation smooth even
    // when individual frames stutter.
    float m_smoothedPhysDt = 0.016f;

    // Drag velocity (pixels/second) from MMDRenderer
    float m_dragVelX = 0.f;
    float m_dragVelY = 0.f;

    float m_blinkTimer    = 0.f;
    float m_blinkInterval = 3.5f;
    float m_blinkPhase    = 0.f;
    bool  m_blinking      = false;

    float m_mouthPhase = 0.f;

    std::mt19937 m_rng;

    int m_affinityTier = TIER_STRANGER;

    void tickBlink(float dt);
    void tickMouth(float dt);
    void applyBlend(float dt);
    void startNextLoop();
    void applyPhysicsInertia(void* model, float dt);

    bool isCategoryUnlocked(const std::string& category) const;
};
