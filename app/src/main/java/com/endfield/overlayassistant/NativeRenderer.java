package com.endfield.overlayassistant;

public final class NativeRenderer {

    static {
        System.loadLibrary("endfield-native");
    }

    public native boolean nativeInit(int surfaceWidth, int surfaceHeight);
    public native void    nativeSurfaceChanged(int width, int height);
    public native void    nativeRender();
    public native void    nativeDestroy();

    public native boolean nativeLoadModel(String pmxPath);

    /**
     * Scan baseDir/motions/{idle,poses,waiting,dance,touch} and load all .vmd
     * files found.  Automatically starts the idle base layer after scanning.
     * Must be called from the GL thread (via queueGLEvent / queueEvent).
     *
     * @param baseDir  e.g. "/sdcard/Documents/Assistant"
     */
    public native boolean nativeScanMotions(String baseDir);

    /**
     * Load a single .vmd file into a named category pool.
     * Used for hot-loading individual files at runtime.
     */
    public native boolean nativeLoadMotion(String vmdPath, String motionCategory);

    /**
     * Legacy: direct category playback (used for "night" / "idle" commands).
     * Internal timer and layer system handles random waiting/dance automatically.
     */
    public native void nativePlayMotionCategory(String motionCategory);

    /**
     * Priority touch interrupt — immediately plays a random animation from the
     * "touch" category on Layer 1, overriding any currently running event.
     * Must be called from the GL thread.
     */
    public native void nativeOnTouch();

    /**
     * Pass window-drag velocity (screen pixels / second) to the physics engine.
     *
     * During overlay dragging, the entire window moves across the screen while
     * the user's finger stays at a fixed position within the GL view — so GL
     * touch coordinates don't change.  Java measures the screen-space velocity
     * (rawX/Y delta / elapsed time) and sends it here so the Bullet physics
     * world can tilt gravity for hair/cloth inertia and fire jiggle impulses.
     *
     * Call with (0, 0) on ACTION_UP so physics damps out after release.
     * Must be called from the GL thread.
     *
     * @param vx  Horizontal velocity in screen pixels/second (positive = rightward)
     * @param vy  Vertical velocity in screen pixels/second (positive = downward)
     */
    public native void nativeSetDragVelocity(float vx, float vy);

    public native void    nativeTouchEvent(float x, float y, int action);
    public native void    nativeSetTransform(float x, float y, float scale, float alpha);
    public native void    nativeSetPowerSave(boolean enabled);
    public native void    nativeSetAffinityTier(int tier);
}
