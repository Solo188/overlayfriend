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
// DRAG_INERTIA_SCALE   — screen-velocity → gravity-tilt multiplier.
// MAX_LATERAL_GRAVITY  — X-axis tilt cap (left/right hair/cloth sway).
// MAX_VERTICAL_INERTIA — Y-axis offset cap; smaller than X because large
//                        offsets to -9.8 base gravity make bodies visibly float.
// INERTIA_DECAY        — EMA snap-back speed (higher = snappier return).
//
static constexpr float DRAG_INERTIA_SCALE    = 18.f / 2000.f;
static constexpr float MAX_LATERAL_GRAVITY   = 18.f;
static constexpr float MAX_VERTICAL_INERTIA  = 8.f;
static constexpr float INERTIA_DECAY         = 0.82f;

// ── High-Damping Mass-Spring jiggle constants ─────────────────────────────────
//
// JIGGLE_TRIGGER_DELTA  — min velocity-change (px/s) to fire an impulse.
//                         60 = reacts to subtle moves; damping prevents wildness.
// JIGGLE_MASS_THRESHOLD — only bodies >= this mass receive impulses.
//                         Keeps light hair joints stable; breast/hip bodies react.
// JIGGLE_IMPULSE_SCALE  — raw impulse scale before mass multiplication.
//                         Reduced vs old value; mass influence compensates.
// JIGGLE_MAX_IMPULSE    — per-body hard cap; prevents Bullet explosions on
//                         fast swipes (Poco X3 Pro high-frequency touch events).
// JIGGLE_IMPULSE_DECAY  — frame-over-frame impulse persistence (0.88 ≈ 8 frames
//                         to 1/e at 60fps) — the "spring-out / memory" effect.
// LINEAR_DAMPING        — applied directly to each qualifying rigid body.
//                         0.95 simulates soft tissue resistance; body moves but
//                         damps quickly instead of bouncing forever (no "jelly").
// ANGULAR_DAMPING       — same principle for rotation.
// MAX_VELOCITY_OFFSET   — caps the velocity correction in applyPhysicsInertia
//                         to prevent the "tearing from body" artefact.
//
static constexpr float JIGGLE_TRIGGER_DELTA  = 60.f;
static constexpr float JIGGLE_MASS_THRESHOLD = 0.3f;
static constexpr float JIGGLE_IMPULSE_SCALE  = 0.012f;
static constexpr float JIGGLE_MAX_IMPULSE    = 5.0f;
static constexpr float JIGGLE_IMPULSE_DECAY  = 0.88f;
static constexpr float LINEAR_DAMPING        = 0.95f;
static constexpr float ANGULAR_DAMPING       = 0.95f;
static constexpr float MAX_VELOCITY_OFFSET   = 4.0f;

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

