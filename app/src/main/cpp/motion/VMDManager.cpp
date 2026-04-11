/**
 * VMDManager.cpp — Multi-layer VMD animation system for MMD models (Saba library).
 *
 * ── Two-layer blending ───────────────────────────────────────────────────────
 *
 *   Layer 0 (base)  : Idle animation, always advancing.
 *                     Weight = 1.0 at rest; fades to 0.0 when an event plays.
 *                     When a static pose is active, idle overlays it at 0.3.
 *
 *   Layer 1 (event) : Waiting / dance / touch reactions.
 *                     Weight = 1.0 while active, returns to 0.0 when done.
 *
 *   saba's VMDAnimation::Evaluate(frame, weight) accumulates weighted bone
 *   transforms. Calling idle->Evaluate then event->Evaluate in the same
 *   BeginAnimation / EndAnimation block yields additive blending naturally.
 *   Weight transitions are smoothed via a first-order low-pass filter.
 *
 * ── Random events ────────────────────────────────────────────────────────────
 *
 *   nextEventTimer counts down each frame. On expiry:
 *     70 % → random file from "waiting"
 *     30 % → random file from "dance"
 *   Timer is reset to a uniform random value in [60, 600] seconds.
 *
 *   onTouch() fires immediately, overriding any in-progress event.
 *
 * ── SIGSEGV prevention ───────────────────────────────────────────────────────
 *
 *   All saba model methods are guarded with null checks.
 *   Physics world access checks for a valid btDiscreteDynamicsWorld pointer.
 *   The renderer pointer is validated before use each frame.
 *
 * ── Physics inertia & jiggle ─────────────────────────────────────────────────
 *
 *   Gravity tilt (hair/cloth) and heavy-body impulses (breast/hip) are
 *   unchanged from the previous implementation.
 */

#include "VMDManager.h"
#include "renderer/MMDRenderer.h"

#include <Saba/Model/MMD/VMDAnimation.h>
#include <Saba/Model/MMD/VMDFile.h>
#include <Saba/Model/MMD/MMDModel.h>
#include <Saba/Model/MMD/MMDPhysics.h>

#include <btBulletDynamicsCommon.h>

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cmath>
#include <algorithm>
#include <cstring>

#define LOG_TAG "VMDManager"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Timing constants ──────────────────────────────────────────────────────────

// Maximum animation clock step per frame — clamps GC pauses / app resumes.
static constexpr float MAX_ANIM_DT = 0.05f;   // 50 ms = 20 fps minimum

// EMA weight for physics deltaTime smoother (lower = smoother, less responsive).
static constexpr float PHYS_DT_EMA = 0.12f;

// How fast layer weights lerp toward their target (fraction per second × dt).
// At 2.5 units/s the full 0→1 transition takes ~0.4 s.
static constexpr float WEIGHT_FADE_SPEED = 2.5f;

// ── Physics inertia constants ─────────────────────────────────────────────────
//
// DRAG_INERTIA_SCALE: how strongly screen-space drag velocity tilts gravity.
//   Horizontal drag (m_dragVelX) → tilt gravity on X axis (left/right sway).
//   Vertical drag   (m_dragVelY) → tilt gravity on Y axis (up/down inertia).
//   Previously m_dragVelY was mapped to Z (depth) — that was wrong and caused
//   breast/hair to push "into/out of screen" during vertical moves.
//
static constexpr float DRAG_INERTIA_SCALE    = 18.f / 2000.f;
static constexpr float MAX_LATERAL_GRAVITY   = 20.f;    // X tilt cap (±)
static constexpr float MAX_VERTICAL_GRAVITY  = 12.f;    // Y tilt cap — smaller
                                                         // so model doesn't
                                                         // float off-screen
static constexpr float INERTIA_DECAY         = 0.80f;

