/**
 * native-lib.cpp
 *
 * JNI bridge between Java (OverlayService / NativeRenderer) and the C++
 * rendering engine.
 *
 * Touch event types (MotionEvent.getAction()):
 *   ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2
 *
 * Frame timing
 * ─────────────
 * The old approach capped frames by returning early when elapsed < minMs.
 * This created an irregular "long–short–long" frame pattern that made
 * animation subtly jerky even at 60 fps.
 *
 * New approach: render on EVERY vsync call from GLSurfaceView
 * (RENDERMODE_CONTINUOUSLY at the system refresh rate).  deltaTime is
 * clamped inside VMDManager to prevent large jumps.  Power-save mode is
 * handled by switching to RENDERMODE_WHEN_DIRTY at 30 fps from Java.
 *
 * SIGSEGV prevention
 * ──────────────────
 * g_destroyed is set to true by nativeDestroy() and checked at the start of
 * every JNI call that touches GL or native objects.  This prevents
 * use-after-free when the GL thread processes queued events after the EGL
 * context has been torn down.
 *
 * Drag velocity / jiggle physics
 * ──────────────────────────────
 * When the overlay window is dragged, Java measures screen-space velocity
 * (rawX/Y delta / dt) and stores it in g_dragVelX/Y via nativeSetDragVelocity.
 * Each frame nativeRender forwards these values to VMDManager, which:
 *   1. Tilts Bullet gravity laterally (hair/cloth inertia, proportional to speed)
 *   2. Fires an impulse on heavy rigid bodies (breast/hip) on sudden stops/jerks
 * On ACTION_UP Java sets velocity to (0,0) so Bullet damps out naturally.
 */

#include <jni.h>
#include <android/log.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include <string>
#include <memory>
#include <atomic>
#include <chrono>

#include "renderer/MMDRenderer.h"
#include "motion/VMDManager.h"

#define LOG_TAG "EndfieldNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<MMDRenderer> g_renderer;
static std::unique_ptr<VMDManager>  g_vmdManager;

// Set to true after nativeDestroy() — guards all subsequent JNI calls.
// This prevents use-after-free on the GL thread when the EGL surface is
// torn down before queued events have been drained.
static std::atomic<bool> g_destroyed{false};

// ─── Drag velocity (set from Java, consumed by VMDManager each frame) ─────────
//
// Using relaxed atomics: the GL thread reads these once per frame and the UI
// thread writes them on every ACTION_MOVE.  A single-frame lag is acceptable
// for physics — the cost of a full memory barrier on every finger movement is
// not worth it.
//
static std::atomic<float> g_dragVelX{0.f};
static std::atomic<float> g_dragVelY{0.f};

// ─── Touch event queue (lock-free SPSC ring buffer) ──────────────────────────
// type: 0=ACTION_DOWN, 1=ACTION_UP, 2=ACTION_MOVE

struct TouchEvent { float x; float y; int type; };
static constexpr size_t TOUCH_QUEUE_SIZE = 64;
static TouchEvent              g_touchQueue[TOUCH_QUEUE_SIZE];
static std::atomic<size_t>     g_touchHead{0};
static std::atomic<size_t>     g_touchTail{0};

static void enqueueTouchEvent(float x, float y, int type) {
    size_t next = (g_touchHead.load(std::memory_order_relaxed) + 1) % TOUCH_QUEUE_SIZE;
    if (next != g_touchTail.load(std::memory_order_acquire)) {
        g_touchQueue[g_touchHead.load()] = {x, y, type};
        g_touchHead.store(next, std::memory_order_release);
    }
}

// ─── Frame timing ─────────────────────────────────────────────────────────────

using Clock = std::chrono::steady_clock;
static Clock::time_point g_lastFrame;

static std::string jstring2str(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    std::string s(chars);
    env->ReleaseStringUTFChars(js, chars);
    return s;
}