// ── Physics inertia (gravity tilt + velocity offset → hair / cloth) ──────────
//
// Two-part inertia system:
//
//   Part 1 — Gravity tilt (unchanged from before):
//     Horizontal drag tilts gravity on X → hair/cloth sways left/right.
//
//   Part 2 — Vertical velocity offset (NEW, replaces raw Y force):
//     Instead of adding a raw force on Y every frame (which causes the
//     "tearing from body" artefact when the window is dragged fast), we
//     compute a Target Velocity Offset and lerp each body's current velocity
//     toward it.  This is a soft constraint — bodies move toward the target
//     but are never teleported, so the mesh cannot stretch beyond realistic
//     limits.  MAX_VELOCITY_OFFSET clamps the correction so even extreme
//     drag speeds don't cause visible pop.
//
//   Screen-Y inversion note:
//     Screen Y grows downward; Bullet World Y grows upward.
//     When window falls  (dragVelY > 0): bodies should feel upward inertia
//     → target velocity offset = +value on Bullet Y.
//     When window rises  (dragVelY < 0): bodies feel downward inertia
//     → target velocity offset = -value on Bullet Y.
//     Formula: targetVelY = -dragVelY * scale  (negate to invert screen→world)
//
void VMDManager::applyPhysicsInertia(void* modelRaw, float /*dt*/) {
    saba::MMDModel* model = static_cast<saba::MMDModel*>(modelRaw);
    if (!model) return;
    auto* mmPhysics = model->GetMMDPhysics();
    if (!mmPhysics) return;
    btDiscreteDynamicsWorld* world = mmPhysics->GetDynamicsWorld();
    if (!world) return;

    // ── Part 1: X gravity tilt (horizontal drag → lateral sway) ──────────
    float targetGx = std::max(-MAX_LATERAL_GRAVITY,
                     std::min( MAX_LATERAL_GRAVITY, -m_dragVelX * DRAG_INERTIA_SCALE));

    btVector3 curG = world->getGravity();
    float newGx = curG.x() + (targetGx - curG.x()) * (1.f - INERTIA_DECAY);

    bool nearStill = (std::abs(m_dragVelX) < 5.f && std::abs(m_dragVelY) < 5.f);
    if (nearStill) newGx *= INERTIA_DECAY;

    // Keep Y at base -9.8; Z stays 0.  Vertical inertia is handled below.
    world->setGravity(btVector3(newGx, -9.8f, 0.f));

    // ── Part 2: Y velocity offset (vertical drag → up/down inertia) ──────
    // Compute target Y velocity: invert screen-Y so falling window = upward felt force.
    float rawTargetVy = -m_dragVelY * DRAG_INERTIA_SCALE * 300.f; // scale px/s → m/s range
    float targetVy    = std::max(-MAX_VELOCITY_OFFSET,
                        std::min( MAX_VELOCITY_OFFSET, rawTargetVy));

    if (nearStill) targetVy = 0.f;

    const btCollisionObjectArray& objs = world->getCollisionObjectArray();
    for (int i = 0; i < objs.size(); ++i) {
        btRigidBody* rb = btRigidBody::upcast(objs[i]);
        if (!rb || rb->isStaticObject() || rb->isKinematicObject()) continue;
        if (rb->getMass() < JIGGLE_MASS_THRESHOLD) continue;

        // Wake the body — Bullet deactivates idle bodies to save CPU.
        // Without this, subtle velocity changes are silently ignored.
        rb->activate(true);

        // Lerp current Y velocity toward the target offset.
        // We only touch Y; X and Z are left to Bullet.
        btVector3 vel = rb->getLinearVelocity();
        float newVy   = vel.y() + (targetVy - vel.y()) * (1.f - INERTIA_DECAY);

        // Displacement clamp: prevent stretching beyond realistic limits.
        // If the corrected velocity would move the body more than MAX_VELOCITY_OFFSET
        // m/s, clamp it back.  This is the soft constraint that replaces raw force.
        newVy = std::max(-MAX_VELOCITY_OFFSET, std::min(MAX_VELOCITY_OFFSET, newVy));

        rb->setLinearVelocity(btVector3(vel.x(), newVy, vel.z()));
    }
}