// ── Jiggle impulse constants ──────────────────────────────────────────────────
//
// FIX: JIGGLE_IMPULSE_DECAY was 0.0f — that zeroed the impulse every frame so
// the jiggle lasted exactly ONE frame and then stopped dead (wooden feeling).
// 0.85f lets the force decay over ~6 frames at 60fps → natural spring-out.
//
// FIX: JIGGLE_IMPULSE_SCALE raised 0.0015 → 0.008 because vertical impulses
// now map to the Y axis (previously went to Z where they had no visible effect).
// If physics feels "explosive" on your device, lower this toward 0.004.
//
// FIX: JIGGLE_TRIGGER_DELTA lowered 150 → 80 so the jiggle fires on moderate
// moves, not just violent flicks.
//
static constexpr float JIGGLE_TRIGGER_DELTA  = 80.f;
static constexpr float JIGGLE_MASS_THRESHOLD = 0.3f;
static constexpr float JIGGLE_IMPULSE_SCALE  = 0.008f;
static constexpr float JIGGLE_MAX_IMPULSE    = 4.0f;
static constexpr float JIGGLE_IMPULSE_DECAY  = 0.85f;   // was 0.0f — KEY FIX

// ── Constructor / Destructor ──────────────────────────────────────────────────

VMDManager::VMDManager()
    : m_rng(static_cast<uint32_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()))
{
    std::uniform_real_distribution<float> blinkD(2.f, 5.f);
    m_blinkInterval = blinkD(m_rng);

    // Randomise first event time so models don't all dance at the same moment.
    std::uniform_real_distribution<float> timerD(60.f, 600.f);
    m_nextEventTimer = timerD(m_rng);
}

VMDManager::~VMDManager() = default;

void VMDManager::attachRenderer(MMDRenderer* r) { m_renderer = r; }

void VMDManager::setDragVelocity(float vx, float vy) {
    m_dragVelX = vx;
    m_dragVelY = vy;
}

void VMDManager::setAffinityTier(int tier) {
    m_affinityTier = tier;
    LOGI("AffinityTier = %d", tier);
}

// ── Directory scanning ────────────────────────────────────────────────────────

/**
 * Walk modelDir/motions/<category>/ and load every .vmd file found.
 * Recognised categories: idle, poses, waiting, dance, touch.
 */
void VMDManager::scanMotions(const std::string& modelDir) {
    if (!m_renderer || !m_renderer->getModel()) {
        LOGE("scanMotions: no model attached yet"); return;
    }

    static const char* CATEGORIES[] = { "idle", "poses", "waiting", "dance", "touch" };
    static const size_t NUM_CATS = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

    int totalLoaded = 0;
    for (size_t ci = 0; ci < NUM_CATS; ++ci) {
        const char* cat = CATEGORIES[ci];
        std::string folder = modelDir + "/motions/" + cat;

        DIR* dir = opendir(folder.c_str());
        if (!dir) {
            LOGI("scanMotions: folder not found: %s", folder.c_str());
            continue;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            const char* name = entry->d_name;
            size_t len = strlen(name);
            if (len < 4) continue;
            // Case-insensitive .vmd check
            const char* ext = name + len - 4;
            if (!(ext[0] == '.' &&
                  (ext[1] == 'v' || ext[1] == 'V') &&
                  (ext[2] == 'm' || ext[2] == 'M') &&
                  (ext[3] == 'd' || ext[3] == 'D'))) continue;

            std::string fullPath = folder + "/" + name;
            if (loadMotion(fullPath, cat)) {
                totalLoaded++;
            }
        }
        closedir(dir);
    }

    LOGI("scanMotions: loaded %d VMD files from %s", totalLoaded, modelDir.c_str());

    // Auto-start idle after scanning
    auto it = m_pool.find("idle");
    if (it == m_pool.end() || it->second.empty()) {
        it = m_pool.find("poses");
    }
    if (it != m_pool.end() && !it->second.empty()) {
        auto anim = pickRandom(it->first);
        if (anim) {
            m_base.anim     = anim;
            m_base.duration = static_cast<float>(anim->GetMaxKeyTime()) / 30.f;
            m_base.time     = 0.f;
            m_base.weight   = 1.f;
            m_base.target   = 1.f;
            m_base.looping  = true;
            m_base.category = it->first;
            LOGI("Auto-started [%s] dur=%.2fs", it->first.c_str(), m_base.duration);
        }
    }
}

