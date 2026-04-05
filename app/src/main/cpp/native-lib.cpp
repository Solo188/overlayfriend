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
    if (!g_renderer) return JNI_FALSE;
    std::string path = jstring2str(env, pmxPath);
    LOGI("nativeLoadModel  path=%s", path.c_str());
    bool ok = g_renderer->loadPMXModel(path);
    if (!ok) LOGE("loadPMXModel failed: %s", path.c_str());
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeLoadMotion(
        JNIEnv* env, jobject /*thiz*/,
        jstring vmdPath, jstring motionCategory)
{
    if (!g_vmdManager) return JNI_FALSE;
    std::string path = jstring2str(env, vmdPath);
    std::string cat  = jstring2str(env, motionCategory);
    LOGI("nativeLoadMotion  path=%s  category=%s", path.c_str(), cat.c_str());
    return g_vmdManager->loadMotion(path, cat) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativePlayMotionCategory(
        JNIEnv* env, jobject /*thiz*/, jstring motionCategory)
{
    if (!g_vmdManager) return;
    std::string cat = jstring2str(env, motionCategory);
    g_vmdManager->playCategory(cat);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeRender(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    if (!g_renderer) return;

    // Compute actual elapsed time since the last frame.
    // We do NOT skip frames based on a target FPS — GLSurfaceView already
    // calls onDrawFrame at the display's vsync rate.  Skipping frames here
    // caused an irregular timing pattern that made animation look jerky.
    auto   now     = Clock::now();
    double elapsed = std::chrono::duration<double>(now - g_lastFrame).count();
    g_lastFrame    = now;

    // Safety clamp: don't let a huge spike (e.g. app resume after 5 s in
    // background) pass a gigantic dt into the physics engine.
    // VMDManager clamps again internally, but this keeps the value sane here.
    float dt = static_cast<float>(std::min(elapsed, 0.25));  // max 250 ms

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

    // Pass current drag velocity to VMDManager so it can tilt physics gravity.
    if (g_vmdManager && g_renderer) {
        g_vmdManager->setDragVelocity(g_renderer->getDragVelX(),
                                      g_renderer->getDragVelY());
    }

    if (g_vmdManager) g_vmdManager->update(dt);
    g_renderer->render(dt);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSurfaceChanged(
        JNIEnv* /*env*/, jobject /*thiz*/, jint width, jint height)
{
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
    if (g_renderer) g_renderer->setTransform(x, y, scale, alpha);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetPowerSave(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled)
{
    // Power-save mode should be handled on the Java side by switching
    // GLSurfaceView to RENDERMODE_WHEN_DIRTY at 30 fps.
    // Log it here so the Java layer can react if needed.
    LOGI("PowerSave %s", (enabled == JNI_TRUE) ? "ON" : "OFF");
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetAffinityTier(
        JNIEnv* /*env*/, jobject /*thiz*/, jint tier)
{
    if (g_vmdManager) g_vmdManager->setAffinityTier(tier);
    LOGI("AffinityTier set to %d", tier);
}

extern "C" JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeDestroy(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    LOGI("nativeDestroy");
    if (g_vmdManager) g_vmdManager.reset();
    if (g_renderer)   { g_renderer->shutdown(); g_renderer.reset(); }
}