// ── Jiggle impulse — High-Damping Mass-Spring system ─────────────────────────
//
// Overview:
//   1. Detect a sudden velocity change (jerk) exceeding JIGGLE_TRIGGER_DELTA.
//   2. Compute an impulse proportional to the jerk, scaled by body mass
//      (heavier bodies = larger impulse so they overcome animation constraints).
//   3. Apply LINEAR_DAMPING / ANGULAR_DAMPING to each qualifying body ONCE on
//      first trigger — this makes them behave like soft tissue instead of
//      bouncing elastic balls ("jelly" prevention).
//   4. Wake every qualifying body with activate(true) EVERY time m_jiggleFired
//      is true — Bullet's sleep system silently ignores forces on sleeping bodies,
//      which was the cause of "subtle jiggles are invisible" complaints.
//   5. Decay the accumulated impulse at JIGGLE_IMPULSE_DECAY per frame for
//      smooth spring-out rather than abrupt cutoff.
//
// Axis mapping (screen → Bullet):
//   deltaVx (finger left/right) → Bullet X impulse  (negate: move right → push left)
//   deltaVy (finger up/down)    → Bullet Y impulse  (negate: move down → push up)
//   Z is always 0 — no depth component from 2D drag.
//
void VMDManager::applyJiggleImpulses(void* modelRaw) {
    float deltaVx  = m_dragVelX - m_prevDragVelX;
    float deltaVy  = m_dragVelY - m_prevDragVelY;
    float deltaMag = std::sqrt(deltaVx * deltaVx + deltaVy * deltaVy);

    m_prevDragVelX = m_dragVelX;
    m_prevDragVelY = m_dragVelY;

    bool trigger = (deltaMag >= JIGGLE_TRIGGER_DELTA) && !m_jiggleFired;
    if (trigger) {
        m_jiggleImpulseX = -deltaVx * JIGGLE_IMPULSE_SCALE;
        m_jiggleImpulseY = -deltaVy * JIGGLE_IMPULSE_SCALE;   // screen-Y inverted → world-Y

        float mag = std::sqrt(m_jiggleImpulseX * m_jiggleImpulseX +
                              m_jiggleImpulseY * m_jiggleImpulseY);
        if (mag > JIGGLE_MAX_IMPULSE) {
            float s = JIGGLE_MAX_IMPULSE / mag;
            m_jiggleImpulseX *= s;
            m_jiggleImpulseY *= s;
        }
        m_jiggleFired = true;
        LOGI("Jiggle fired: IX=%.3f IY=%.3f dV=%.1f", m_jiggleImpulseX, m_jiggleImpulseY, deltaMag);
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

        // ── Step 2: Wake the body unconditionally ─────────────────────────
        // DISABLE_DEACTIVATION is set in update() Step 0b, but activate(true)
        // here is an extra guarantee for bodies that may have been reset by
        // Saba's ResetPhysics between frames.
        rb->activate(true);

        // ── Step 3: Mass-scaled impulse ───────────────────────────────────
        // Heavier bodies (breast, hip in PMX) need a proportionally larger
        // impulse to overcome their animation pose offset and move visibly.
        // Light bodies (hair ribbons, small accessories) get the base impulse.
        float massInfluence = 1.0f + mass * 0.8f;   // heavier → stronger reaction
        float scaledIx = m_jiggleImpulseX * massInfluence;
        float scaledIy = m_jiggleImpulseY * massInfluence;

        // Per-body hard cap — prevents any single body from exploding
        float bodyMag = std::sqrt(scaledIx * scaledIx + scaledIy * scaledIy);
        if (bodyMag > JIGGLE_MAX_IMPULSE) {
            float s = JIGGLE_MAX_IMPULSE / bodyMag;
            scaledIx *= s;
            scaledIy *= s;
        }

        // Apply: X=left/right, Y=up/down (screen-Y already negated above), Z=0
        rb->applyCentralImpulse(btVector3(scaledIx, scaledIy, 0.f));
    }

    // ── Step 4: Decay impulse for spring-out effect ───────────────────────
    // 0.88^60 ≈ 0.0006 → impulse reaches noise floor in ~1 second at 60fps.
    m_jiggleImpulseX *= JIGGLE_IMPULSE_DECAY;
    m_jiggleImpulseY *= JIGGLE_IMPULSE_DECAY;
}

