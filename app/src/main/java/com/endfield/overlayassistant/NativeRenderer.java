package com.endfield.overlayassistant;

public final class NativeRenderer {

    static {
        System.loadLibrary("endfield-native");
    }

    public native boolean nativeInit(int surfaceWidth, int surfaceHeight);
    public native void nativeSurfaceChanged(int width, int height);
    public native void nativeRender();
    public native void nativeDestroy();

    public native boolean nativeLoadModel(String pmxPath);

    /**
     * Auto-scan modelDir/motions/{idle,poses,waiting,dance,touch} and load all
     * .vmd files found.  Automatically starts idle playback after loading.
     * Must be called from the GL thread (via queueGLEvent).
     */
    public native boolean nativeScanMotions(String modelDir);

    /**
     * Load a single VMD file into the given category pool.
     * Kept for backward compatibility (e.g. user-supplied VMD files).
     */
    public native boolean nativeLoadMotion(String vmdPath, String motionCategory);

    /**
     * Legacy category playback — kept for backward compatibility.
     * Internal layer system now manages playback automatically.
     */
    public native void nativePlayMotionCategory(String motionCategory);

    /**
     * Priority touch interrupt: immediately plays a random animation from the
     * "touch" category on Layer 1, overriding any currently running event.
     * Call from the GL thread for thread safety.
     */
    public native void nativeOnTouch();

    public native void nativeTouchEvent(float x, float y, int action);

    public native void nativeSetTransform(float x, float y, float scale, float alpha);
    public native void nativeSetPowerSave(boolean enabled);
    public native void nativeSetAffinityTier(int tier);
}
