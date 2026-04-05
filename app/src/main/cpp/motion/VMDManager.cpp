/**
 * VMDManager.cpp
 *
 * Correct Saba animation sequence per frame:
 *   model->BeginAnimation()
 *   model->UpdateAllAnimation(anim, frame, dt)   <- bones/IK/physics
 *   model->Update()                              <- SKIN MESH recalculation
 *   model->EndAnimation()
 *
 * Loop behaviour by category:
 *   "idle", "touch", "night", "friend" — seamless immediate loop
 *   "user" (files from Documents/Assistant/motion) — 10-second pause after
 *           each play-through, then restarts.  The model holds bind pose
 *           during the pause so hair/cloth physics still run.
 */

#include "VMDManager.h"
#include "renderer/MMDRenderer.h"

#include <Saba/Model/MMD/VMDAnimation.h>
#include <Saba/Model/MMD/VMDFile.h>
#include <Saba/Model/MMD/MMDModel.h>

#include <android/log.h>
#include <cmath>
#include <algorithm>

#define LOG_TAG "VMDManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const std::unordered_map<std::string, int> CATEGORY_TIER = {
    {"idle",   VMDManager::TIER_STRANGER},
    {"touch",  VMDManager::TIER_STRANGER},
    {"night",  VMDManager::TIER_PARTNER},
    {"friend", VMDManager::TIER_FRIEND},
    // "user" is intentionally absent — always unlocked
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

bool VMDManager::loadMotion(const std::string& vmdPath, const std::string& category) {
    if (!m_renderer || !m_renderer->getModel()) {
        LOGE("loadMotion: no model loaded yet");
        return false;
    }

    saba::MMDModel* raw = m_renderer->getModel();
    std::shared_ptr<saba::MMDModel> modelPtr(raw, [](saba::MMDModel*){});

    auto anim = std::make_shared<saba::VMDAnimation>();
    if (!anim->Create(modelPtr)) {
        LOGE("VMDAnimation::Create failed");
        return false;
    }

    saba::VMDFile vmdFile;
    if (!saba::ReadVMDFile(&vmdFile, vmdPath.c_str())) {
        LOGE("ReadVMDFile failed: %s", vmdPath.c_str());
        return false;
    }
    if (!anim->Add(vmdFile)) {
        LOGE("VMDAnimation::Add failed: %s", vmdPath.c_str());
        return false;
    }

    m_motionPool[category].push_back({vmdPath, anim});
    LOGI("Loaded VMD [%s] %s  pool=%zu",
         category.c_str(), vmdPath.c_str(), m_motionPool[category].size());
    return true;
}

bool VMDManager::isCategoryUnlocked(const std::string& cat) const {
    auto it = CATEGORY_TIER.find(cat);
    if (it == CATEGORY_TIER.end()) return true;  // "user" and unknown → always unlocked
    return m_affinityTier >= it->second;
}

void VMDManager::playCategory(const std::string& category) {
    if (!isCategoryUnlocked(category)) {
        LOGI("Category [%s] locked for tier %d", category.c_str(), m_affinityTier);
        return;
    }
    auto it = m_motionPool.find(category);
    if (it == m_motionPool.end() || it->second.empty()) {
        LOGI("No motions in category [%s]", category.c_str());
        return;
    }

    // Cancel any ongoing pause when a new category is explicitly requested
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

// ─── Main update — called every frame from nativeRender ──────────────────────

void VMDManager::update(float deltaTime) {
    if (!m_renderer) return;

    saba::MMDModel* model = m_renderer->getModel();
    if (!model) return;

    model->BeginAnimation();

    tickBlink(deltaTime);
    tickMouth(deltaTime);
    applyBlend(deltaTime);

    if (m_pauseActive) {
        // ── Waiting between loops (only "user" category) ──────────────────
        m_pauseTimer -= deltaTime;
        if (m_pauseTimer <= 0.f) {
            m_pauseActive = false;
            LOGI("Pause over — restarting [%s]", m_currentCategory.c_str());
            playCategory(m_currentCategory);
        }
        // Hold bind pose while paused so hair/cloth physics still simulate
        model->UpdateAllAnimation(nullptr, 0.f, deltaTime);

    } else if (m_currentAnim) {
        // ── Normal playback ───────────────────────────────────────────────
        m_animTime += deltaTime;
        if (m_animTime > m_animDuration && m_animDuration > 0.f) {
            startNextLoop();
        }
        model->UpdateAllAnimation(m_currentAnim.get(), m_animTime * 30.f, deltaTime);

    } else {
        // Bind pose
        model->UpdateAllAnimation(nullptr, 0.f, deltaTime);
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
    // "user" category: pause for USER_LOOP_PAUSE seconds before replaying.
    // All other categories loop immediately for a seamless feel.
    if (m_currentCategory == "user") {
        LOGI("[user] animation ended — pausing %.1fs before next loop", USER_LOOP_PAUSE);
        m_pauseActive = true;
        m_pauseTimer  = USER_LOOP_PAUSE;
        // Keep m_currentAnim so we know which category to restart
    } else {
        playCategory(m_currentCategory);
    }
}

void VMDManager::tickBlink(float dt) {
    if (!m_renderer) return;
    m_blinkTimer += dt;
    if (!m_blinking && m_blinkTimer >= m_blinkInterval) {
        m_blinking   = true;
        m_blinkPhase = 0.f;
        m_blinkTimer = 0.f;
        std::uniform_real_distribution<float> d(2.5f, 6.f);
        m_blinkInterval = d(m_rng);
    }
    if (m_blinking) {
        m_blinkPhase += dt;
        constexpr float H = 0.08f;
        float w = 0.f;
        if      (m_blinkPhase < H)     w = m_blinkPhase / H;
        else if (m_blinkPhase < H*2.f) w = 1.f - (m_blinkPhase - H) / H;
        else { w = 0.f; m_blinking = false; }
        m_renderer->setMorphWeight("\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", w);
        m_renderer->setMorphWeight("blink", w);
    }
}

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
