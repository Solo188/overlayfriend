/**
 * VMDManager.cpp
 *
 * Saba animation update sequence (mandatory order):
 *   model->BeginAnimation()
 *   model->UpdateAllAnimation(anim, frameNumber, physicsDt)
 *   model->Update()
 *   model->EndAnimation()
 *
 * ── Smooth animation ────────────────────────────────────────────────────────
 *
 *  Problem: animation "jerks" between keyframes.
 *  Root causes and fixes applied here:
 *
 *  1. deltaTime spikes (GC, resume, first frame)
 *     Fix: clamp rawDeltaTime to MAX_ANIM_DT (50 ms) before advancing
 *          the animation clock.  This prevents the model from "teleporting"
 *          to a distant keyframe in a single step.
 *
 *  2. Physics timestep variance
 *     Bullet physics is sensitive to irregular dt.  A single long frame
 *     causes the physics to make many substeps at once, which produces a
 *     visible "snap".  Fix: feed Bullet a SMOOTHED (EMA) dt that absorbs
 *     single-frame spikes while still following the true average frame rate.
 *
 *  3. Jerky loop seam
 *     When the animation resets to frame 0 the overshoot time was discarded,
 *     causing a brief freeze at the loop point.
 *     Fix: carry the overshoot (excess time beyond m_animDuration) into the
 *          new iteration in startNextLoop().
 *
 * ── Physics inertia during drag ──────────────────────────────────────────────
 *
 *  When the user drags the model, MMDRenderer computes drag velocity
 *  (pixels/second) and passes it via setDragVelocity().  We use it to tilt
 *  the Bullet gravity vector opposite to the motion direction, so hair and
 *  clothes appear to "lag behind" with intensity proportional to speed.
 *  The effect blends in/out smoothly using an exponential filter on the
 *  gravity components.
 */

#include "VMDManager.h"
#include "renderer/MMDRenderer.h"

#include <Saba/Model/MMD/VMDAnimation.h>
#include <Saba/Model/MMD/VMDFile.h>
#include <Saba/Model/MMD/MMDModel.h>
#include <Saba/Model/MMD/MMDPhysics.h>

#include <btBulletDynamicsCommon.h>

#include <android/log.h>
#include <cmath>
#include <algorithm>

#define LOG_TAG "VMDManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Maximum animation clock advance per frame (seconds).
// Anything larger (GC pause, app resume) is silently clamped.
static constexpr float MAX_ANIM_DT = 0.05f;   // 50 ms ≡ min 20 fps

// EMA weight for the physics deltaTime smoother.
// Lower = smoother but less responsive to genuine FPS changes.
static constexpr float PHYS_DT_EMA = 0.12f;

// Scale factor: pixels/second → Bullet gravity offset (m/s²)
// 2000 px/s drag → ~12 m/s² lateral gravity (roughly 1.2 g)
static constexpr float DRAG_INERTIA_SCALE = 12.f / 2000.f;

// Maximum lateral gravity from inertia (keeps hair/cloth from going insane)
static constexpr float MAX_LATERAL_GRAVITY = 15.f;

// How fast the inertia gravity returns to normal when drag stops.
// 0 = instant reset, 1 = never resets.  0.75 gives a natural settling feel.
static constexpr float INERTIA_DECAY = 0.75f;

static const std::unordered_map<std::string, int> CATEGORY_TIER = {
    {"idle",   VMDManager::TIER_STRANGER},
    {"touch",  VMDManager::TIER_STRANGER},
    {"night",  VMDManager::TIER_PARTNER},
    {"friend", VMDManager::TIER_FRIEND},
    // "user" intentionally absent — always unlocked
};

