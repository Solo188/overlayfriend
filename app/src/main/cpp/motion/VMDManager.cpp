/**
 * VMDManager.cpp
 *
 * Correct Saba animation sequence per frame:
 *   model->BeginAnimation()
 *   model->UpdateAllAnimation(anim, frame, dt)   <- bones/IK/physics
 *   model->Update()                              <- SKIN MESH recalculation
 *   model->EndAnimation()
 *
 * Omitting Update() means GetUpdatePositions() returns stale/zero data
 * and the model is invisible (all vertices at origin = black screen).
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
        LOGI("No motions in category [%s]", category.c_str());
        return;
    }
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

    // ── Step 1: BeginAnimation — resets morph/node deltas for this frame ─
    model->BeginAnimation();

    // ── Step 2: Procedural morphs (blink, mouth) ─────────────────────────
    tickBlink(deltaTime);
    tickMouth(deltaTime);
    applyBlend(deltaTime);

    // ── Step 3: UpdateAllAnimation — evaluates VMD keys, IK, physics ─────
    if (m_currentAnim) {
        m_animTime += deltaTime;
        if (m_animTime > m_animDuration && m_animDuration > 0.f)
            startNextLoop();
        model->UpdateAllAnimation(m_currentAnim.get(), m_animTime * 30.f, deltaTime);
    } else {
        // Bind pose — still need to run physics for hair/cloth simulation
        model->UpdateAllAnimation(nullptr, 0.f, deltaTime);
    }

    // ── Step 4: Update — recalculates skin mesh into GetUpdatePositions() ─
    // THIS IS THE CRITICAL CALL. Without it, GetUpdatePositions() returns
    // stale/zero data and the model renders as an invisible point → BLACK SCREEN.
    model->Update();

    // ── Step 5: EndAnimation — finalises node transforms ─────────────────
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

void VMDManager::startNextLoop() { playCategory(m_currentCategory); }

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