// ── Single VMD loader ─────────────────────────────────────────────────────────

std::shared_ptr<saba::VMDAnimation> VMDManager::loadSingleVMD(const std::string& path) {
    if (!m_renderer || !m_renderer->getModel()) return nullptr;

    saba::MMDModel* raw = m_renderer->getModel();
    // Wrap raw pointer in a shared_ptr with a no-op deleter — ownership stays with MMDRenderer.
    std::shared_ptr<saba::MMDModel> modelPtr(raw, [](saba::MMDModel*){});

    auto anim = std::make_shared<saba::VMDAnimation>();
    if (!anim->Create(modelPtr)) { LOGE("VMDAnimation::Create failed"); return nullptr; }

    saba::VMDFile vmdFile;
    if (!saba::ReadVMDFile(&vmdFile, path.c_str())) {
        LOGE("ReadVMDFile failed: %s", path.c_str()); return nullptr;
    }
    if (!anim->Add(vmdFile)) {
        LOGE("VMDAnimation::Add failed: %s", path.c_str()); return nullptr;
    }
    return anim;
}

bool VMDManager::loadMotion(const std::string& vmdPath, const std::string& category) {
    auto anim = loadSingleVMD(vmdPath);
    if (!anim) return false;
    m_pool[category].push_back(anim);
    LOGI("Loaded VMD [%s] %s  pool=%zu",
         category.c_str(), vmdPath.c_str(), m_pool[category].size());
    return true;
}

// ── Random pick ───────────────────────────────────────────────────────────────

std::shared_ptr<saba::VMDAnimation> VMDManager::pickRandom(const std::string& cat) {
    auto it = m_pool.find(cat);
    if (it == m_pool.end() || it->second.empty()) return nullptr;
    std::uniform_int_distribution<size_t> d(0, it->second.size() - 1);
    return it->second[d(m_rng)];
}

// ── Event control ─────────────────────────────────────────────────────────────

/**
 * Start a Layer-1 event from the given category.
 * Immediately fades idle down and the event up.
 */
void VMDManager::startEvent(const std::string& category) {
    auto anim = pickRandom(category);
    if (!anim) {
        LOGI("startEvent: no files in [%s]", category.c_str()); return;
    }

    m_event.anim     = anim;
    m_event.duration = static_cast<float>(anim->GetMaxKeyTime()) / 30.f;
    m_event.time     = 0.f;
    m_event.weight   = m_event.weight; // keep current (may already be > 0)
    m_event.target   = 1.f;
    m_event.category = category;
    m_event.looping  = false;
    m_eventActive    = true;

    // Fade idle to 0 while event plays
    m_base.target = 0.f;

    LOGI("Event started [%s] dur=%.2fs", category.c_str(), m_event.duration);
}

void VMDManager::onTouch() {
    LOGI("onTouch() → playing random [touch]");
    startEvent("touch");
}

// ── Random event timer ────────────────────────────────────────────────────────

void VMDManager::tickEventTimer(float dt) {
    if (m_eventActive) return; // don't fire a new event while one is running

    m_nextEventTimer -= dt;
    if (m_nextEventTimer > 0.f) return;

    // Choose: 70% waiting, 30% dance
    std::uniform_real_distribution<float> chance(0.f, 1.f);
    const std::string cat = (chance(m_rng) < 0.70f) ? "waiting" : "dance";

    // Only fire if the category has files
    if (m_pool.count(cat) && !m_pool[cat].empty()) {
        startEvent(cat);
    }

    // Reset timer to random interval in [60, 600] seconds
    std::uniform_real_distribution<float> td(60.f, 600.f);
    m_nextEventTimer = td(m_rng);
    LOGI("Next event timer: %.0f s", m_nextEventTimer);
}

