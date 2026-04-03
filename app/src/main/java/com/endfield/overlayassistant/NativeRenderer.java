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
    public native boolean nativeLoadMotion(String vmdPath, String motionCategory);
    public native void nativePlayMotionCategory(String motionCategory);

    public native void nativeTouchEvent(float x, float y, int action);

    public native void nativeSetTransform(float x, float y, float scale, float alpha);
    public native void nativeSetPowerSave(boolean enabled);
    public native void nativeSetAffinityTier(int tier);
}
