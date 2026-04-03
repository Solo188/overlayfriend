/**
 * native-lib.cpp
 *
 * JNI bridge between Java (OverlayService / NativeRenderer) and the C++
 * rendering engine.  Every public symbol is a JNI-exported function named
 * Java_com_endfield_overlayassistant_NativeRenderer_<method>.
 *
 * Threading model:
 *   - All GL calls happen on the GL thread owned by NativeRenderer (Java side).
 *   - Touch / affinity callbacks arrive on the Android main thread; they are
 *     queued into an atomic ring buffer and consumed on the GL thread.
 */

#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>

#include "renderer/MMDRenderer.h"
#include "motion/VMDManager.h"

#define LOG_TAG "EndfieldNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static std::unique_ptr<MMDRenderer> g_renderer;
static std::unique_ptr<VMDManager>  g_vmdManager;

struct TouchEvent { float x; float y; int type; };
static constexpr size_t TOUCH_QUEUE_SIZE = 64;
static TouchEvent  g_touchQueue[TOUCH_QUEUE_SIZE];
static std::atomic<size_t> g_touchHead{0};
static std::atomic<size_t> g_touchTail{0};

static void enqueueTouchEvent(float x, float y, int type) {
    size_t next = (g_touchHead.load(std::memory_order_relaxed) + 1) % TOUCH_QUEUE_SIZE;
    if (next != g_touchTail.load(std::memory_order_acquire)) {
        g_touchQueue[g_touchHead.load()] = {x, y, type};
        g_touchHead.store(next, std::memory_order_release);
    }
}

static std::atomic<int>  g_targetFps{60};
static std::atomic<bool> g_powerSave{false};

using Clock = std::chrono::steady_clock;
static Clock::time_point g_lastFrame;

static std::string jstring2str(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    std::string s(chars);
    env->ReleaseStringUTFChars(js, chars);
    return s;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeInit(
        JNIEnv* env, jobject /*thiz*/,
        jint surfaceWidth, jint surfaceHeight)
{
    LOGI("nativeInit  w=%d  h=%d", surfaceWidth, surfaceHeight);

    if (!g_renderer) {
        g_renderer = std::make_unique<MMDRenderer>();
    }
    if (!g_vmdManager) {
        g_vmdManager = std::make_unique<VMDManager>();
    }

    bool ok = g_renderer->initialize(surfaceWidth, surfaceHeight);
    if (!ok) {
        LOGE("MMDRenderer::initialize failed");
        return JNI_FALSE;
    }
    g_vmdManager->attachRenderer(g_renderer.get());

    g_lastFrame = Clock::now();
    LOGI("nativeInit  OK");
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeLoadModel(
        JNIEnv* env, jobject /*thiz*/, jstring pmxPath)
{
    if (!g_renderer) return JNI_FALSE;
    std::string path = jstring2str(env, pmxPath);
    LOGI("nativeLoadModel  path=%s", path.c_str());
    bool ok = g_renderer->loadPMXModel(path);
    if (!ok) { LOGE("loadPMXModel failed: %s", path.c_str()); }
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
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

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativePlayMotionCategory(
        JNIEnv* env, jobject /*thiz*/, jstring motionCategory)
{
    if (!g_vmdManager) return;
    std::string cat = jstring2str(env, motionCategory);
    g_vmdManager->playCategory(cat);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeRender(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    if (!g_renderer) return;

    auto now     = Clock::now();
    double minMs = 1000.0 / g_targetFps.load(std::memory_order_relaxed);
    double elapsed = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
    if (elapsed < minMs) return;
    double deltaTime = elapsed / 1000.0;
    g_lastFrame = now;

    size_t head = g_touchHead.load(std::memory_order_acquire);
    while (g_touchTail.load(std::memory_order_relaxed) != head) {
        size_t tail = g_touchTail.load(std::memory_order_relaxed);
        TouchEvent& ev = g_touchQueue[tail];
        if (ev.type == 0) {
            g_renderer->onTouchDown(ev.x, ev.y);
        }
        g_touchTail.store((tail + 1) % TOUCH_QUEUE_SIZE, std::memory_order_release);
        head = g_touchHead.load(std::memory_order_acquire);
    }

    if (g_vmdManager) g_vmdManager->update(static_cast<float>(deltaTime));
    g_renderer->render(static_cast<float>(deltaTime));
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSurfaceChanged(
        JNIEnv* /*env*/, jobject /*thiz*/, jint width, jint height)
{
    if (g_renderer) g_renderer->onSurfaceChanged(width, height);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeTouchEvent(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat x, jfloat y, jint action)
{
    enqueueTouchEvent(x, y, action);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetTransform(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jfloat x, jfloat y, jfloat scale, jfloat alpha)
{
    if (g_renderer) g_renderer->setTransform(x, y, scale, alpha);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetPowerSave(
        JNIEnv* /*env*/, jobject /*thiz*/, jboolean enabled)
{
    bool on = (enabled == JNI_TRUE);
    g_powerSave.store(on, std::memory_order_relaxed);
    g_targetFps.store(on ? 30 : 60, std::memory_order_relaxed);
    LOGI("PowerSave %s -> %d FPS", on ? "ON" : "OFF", g_targetFps.load());
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeSetAffinityTier(
        JNIEnv* /*env*/, jobject /*thiz*/, jint tier)
{
    if (g_vmdManager) g_vmdManager->setAffinityTier(tier);
    LOGI("AffinityTier set to %d", tier);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_endfield_overlayassistant_NativeRenderer_nativeDestroy(
        JNIEnv* /*env*/, jobject /*thiz*/)
{
    LOGI("nativeDestroy");
    if (g_vmdManager) { g_vmdManager.reset(); }
    if (g_renderer)   { g_renderer->shutdown(); g_renderer.reset(); }
}