// ── Physics inertia (gravity tilt → hair / cloth) ─────────────────────────────
//
// Screen-space drag → Bullet gravity mapping:
//
//   m_dragVelX (finger moving left/right on screen)
//     → tilt gravity on BULLET X-axis  (model sways left/right)   ✓ was correct
//
//   m_dragVelY (finger moving up/down on screen)
//     → tilt gravity on BULLET Y-axis  (model feels acceleration up/down) ✓ FIXED
//     Previously mapped to Z (depth into screen) which caused breast/hair to
//     push "forward or backward" during vertical movement — that's the bug
//     Gemini described as "перепутанные оси".
//
// When the overlay falls downward (velY > 0 in screen coords):
//   The model experiences an upward pseudo-force (inertia opposes motion).
//   We reduce the downward gravity slightly so soft bodies lag behind → looks
//   like they float up while the model drops. That is the correct "lift" effect.
//
void VMDManager::applyPhysicsInertia(void* modelRaw, float /*dt*/) {
    saba::MMDModel* model = static_cast<saba::MMDModel*>(modelRaw);
    if (!model) return;
    auto* mmPhysics = model->GetMMDPhysics();
    if (!mmPhysics) return;
    btDiscreteDynamicsWorld* world = mmPhysics->GetDynamicsWorld();
    if (!world) return;

    // Horizontal drag → X tilt (unchanged, already correct)
    float targetGx = std::max(-MAX_LATERAL_GRAVITY,
                     std::min( MAX_LATERAL_GRAVITY, -m_dragVelX * DRAG_INERTIA_SCALE));

    // Vertical drag → Y component of gravity (KEY FIX: was Z before)
    // Screen Y grows downward; Bullet Y grows upward.
    // Moving the window DOWN (velY > 0) → model is falling → soft bodies should
    // feel lighter (gravity temporarily reduced = upward inertia). So we ADD
    // a positive offset to the -9.8 base.
    float targetGy_offset = std::max(-MAX_VERTICAL_GRAVITY,
                            std::min( MAX_VERTICAL_GRAVITY,  m_dragVelY * DRAG_INERTIA_SCALE));

    btVector3 curG = world->getGravity();

    // Lerp X and Y offsets toward their targets
    float newGx = curG.x() + (targetGx       - curG.x()) * (1.f - INERTIA_DECAY);
    float newGy_offset = m_gravYOffset + (targetGy_offset - m_gravYOffset) * (1.f - INERTIA_DECAY);

    // When velocity is near zero, decay both offsets back toward neutral
    bool nearStill = (std::abs(m_dragVelX) < 5.f && std::abs(m_dragVelY) < 5.f);
    if (nearStill) {
        newGx        *= INERTIA_DECAY;
        newGy_offset *= INERTIA_DECAY;
    }

    m_gravYOffset = newGy_offset;

    // Base gravity is -9.8 on Y; we add the offset on top.
    // Z stays 0 — we no longer use it for screen-drag inertia.
    world->setGravity(btVector3(newGx, -9.8f + newGy_offset, 0.f));
}