VMDManager::VMDManager()
    : m_rng(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{
    std::uniform_real_distribution<float> d(2.f, 5.f);
    m_blinkInterval = d(m_rng);
}

VMDManager::~VMDManager() = default;

void VMDManager::attachRenderer(MMDRenderer* r) { m_renderer = r; }

void VMDManager::setDragVelocity(float vx, float vy) {
    m_dragVelX = vx;
    m_dragVelY = vy;
}

bool VMDManager::loadMotion(const std::string& vmdPath, const std::string& category) {
    if (!m_renderer || !m_renderer->getModel()) {
        LOGE("loadMotion: no model loaded yet"); return false;
    }

    saba::MMDModel* raw = m_renderer->getModel();
    std::shared_ptr<saba::MMDModel> modelPtr(raw, [](saba::MMDModel*){});

    auto anim = std::make_shared<saba::VMDAnimation>();
    if (!anim->Create(modelPtr)) { LOGE("VMDAnimation::Create failed"); return false; }

    saba::VMDFile vmdFile;
    if (!saba::ReadVMDFile(&vmdFile, vmdPath.c_str())) {
        LOGE("ReadVMDFile failed: %s", vmdPath.c_str()); return false;
    }
    if (!anim->Add(vmdFile)) {
        LOGE("VMDAnimation::Add failed: %s", vmdPath.c_str()); return false;
    }

    m_motionPool[category].push_back({vmdPath, anim});
    LOGI("Loaded VMD [%s] %s  pool=%zu",
         category.c_str(), vmdPath.c_str(), m_motionPool[category].size());
    return true;
}

bool VMDManager::isCategoryUnlocked(const std::string& cat) const {
    auto it = CATEGORY_TIER.find(cat);
    if (it == CATEGORY_TIER.end()) return true;
    return m_affinityTier >= it->second;
}

void VMDManager::playCategory(const std::string& category) {
    if (!isCategoryUnlocked(category)) {
        LOGI("Category [%s] locked for tier %d", category.c_str(), m_affinityTier);
        return;
    }
    auto it = m_motionPool.find(category);
    if (it == m_motionPool.end() || it->second.empty()) {
        LOGI("No motions in category [%s]", category.c_str()); return;
    }

    m_pauseActive = false;
    m_pauseTimer  = 0.f;

    auto& pool = it->second;
    std::uniform_int_distribution<size_t> pick(0, pool.size()-1);
    size_t idx = pick(m_rng);

    if (m_currentAnim) {
        m_prevAnim     = m_currentAnim;
        m_prevAnimTime = m_animTime;
        m_blend        = {0.f, 0.5f, true};
    }
    m_currentAnim     = pool[idx].anim;
    m_currentCategory = category;
    m_animTime        = 0.f;
    m_animDuration    = static_cast<float>(m_currentAnim->GetMaxKeyTime()) / 30.f;
    LOGI("Playing [%s] idx=%zu dur=%.2fs", category.c_str(), idx, m_animDuration);
}

// ─── Physics inertia ──────────────────────────────────────────────────────────

void VMDManager::applyPhysicsInertia(void* modelRaw, float /*dt*/) {
    saba::MMDModel* model = static_cast<saba::MMDModel*>(modelRaw);
    auto* mmPhysics = model->GetMMDPhysics();
    if (!mmPhysics) return;

    btDiscreteDynamicsWorld* world = mmPhysics->GetDynamicsWorld();
    if (!world) return;

    // Target lateral gravity from drag velocity (opposes motion = inertia)
    float targetGx = std::max(-MAX_LATERAL_GRAVITY,
                     std::min( MAX_LATERAL_GRAVITY, -m_dragVelX * DRAG_INERTIA_SCALE));
    float targetGz = std::max(-MAX_LATERAL_GRAVITY,
                     std::min( MAX_LATERAL_GRAVITY, -m_dragVelY * DRAG_INERTIA_SCALE));

    // Smoothly blend current gravity toward target so transitions are gradual
    btVector3 curG = world->getGravity();
    float newGx = curG.x() + (targetGx - curG.x()) * (1.f - INERTIA_DECAY);
    float newGz = curG.z() + (targetGz - curG.z()) * (1.f - INERTIA_DECAY);

    // When there is no drag, blend back toward 0 on lateral axes
    if (std::abs(m_dragVelX) < 5.f && std::abs(m_dragVelY) < 5.f) {
        newGx *= INERTIA_DECAY;
        newGz *= INERTIA_DECAY;
    }

    world->setGravity(btVector3(newGx, -9.8f, newGz));
}

// ─── Main update ──────────────────────────────────────────────────────────────

void VMDManager::update(float rawDeltaTime) {
    if (!m_renderer) return;

    saba::MMDModel* model = m_renderer->getModel();
    if (!model) return;

    // ── Animation clock: clamped to prevent large jumps ───────────────────
    const float animDt = std::min(rawDeltaTime, MAX_ANIM_DT);

    // ── Physics dt: exponential moving average — absorbs single-frame spikes
    // while still tracking genuine frame-rate changes over time.
    m_smoothedPhysDt = m_smoothedPhysDt * (1.f - PHYS_DT_EMA)
                     + animDt            * PHYS_DT_EMA;
    // Hard floor so Bullet never receives 0 (avoid div-by-zero in substep calc)
    const float physDt = std::max(m_smoothedPhysDt, 0.001f);

    model->BeginAnimation();

    tickBlink(animDt);
    tickMouth(animDt);
    applyBlend(animDt);
    applyPhysicsInertia(model, physDt);

    if (m_pauseActive) {
        m_pauseTimer -= animDt;
        if (m_pauseTimer <= 0.f) {
            m_pauseActive = false;
            LOGI("Pause over — restarting [%s]", m_currentCategory.c_str());
            playCategory(m_currentCategory);
        }
        // Bind pose while paused; physics continues so hair settles naturally.
        model->UpdateAllAnimation(nullptr, 0.f, physDt);

    } else if (m_currentAnim) {
        m_animTime += animDt;

        if (m_animDuration > 0.f && m_animTime > m_animDuration) {
            startNextLoop();
            // After a seamless loop, startNextLoop() adjusts m_animTime for
            // overshoot so UpdateAllAnimation evaluates the correct sub-frame.
        }

        // frameNumber is float → Saba Bezier-interpolates between keyframes.
        model->UpdateAllAnimation(m_currentAnim.get(), m_animTime * 30.f, physDt);

    } else {
        model->UpdateAllAnimation(nullptr, 0.f, physDt);
    }

    model->Update();
    model->EndAnimation();
}

void VMDManager::applyBlend(float dt) {
    if (!m_blend.active || !m_prevAnim || !m_currentAnim) return;
    m_blend.elapsed += dt;
    m_prevAnimTime  += dt;
    if (m_blend.elapsed / m_blend.duration >= 1.f) {
        m_blend.active = false;
        m_prevAnim.reset();
    }
}

void VMDManager::startNextLoop() {
    if (m_currentCategory == "user") {
        LOGI("[user] ended — pausing %.1fs", USER_LOOP_PAUSE);
        m_pauseActive = true;
        m_pauseTimer  = USER_LOOP_PAUSE;
        // Keep m_animTime at the last frame so the model holds end-pose.
    } else {
        // Seamless loop: preserve overshoot so the seam is invisible.
        float overshoot = m_animTime - m_animDuration;
        playCategory(m_currentCategory);        // resets m_animTime to 0
        m_animTime = std::max(0.f, overshoot);  // restore overshoot
    }
}

// ─── Blink ────────────────────────────────────────────────────────────────────

void VMDManager::tickBlink(float dt) {
    if (!m_renderer) return;
    m_blinkTimer += dt;
    if (!m_blinking && m_blinkTimer >= m_blinkInterval) {
        m_blinking      = true;
        m_blinkPhase    = 0.f;
        m_blinkTimer    = 0.f;
        std::uniform_real_distribution<float> d(2.5f, 6.f);
        m_blinkInterval = d(m_rng);
    }
    if (m_blinking) {
        m_blinkPhase += dt;
        constexpr float H = 0.08f;
        float w;
        if      (m_blinkPhase < H)     w = m_blinkPhase / H;
        else if (m_blinkPhase < H*2.f) w = 1.f - (m_blinkPhase - H) / H;
        else { w = 0.f; m_blinking = false; }
        m_renderer->setMorphWeight("\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", w);
        m_renderer->setMorphWeight("blink", w);
    }
}

// ─── Mouth idle ───────────────────────────────────────────────────────────────

void VMDManager::tickMouth(float dt) {
    if (!m_renderer) return;
    m_mouthPhase += dt * 1.2f;
    float w = (std::sin(m_mouthPhase) * 0.5f + 0.5f) * 0.06f;
    m_renderer->setMorphWeight("\xe3\x81\x82", w);
    m_renderer->setMorphWeight("mouth_a", w);
}

void VMDManager::setAffinityTier(int tier) {
    m_affinityTier = tier;
    LOGI("AffinityTier = %d", tier);
}