// ─── JNI functions ────────────────────────────────────────────────────────────

extern "C" JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeInit(
        JNIEnv* env, jobject /*thiz*/,
        jint surfaceWidth, jint surfaceHeight)
{
    LOGI("nativeInit  w=%d  h=%d", surfaceWidth, surfaceHeight);
    g_destroyed.store(false, std::memory_order_release);

    if (!g_renderer)   g_renderer   = std::make_unique<MMDRenderer>();
    if (!g_vmdManager) g_vmdManager = std::make_unique<VMDManager>();

    bool ok = g_renderer->initialize(surfaceWidth, surfaceHeight);
    if (!ok) { LOGE("MMDRenderer::initialize failed"); return JNI_FALSE; }

    g_vmdManager->attachRenderer(g_renderer.get());
    g_lastFrame = Clock::now();

    LOGI("nativeInit  OK");
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeLoadModel(
        JNIEnv* env, jobject /*thiz*/, jstring pmxPath)
{
    if (g_destroyed.load(std::memory_order_acquire) || !g_renderer) return JNI_FALSE;
    std::string path = jstring2str(env, pmxPath);
    LOGI("nativeLoadModel  path=%s", path.c_str());
    bool ok = g_renderer->loadPMXModel(path);
    if (!ok) LOGE("loadPMXModel failed: %s", path.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Auto-scan modelDir/motions/{idle,poses,waiting,dance,touch} and load all VMD
 * files.  Replaces the per-file nativeLoadMotion loop for new categories.
 */
extern "C" JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeScanMotions(
        JNIEnv* env, jobject /*thiz*/, jstring modelDir)
{
    if (g_destroyed.load(std::memory_order_acquire) || !g_vmdManager) return JNI_FALSE;
    std::string dir = jstring2str(env, modelDir);
    LOGI("nativeScanMotions  dir=%s", dir.c_str());
    g_vmdManager->scanMotions(dir);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeLoadMotion(
        JNIEnv* env, jobject /*thiz*/,
        jstring vmdPath, jstring motionCategory)
{
    if (g_destroyed.load(std::memory_order_acquire) || !g_vmdManager) return JNI_FALSE;
    std::string path = jstring2str(env, vmdPath);
    std::string cat  = jstring2str(env, motionCategory);
    LOGI("nativeLoadMotion  path=%s  category=%s", path.c_str(), cat.c_str());
    return g_vmdManager->loadMotion(path, cat) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativePlayMotionCategory(
        JNIEnv* env, jobject /*thiz*/, jstring motionCategory)
{
    // Kept for backward compatibility (night / user / friend categories from Java).
    // New categories (idle, waiting, dance, touch) are managed internally.
    if (g_destroyed.load(std::memory_order_acquire) || !g_vmdManager) return;
    // No-op: internal timer and layer system now handles all playback decisions.
    (void)motionCategory;
}

/**
 * Priority-interrupt: immediately plays a random "touch" animation on Layer 1.
 * Called from Java when the user headpats / taps the model.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeOnTouch(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    if (g_destroyed.load(std::memory_order_acquire) || !g_vmdManager) return;
    g_vmdManager->onTouch();
}

/**
 * nativeSetDragVelocity — called from Java's ACTION_MOVE handler.
 *
 * Java computes velocity = (rawPos - prevRawPos) / deltaTime for each pointer
 * move event and passes it here.  The values are stored in relaxed atomics and
 * consumed by nativeRender once per frame.
 *
 * On ACTION_UP Java calls this with (0, 0) to signal "drag stopped" so Bullet
 * can damp the jiggle back to rest naturally.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetDragVelocity(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat vx, jfloat vy)
{
    if (g_destroyed.load(std::memory_order_acquire)) return;
    g_dragVelX.store(vx, std::memory_order_relaxed);
    g_dragVelY.store(vy, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeRender(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    // Guard: don't render after nativeDestroy().
    if (g_destroyed.load(std::memory_order_acquire)) return;
    if (!g_renderer) return;

    auto   now     = Clock::now();
    double elapsed = std::chrono::duration<double>(now - g_lastFrame).count();
    g_lastFrame    = now;

    // Safety clamp before handing to VMDManager (which clamps again internally).
    float dt = static_cast<float>(std::min(elapsed, 0.25));

    // ── Process queued touch events on the GL thread ──────────────────────
    size_t head = g_touchHead.load(std::memory_order_acquire);
    while (g_touchTail.load(std::memory_order_relaxed) != head) {
        size_t tail = g_touchTail.load(std::memory_order_relaxed);
        TouchEvent& ev = g_touchQueue[tail];

        switch (ev.type) {
            case 0: g_renderer->onTouchDown(ev.x, ev.y); break;   // ACTION_DOWN
            case 2: g_renderer->onTouchMove(ev.x, ev.y); break;   // ACTION_MOVE
            case 1: g_renderer->onTouchUp();              break;   // ACTION_UP
            default: break;
        }

        g_touchTail.store((tail + 1) % TOUCH_QUEUE_SIZE, std::memory_order_release);
        head = g_touchHead.load(std::memory_order_acquire);
    }

    // ── Forward drag velocity to physics ─────────────────────────────────
    //
    // Java measures window-level drag (the overlay moves across the screen),
    // which is invisible to GL touch coordinates.  Use the Java-provided value
    // rather than the renderer's internal touch tracking.
    if (g_vmdManager) {
        g_vmdManager->setDragVelocity(
            g_dragVelX.load(std::memory_order_relaxed),
            g_dragVelY.load(std::memory_order_relaxed));
    }

    if (g_vmdManager) g_vmdManager->update(dt);
    g_renderer->render(dt);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSurfaceChanged(
        JNIEnv* /*env*/, jobject /*thiz*/, jint width, jint height)
{
    if (g_destroyed.load(std::memory_order_acquire)) return;
    if (g_renderer) g_renderer->onSurfaceChanged(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeTouchEvent(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat x, jfloat y, jint action)
{
    enqueueTouchEvent(x, y, action);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetTransform(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jfloat x, jfloat y, jfloat scale, jfloat alpha)
{
    if (g_destroyed.load(std::memory_order_acquire)) return;
    if (g_renderer) g_renderer->setTransform(x, y, scale, alpha);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetPowerSave(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled)
{
    LOGI("PowerSave %s", (enabled == JNI_TRUE) ? "ON" : "OFF");
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetAffinityTier(
        JNIEnv* /*env*/, jobject /*thiz*/, jint tier)
{
    if (g_destroyed.load(std::memory_order_acquire)) return;
    if (g_vmdManager) g_vmdManager->setAffinityTier(tier);
    LOGI("AffinityTier set to %d", tier);
}

/**
 * nativeDestroy — called from the GL thread via queueEvent in onDetachedFromWindow.
 *
 * CRITICAL: Java-side OverlayView MUST wait for this call to complete (via
 * CountDownLatch) before calling GLSurfaceView.onPause(), which destroys the
 * EGL surface.  Without this synchronisation, the Adreno gralloc driver can
 * crash with SIGSEGV (SEGV_ACCERR) inside ReleaseBuffer while the GL thread
 * is still flushing commands.
 */
extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeDestroy(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    LOGI("nativeDestroy");

    // Signal all other JNI calls to bail out immediately.
    g_destroyed.store(true, std::memory_order_release);

    // Reset velocity so stale values don't leak into the next session.
    g_dragVelX.store(0.f, std::memory_order_relaxed);
    g_dragVelY.store(0.f, std::memory_order_relaxed);

    if (g_vmdManager) g_vmdManager.reset();
    if (g_renderer)   { g_renderer->shutdown(); g_renderer.reset(); }

    LOGI("nativeDestroy complete");
}