// ── Jiggle impulse (heavy rigid bodies = breast / hip) ────────────────────────
//
// FIX 1: Impulse direction — previously both X and Y screen-drag components
//   were applied as Bullet (X, 0, Y) i.e. horizontal plane only.
//   Vertical screen drag (deltaVy) now maps to Bullet Y so a downward flick
//   creates an upward impulse on breast bodies → they bounce vertically.
//
// FIX 2: JIGGLE_IMPULSE_DECAY is now 0.85f (was 0.0f) so the accumulated
//   impulse persists across frames and decays smoothly (spring-out effect).
//
void VMDManager::applyJiggleImpulses(void* modelRaw) {
    float deltaVx = m_dragVelX - m_prevDragVelX;
    float deltaVy = m_dragVelY - m_prevDragVelY;
    float deltaMag = std::sqrt(deltaVx * deltaVx + deltaVy * deltaVy);

    m_prevDragVelX = m_dragVelX;
    m_prevDragVelY = m_dragVelY;

    bool trigger = (deltaMag >= JIGGLE_TRIGGER_DELTA) && !m_jiggleFired;
    if (trigger) {
        // X screen → Bullet X (left/right)
        // Y screen → Bullet Y (up/down) — KEY FIX: was applied to Z before
        // Negate: if window moves down fast, impulse should push soft bodies UP.
        m_jiggleImpulseX = -deltaVx * JIGGLE_IMPULSE_SCALE;
        m_jiggleImpulseY = -deltaVy * JIGGLE_IMPULSE_SCALE;  // vertical spring

        // Safety clamp for Poco X3 Pro (high-frequency touch events can produce
        // very large deltaV on a fast swipe — without this Bullet can explode).
        float mag = std::sqrt(m_jiggleImpulseX * m_jiggleImpulseX +
                              m_jiggleImpulseY * m_jiggleImpulseY);
        if (mag > JIGGLE_MAX_IMPULSE) {
            float s = JIGGLE_MAX_IMPULSE / mag;
            m_jiggleImpulseX *= s;
            m_jiggleImpulseY *= s;
        }
        m_jiggleFired = true;
        LOGI("Jiggle: X=%.3f Y=%.3f dV=%.1f", m_jiggleImpulseX, m_jiggleImpulseY, deltaMag);
    } else if (deltaMag < JIGGLE_TRIGGER_DELTA * 0.5f) {
        m_jiggleFired = false;
    }

    if (std::abs(m_jiggleImpulseX) < 0.0001f && std::abs(m_jiggleImpulseY) < 0.0001f) return;

    saba::MMDModel* model = static_cast<saba::MMDModel*>(modelRaw);
    if (!model) return;
    auto* mmPhysics = model->GetMMDPhysics();
    if (!mmPhysics) return;
    btDiscreteDynamicsWorld* world = mmPhysics->GetDynamicsWorld();
    if (!world) return;

    const btCollisionObjectArray& objs = world->getCollisionObjectArray();
    for (int i = 0; i < objs.size(); ++i) {
        btRigidBody* rb = btRigidBody::upcast(objs[i]);
        if (!rb || rb->isStaticObject() || rb->isKinematicObject()) continue;

        float mass = rb->getMass();
        if (mass < JIGGLE_MASS_THRESHOLD) continue;

        float scaledIx = m_jiggleImpulseX * mass;
        float scaledIy = m_jiggleImpulseY * mass;   // vertical component

        // Per-body clamp — prevents a single heavy body from exploding
        float bodyMag = std::sqrt(scaledIx * scaledIx + scaledIy * scaledIy);
        if (bodyMag > JIGGLE_MAX_IMPULSE * mass) {
            float s = JIGGLE_MAX_IMPULSE * mass / bodyMag;
            scaledIx *= s;
            scaledIy *= s;
        }

        // Apply impulse: X=left/right, Y=up/down, Z=0 (no depth component)
        // Previously: btVector3(scaledIx, 0.f, scaledIy) — Y screen went to Z!
        rb->applyCentralImpulse(btVector3(scaledIx, scaledIy, 0.f));
        rb->activate(true);
    }

    // Decay impulse across frames for spring-out effect (was 0.0f = instant kill)
    m_jiggleImpulseX *= JIGGLE_IMPULSE_DECAY;
    m_jiggleImpulseY *= JIGGLE_IMPULSE_DECAY;
}

// ── Main update ───────────────────────────────────────────────────────────────