// ── Main update — Animation → Force Injection → Physics → Bone Sync ──────────
//
// Pipeline (strict order, each step depends on the previous):
//
//   Step 0  Advance timers, layer weights, morph/blink
//   Step 1  Evaluate VMD keyframes → bone local transforms (BeginAnimation …
//             UpdateMorphAnimation, Evaluate layers)
//   Step 2  Pre-physics bone update: UpdateNodeAnimation(false)
//             Propagates VMD result into global bone matrices for bones that
//             are NOT driven after physics (IsDeformAfterPhysics == false).
//             Kinematic bodies read these matrices to place themselves.
//   Step 3  Force injection window — applyPhysicsInertia + applyJiggleImpulses.
//             This is the ONLY correct moment: bones are positioned by VMD,
//             but Bullet hasn't stepped yet.  Forces applied here survive the
//             upcoming stepSimulation call intact.
//   Step 4  Physics solve — model->UpdatePhysicsAnimation(physDt).
//             Internally: SetActivation(true) on all MMDRigidBody objects
//             (switches Dynamic bodies to their active MotionState so Bullet
//             drives them instead of VMD keyframes), calls
//             physics->Update(physDt) → stepSimulation, then
//             ReflectGlobalTransform + CalcLocalTransform to write Bullet
//             results back into MMDNode global matrices.
//   Step 5  Post-physics bone update: UpdateNodeAnimation(true)
//             Propagates physics results into bones flagged
//             IsDeformAfterPhysics == true (hair, cloth, skirt chains).
//   Step 6  model->Update() — compute final skinning matrices.
//   Step 7  EndAnimation()
//
// Physics-priority guarantee:
//   Dynamic MMDRigidBody objects (breast/hair/skirt in PMX) use
//   DynamicMotionState / DynamicAndBoneMergeMotionState.  When
//   SetActivation(true) is called in Step 4, Bullet removes the
//   CF_KINEMATIC_OBJECT flag and switches to the active MotionState —
//   after that, the rigid body's world transform is driven 100% by
//   Bullet's simulation result.  VMD keyframes for those bones are
//   loaded in Step 1–2 but then overwritten in Step 4–5 by physics.
//   So we do NOT need a separate "ignore VMD for physics bones" loop —
//   Saba already implements this through the MotionState pattern.
//
// Substep stabilisation:
//   We configure MMDPhysics before the first solve to use fixed 60Hz
//   substeps with up to 4 substeps per frame.  This prevents jitter
//   during heavy dance animations where the animation clock and the
//   physics clock can diverge by up to 2× (e.g. GC pause → large dt).
//
void VMDManager::update(float rawDeltaTime) {
    if (!m_renderer) return;

    saba::MMDModel* model = m_renderer->getModel();
    if (!model) return;

    // ── Step 0a: Configure physics substep on first call ─────────────────
    // SetFPS / SetMaxSubStepCount are cheap no-ops after the first frame.
    // 60 Hz fixed substep, max 4 substeps = handles up to 4× frame-time
    // spikes (GC pauses, activity resume) without physics explosion.
    {
        auto* physMan = model->GetPhysicsManager();
        if (physMan) {
            auto* mmPhysics = physMan->GetMMDPhysics();
            if (mmPhysics) {
                mmPhysics->SetFPS(60.f);
                mmPhysics->SetMaxSubStepCount(4);
            }
        }
    }

    // ── Step 0b: Force DISABLE_DEACTIVATION on all dynamic bodies ────────
    // Bullet's sleep system silently ignores forces on sleeping bodies.
    // During idle the model barely moves so bodies fall asleep within ~2 s.
    // Setting DISABLE_DEACTIVATION once here prevents that for the whole
    // session — the overhead is negligible (one flag check per body/frame).
    // We also set high damping (LINEAR_DAMPING / ANGULAR_DAMPING) for the
    // "heavy tissue" behaviour requested; setDamping is idempotent.
    {
        auto* physMan = model->GetPhysicsManager();
        if (physMan) {
            auto* rigidbodys = physMan->GetRigidBodys();
            if (rigidbodys) {
                for (auto& mmdRb : *rigidbodys) {
                    btRigidBody* rb = mmdRb->GetRigidBody();
                    if (!rb || rb->isStaticObject() || rb->isKinematicObject()) continue;
                    if (rb->getMass() < JIGGLE_MASS_THRESHOLD) continue;

                    // Prevent sleep — ensures subtle jiggles are never silently ignored.
                    rb->setActivationState(DISABLE_DEACTIVATION);
                    // High damping = soft-tissue resistance (no jelly bounce).
                    rb->setDamping(LINEAR_DAMPING, ANGULAR_DAMPING);
                }
            }
        }
    }

    // ── Step 0c: Clamp delta time ─────────────────────────────────────────
    // MAX_ANIM_DT caps the animation clock so a GC pause does not make
    // characters teleport.  EMA smooths the physics dt for Bullet stability.
    const float animDt = std::min(rawDeltaTime, MAX_ANIM_DT);

    m_smoothedPhysDt = m_smoothedPhysDt * (1.f - PHYS_DT_EMA)
                     + animDt            * PHYS_DT_EMA;
    // physDt is passed to Saba's UpdatePhysicsAnimation, which passes it to
    // MMDPhysics::Update → stepSimulation(physDt, maxSubSteps, 1/fps).
    // Clamp to [0.001, 1/30] — prevents runaway on very slow frames.
    const float physDt = std::max(0.001f, std::min(m_smoothedPhysDt, 1.f / 30.f));

    // ── Step 0d: Tick auxiliary systems ───────────────────────────────────
    tickBlink(animDt);
    tickMouth(animDt);
    tickEventTimer(animDt);

    // ── Step 1: Advance animation layers and evaluate VMD keyframes ───────
    if (m_base.anim) {
        m_base.time += animDt;
        if (m_base.looping && m_base.duration > 0.f && m_base.time > m_base.duration)
            m_base.time = std::fmod(m_base.time, m_base.duration);
    }

    if (m_eventActive && m_event.anim) {
        m_event.time += animDt;
        if (m_event.duration > 0.f && m_event.time >= m_event.duration) {
            m_eventActive  = false;
            m_event.target = 0.f;
            m_base.target  = 1.f;
            LOGI("Event [%s] complete", m_event.category.c_str());
        }
    }

    float alpha = std::min(1.f, WEIGHT_FADE_SPEED * animDt);
    m_base.weight  += (m_base.target  - m_base.weight)  * alpha;
    m_event.weight += (m_event.target - m_event.weight) * alpha;

    model->BeginAnimation();

    if (m_base.anim && m_base.weight > 0.001f)
        m_base.anim->Evaluate(m_base.time * 30.f, m_base.weight);

    if (m_event.anim && m_event.weight > 0.001f)
        m_event.anim->Evaluate(m_event.time * 30.f, m_event.weight);

    model->UpdateMorphAnimation();

    // ── Step 2: Pre-physics bone update ───────────────────────────────────
    // Propagates VMD keyframe results (local transforms) into world-space
    // global matrices for bones where IsDeformAfterPhysics == false.
    // Kinematic rigid bodies (bone-driven) read these matrices now to
    // position themselves in the Bullet world before the physics step.
    model->UpdateNodeAnimation(false);

    // ── Step 3: Force injection — AFTER bone update, BEFORE physics step ─
    // This is the critical ordering: VMD has placed bones, but Bullet hasn't
    // run yet.  Gravity tilt and jiggle impulses injected here are seen by
    // the upcoming stepSimulation and influence the physics outcome.
    //
    // If we called applyPhysicsInertia BEFORE UpdateNodeAnimation(false)
    // (as in the old code), kinematic bodies would not yet reflect the VMD
    // pose, so the forces would be computed against stale positions.
    applyPhysicsInertia(model, physDt);
    applyJiggleImpulses(model);

    // ── Step 4: Physics solve ─────────────────────────────────────────────
    // Internally:
    //   1. SetActivation(true) on all MMDRigidBody objects — this is the
    //      Kinematic→Dynamic switch for breast/hair bodies.  After this call
    //      their world transform is owned by Bullet, NOT by VMD keyframes.
    //   2. physics->Update(elapsed) → stepSimulation with configured substeps.
    //   3. ReflectGlobalTransform + CalcLocalTransform — writes Bullet results
    //      back into MMDNode global and local matrices.
    model->UpdatePhysicsAnimation(physDt);

    // ── Step 5: Post-physics bone update ─────────────────────────────────
    // Propagates physics results into child bones flagged
    // IsDeformAfterPhysics == true (hair chains, cloth, skirt joints).
    // These bones read the world matrices written by Step 4 and compute
    // their own positions relative to the physics-driven parent.
    model->UpdateNodeAnimation(true);

    // ── Step 6 & 7: Finalize ──────────────────────────────────────────────
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
