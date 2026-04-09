#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <random>
#include <chrono>

class MMDRenderer;

namespace saba {
    class VMDAnimation;
}

/**
 * VMDManager — Multi-layer VMD animation controller for MMD models.
 *
 * Architecture:
 *   Layer 0 (base):  Idle animation looping continuously.
 *                    Weight = 1.0 normally, fades to 0.0 when event plays.
 *                    When a pose is active, idle overlays it at weight 0.3.
 *   Layer 1 (event): Waiting / dance / touch animations.
 *                    Weight = 1.0 while active, returns to 0.0 when finished.
 *
 * Random events:
 *   Every 60–600 s a random waiting (70%) or dance (30%) animation fires.
 *   onTouch() overrides any running event immediately with a touch animation.
 *
 * Motion pool layout (auto-scanned from modelDir/motions/):
 *   idle/      — base looping breath / stand animations
 *   poses/     — static or near-static poses (treated as idle replacements)
 *   waiting/   — idle-variant events fired by the random timer
 *   dance/     — dance events fired by the random timer
 *   touch/     — high-priority reactions fired by onTouch()
 */
class VMDManager {
public:
    static constexpr int TIER_STRANGER = 0;
    static constexpr int TIER_FRIEND   = 1;
    static constexpr int TIER_PARTNER  = 2;

    VMDManager();
    ~VMDManager();

    void attachRenderer(MMDRenderer* renderer);

    // Scan modelDir/motions/{idle,poses,waiting,dance,touch} and load all .vmd files.
    // Automatically starts idle playback after loading.
    void scanMotions(const std::string& modelDir);

    // Load a single VMD file into the given category pool.
    bool loadMotion(const std::string& vmdPath, const std::string& category);

    // Immediately interrupt any running event and play a random touch animation.
    void onTouch();

    // Called every frame from the GL thread.
    // rawDeltaTime: actual elapsed seconds (clamping is applied internally).
    void update(float rawDeltaTime);

    // Drag velocity in screen pixels/second — used to tilt Bullet gravity for
    // hair/cloth inertia proportional to drag speed.
    void setDragVelocity(float vx, float vy);

    void setAffinityTier(int tier);

private:
    // One animation layer (idle/base or event).
    struct AnimState {
        std::shared_ptr<saba::VMDAnimation> anim;
        float time     = 0.f;   // playback position (seconds)
        float duration = 0.f;   // total length (seconds); 0 = static
        float weight   = 0.f;   // current blended weight (0..1)
        float target   = 0.f;   // target weight for smooth fade
        std::string category;
        bool  looping  = true;
    };

    MMDRenderer* m_renderer = nullptr;

    // Motion pool: category → loaded VMDAnimation objects
    std::map<std::string, std::vector<std::shared_ptr<saba::VMDAnimation>>> m_pool;

    // Layer 0: base (idle / pose)
    AnimState m_base;

    // Layer 1: event (waiting / dance / touch)
    AnimState m_event;
    bool      m_eventActive = false;

    // Seconds until the next random waiting/dance event fires.
    float m_nextEventTimer = 120.f;

    // EMA-smoothed physics deltaTime for Bullet substep stability.
    float m_smoothedPhysDt = 0.016f;

    // Drag velocity (pixels/second) supplied by MMDRenderer.
    float m_dragVelX = 0.f, m_dragVelY = 0.f;

    // Previous drag velocity for jiggle impulse detection.
    float m_prevDragVelX = 0.f, m_prevDragVelY = 0.f;

    // Accumulated jiggle impulse (breast/hip bodies).
    float m_jiggleImpulseX = 0.f, m_jiggleImpulseY = 0.f;
    bool  m_jiggleFired = false;

    // Blink state
    float m_blinkTimer    = 0.f;
    float m_blinkInterval = 3.5f;
    float m_blinkPhase    = 0.f;
    bool  m_blinking      = false;

    // Mouth idle oscillation
    float m_mouthPhase = 0.f;

    std::mt19937 m_rng;
    int m_affinityTier = TIER_STRANGER;

    // Internal helpers
    std::shared_ptr<saba::VMDAnimation> pickRandom(const std::string& cat);
    std::shared_ptr<saba::VMDAnimation> loadSingleVMD(const std::string& path);
    void startEvent(const std::string& category);
    void tickEventTimer(float dt);
    void tickBlink(float dt);
    void tickMouth(float dt);
    void applyPhysicsInertia(void* model, float dt);
    void applyJiggleImpulses(void* model);
};