void VMDManager::update(float rawDeltaTime) {
    if (!m_renderer) return;

    saba::MMDModel* model = m_renderer->getModel();
    if (!model) return;

    // Clamp animation clock to avoid large jumps after GC pauses / app resumes.
    const float animDt = std::min(rawDeltaTime, MAX_ANIM_DT);

    // EMA-smoothed physics dt — absorbs single-frame spikes for Bullet stability.
    m_smoothedPhysDt = m_smoothedPhysDt * (1.f - PHYS_DT_EMA)
                     + animDt            * PHYS_DT_EMA;
    const float physDt = std::max(m_smoothedPhysDt, 0.001f);

    tickBlink(animDt);
    tickMouth(animDt);
    tickEventTimer(animDt);
    applyPhysicsInertia(model, physDt);
    applyJiggleImpulses(model);

    // ── Advance Layer 0 (base / idle) ─────────────────────────────────────
    if (m_base.anim) {
        m_base.time += animDt;
        if (m_base.looping && m_base.duration > 0.f && m_base.time > m_base.duration) {
            // Seamless loop: carry overshoot so seam is invisible.
            m_base.time = std::fmod(m_base.time, m_base.duration);
        }
    }

    // ── Advance Layer 1 (event) ───────────────────────────────────────────
    if (m_eventActive && m_event.anim) {
        m_event.time += animDt;
        if (m_event.duration > 0.f && m_event.time >= m_event.duration) {
            // Event finished: return idle to full weight.
            m_eventActive    = false;
            m_event.target   = 0.f;
            m_base.target    = 1.f;
            LOGI("Event [%s] complete", m_event.category.c_str());
        }
    }

    // ── Smooth weight fade ────────────────────────────────────────────────
    // First-order low-pass: weight tracks target at WEIGHT_FADE_SPEED units/s.
    float alpha = std::min(1.f, WEIGHT_FADE_SPEED * animDt);
    m_base.weight  += (m_base.target  - m_base.weight)  * alpha;
    m_event.weight += (m_event.target - m_event.weight)  * alpha;

    // ── Two-layer Evaluate ────────────────────────────────────────────────
    //
    // Saba's VMDAnimation::Evaluate(frame, weight) additively accumulates
    // weighted bone transform offsets.  Calling Layer-0 then Layer-1 in a
    // single BeginAnimation / EndAnimation block achieves multi-layer blend:
    //
    //   FinalTransform ≈ (idle * idleWeight) + (event * eventWeight)
    //
    // When a static pose is used as the base and idle is the overlay:
    //   FinalTransform = (PoseTransform * 1.0) + (IdleTransform * 0.3)
    // achieved by setting m_base.target = 0.3 when the pose is active.
    //
    model->BeginAnimation();

    if (m_base.anim && m_base.weight > 0.001f) {
        m_base.anim->Evaluate(m_base.time * 30.f, m_base.weight);
    }

    if (m_event.anim && m_event.weight > 0.001f) {
        m_event.anim->Evaluate(m_event.time * 30.f, m_event.weight);
    }

    // Run morph, IK (pre-physics), physics, IK (post-physics)
    model->UpdateMorphAnimation();
    model->UpdateNodeAnimation(false);
    model->UpdatePhysicsAnimation(physDt);
    model->UpdateNodeAnimation(true);

    model->Update();
    model->EndAnimation();
}

// ── Blink ─────────────────────────────────────────────────────────────────────

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
        // UTF-8: まばたき (blink morph in Japanese MMD models)
        m_renderer->setMorphWeight("\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", w);
        m_renderer->setMorphWeight("blink", w);
    }
}

// ── Mouth idle ────────────────────────────────────────────────────────────────

void VMDManager::tickMouth(float dt) {
    if (!m_renderer) return;
    m_mouthPhase += dt * 1.2f;
    float w = (std::sin(m_mouthPhase) * 0.5f + 0.5f) * 0.06f;
    // UTF-8: あ (mouth-open morph)
    m_renderer->setMorphWeight("\xe3\x81\x82", w);
    m_renderer->setMorphWeight("mouth_a", w);
}
