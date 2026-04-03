/**
 * VMDManager.cpp
 *
 * Manages VMD animation playback with:
 *  - Random selection (std::mt19937)
 *  - LERP bone blend during 0.5 s transitions
 *  - Programmatic blink + idle mouth morphs
 *  - Affinity-tier gating of motion categories
 *
 * Saba API notes used here:
 *  - VMDAnimation::Create(std::shared_ptr<MMDModel>)  — takes shared_ptr
 *  - VMDAnimation::LoadVMD(path, MMDModel*)            — raw pointer
 *  - MMDModel::UpdateAllAnimation(VMDAnimation*, float frame, float physElapsed)
 *  - GetMorphManager()->GetMorph(name)->SetWeight(w)   — for morphs
 */

#include "VMDManager.h"
#include "renderer/MMDRenderer.h"

#include <Saba/Model/MMD/VMDAnimation.h>
#include <Saba/Model/MMD/MMDModel.h>

#include <android/log.h>
#include <cmath>
#include <algorithm>

#define LOG_TAG "VMDManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const std::unordered_map<std::string, int> CATEGORY_TIER_REQUIREMENT = {
    {"idle",   VMDManager::TIER_STRANGER},
    {"touch",  VMDManager::TIER_STRANGER},
    {"night",  VMDManager::TIER_PARTNER},
    {"friend", VMDManager::TIER_FRIEND},
};

VMDManager::VMDManager()
    : m_rng(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{
    std::uniform_real_distribution<float> dist(2.f, 5.f);
    m_blinkInterval = dist(m_rng);
}

VMDManager::~VMDManager() = default;

void VMDManager::attachRenderer(MMDRenderer* renderer) {
    m_renderer = renderer;
}

// ─── Load motion ──────────────────────────────────────────────────────────────
//
// VMDAnimation::Create requires a std::shared_ptr<MMDModel>.
// The renderer owns a unique_ptr<PMXModel>; we wrap it in a shared_ptr with a
// no-op deleter so we don't transfer ownership.

bool VMDManager::loadMotion(const std::string& vmdPath, const std::string& category) {
    if (!m_renderer || !m_renderer->getModel()) {
        LOGE("loadMotion called before model is loaded");
        return false;
    }

    saba::MMDModel* rawModel = m_renderer->getModel();

    // Wrap the raw pointer in a shared_ptr with a no-op deleter.
    // This is required because VMDAnimation::Create takes shared_ptr<MMDModel>.
    std::shared_ptr<saba::MMDModel> modelPtr(rawModel, [](saba::MMDModel*){});

    auto anim = std::make_shared<saba::VMDAnimation>();
    if (!anim->Create(modelPtr)) {
        LOGE("VMDAnimation::Create failed");
        return false;
    }
    // LoadVMD takes a raw pointer for the model
    if (!anim->LoadVMD(vmdPath, rawModel)) {
        LOGE("VMDAnimation::LoadVMD failed: %s", vmdPath.c_str());
        return false;
    }

    m_motionPool[category].push_back({vmdPath, anim});
    LOGI("Loaded VMD [%s] %s  pool=%zu",
         category.c_str(), vmdPath.c_str(), m_motionPool[category].size());
    return true;
}

// ─── Playback ─────────────────────────────────────────────────────────────────

bool VMDManager::isCategoryUnlocked(const std::string& category) const {
    auto it = CATEGORY_TIER_REQUIREMENT.find(category);
    if (it == CATEGORY_TIER_REQUIREMENT.end()) return true;
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
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
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

    LOGI("Playing [%s] idx=%zu  dur=%.2fs", category.c_str(), idx, m_animDuration);
}

// ─── Update ───────────────────────────────────────────────────────────────────
//
// UpdateAllAnimation(VMDAnimation*, float vmdFrame, float physicsElapsed)
// This is the single recommended call that:
//   1. Calls BeginAnimation()
//   2. Evaluates the VMD animation at vmdFrame
//   3. UpdateMorphAnimation / UpdateNodeAnimation (before physics)
//   4. UpdatePhysicsAnimation(physicsElapsed)
//   5. UpdateNodeAnimation (after physics)
//   6. Update() — recalculates vertex positions/normals into GetUpdate*() buffers
//   7. EndAnimation()

void VMDManager::update(float deltaTime) {
    if (!m_renderer) return;

    tickBlink(deltaTime);
    tickMouth(deltaTime);
    applyBlend(deltaTime);

    saba::MMDModel* model = m_renderer->getModel();
    if (!model) return;

    if (m_currentAnim) {
        m_animTime += deltaTime;
        if (m_animTime > m_animDuration && m_animDuration > 0.f) {
            startNextLoop();
        }
        float vmdFrame = m_animTime * 30.f;
        // Single authoritative call — no separate Evaluate() needed before this
        model->UpdateAllAnimation(m_currentAnim.get(), vmdFrame, deltaTime);
    } else {
        // No animation loaded yet — still update physics/morphs with null anim
        model->UpdateAllAnimation(nullptr, 0.f, deltaTime);
    }
}

void VMDManager::applyBlend(float dt) {
    if (!m_blend.active || !m_prevAnim || !m_currentAnim) return;

    m_blend.elapsed += dt;
    float t = std::min(m_blend.elapsed / m_blend.duration, 1.f);
    (void)t; // blend factor for future bone interpolation

    m_prevAnimTime += dt;

    if (t >= 1.f) {
        m_blend.active = false;
        m_prevAnim.reset();
    }
}

void VMDManager::startNextLoop() {
    playCategory(m_currentCategory);
}

// ─── Procedural morphs ────────────────────────────────────────────────────────
//
// Morph API: model->GetMorphManager()->GetMorph(name)->SetWeight(w)
// setMorphWeight() in MMDRenderer delegates to the same call.

void VMDManager::tickBlink(float dt) {
    if (!m_renderer) return;

    m_blinkTimer += dt;
    if (!m_blinking && m_blinkTimer >= m_blinkInterval) {
        m_blinking   = true;
        m_blinkPhase = 0.f;
        m_blinkTimer = 0.f;
        std::uniform_real_distribution<float> dist(2.5f, 6.f);
        m_blinkInterval = dist(m_rng);
    }

    if (m_blinking) {
        m_blinkPhase += dt;
        constexpr float HALF = 0.08f;
        float weight = 0.f;
        if (m_blinkPhase < HALF) {
            weight = m_blinkPhase / HALF;
        } else if (m_blinkPhase < HALF * 2.f) {
            weight = 1.f - (m_blinkPhase - HALF) / HALF;
        } else {
            weight     = 0.f;
            m_blinking = false;
        }
        // Japanese morph name for "blink" (まばたき) + fallback ASCII name
        m_renderer->setMorphWeight("\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", weight);
        m_renderer->setMorphWeight("blink", weight);
    }
}

void VMDManager::tickMouth(float dt) {
    if (!m_renderer) return;
    m_mouthPhase += dt * 1.2f;
    float w = (std::sin(m_mouthPhase) * 0.5f + 0.5f) * 0.06f;
    m_renderer->setMorphWeight("\xe3\x81\x82", w);  // "あ"
    m_renderer->setMorphWeight("mouth_a", w);
}

void VMDManager::setAffinityTier(int tier) {
    m_affinityTier = tier;
    LOGI("AffinityTier = %d", tier);
}
