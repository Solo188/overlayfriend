package com.endfield.overlayassistant;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.GLSurfaceView;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class OverlayView extends FrameLayout {

    private final NativeRenderer             m_renderer;
    private final AffinityManager            m_affinity;
    private final IAiAssistant               m_ai;
    private final WindowManager.LayoutParams m_params;

    private       GLSurfaceView              m_glView;
    private       TextView                   m_bubble;
    private final Handler                    m_uiHandler = new Handler(Looper.getMainLooper());

    // FIX: track whether nativeInit has completed so nativeRender never runs before it
    private volatile boolean m_glReady = false;

    private float m_touchStartRawX;
    private float m_touchStartRawY;
    private int   m_initParamX;
    private int   m_initParamY;

    private boolean m_positionLocked = false;
    private boolean m_silentMode     = false;

    private static final int HEADPAT_SLOP_PX = 20;

    // Fixed GL surface size — must match the size passed to nativeInit
    private static final int GL_W = 400;
    private static final int GL_H = 600;

    public OverlayView(Context context,
                       NativeRenderer renderer,
                       AffinityManager affinity,
                       IAiAssistant ai,
                       WindowManager.LayoutParams params) {
        super(context);
        m_renderer = renderer;
        m_affinity = affinity;
        m_ai       = ai;
        m_params   = params;
        inflate(context);
    }

    private void inflate(Context context) {
        m_glView = new GLSurfaceView(context);
        m_glView.setEGLContextClientVersion(3);
        m_glView.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        m_glView.setRenderer(new GLSurfaceView.Renderer() {
            @Override
            public void onSurfaceCreated(GL10 gl, EGLConfig config) {
                // FIX: use fixed GL_W/GL_H instead of getWidth()/getHeight() which
                // return 0 at this point (view not measured yet). onSurfaceChanged
                // will follow immediately with the real dimensions and correct them.
                m_glReady = false;
                boolean ok = m_renderer.nativeInit(GL_W, GL_H);
                if (ok) m_glReady = true;
            }

            @Override
            public void onSurfaceChanged(GL10 gl, int width, int height) {
                // This is called with real dimensions right after onSurfaceCreated.
                // Always update the viewport so the renderer knows the actual size.
                m_renderer.nativeSurfaceChanged(width, height);
                // If init somehow failed (e.g. 0x0 fallback), retry it here.
                if (!m_glReady && width > 0 && height > 0) {
                    boolean ok = m_renderer.nativeInit(width, height);
                    if (ok) m_glReady = true;
                }
            }

            @Override
            public void onDrawFrame(GL10 gl) {
                // FIX: guard against rendering before init completes
                if (!m_glReady) return;
                m_renderer.nativeRender();
            }
        });
        m_glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        addView(m_glView, new FrameLayout.LayoutParams(GL_W, GL_H));

        m_bubble = new TextView(context);
        m_bubble.setTextSize(14f);
        m_bubble.setTextColor(0xFFFFFFFF);
        m_bubble.setBackgroundColor(0xCC000000);
        m_bubble.setPadding(16, 8, 16, 8);
        m_bubble.setVisibility(View.GONE);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 10;
        addView(m_bubble, lp);
    }

    public void loadModel(String pmxPath) {
        // FIX: queue on GL thread — model loading must happen on the GL thread
        // because it uploads textures via OpenGL calls (glGenTextures etc.)
        m_glView.queueEvent(() -> m_renderer.nativeLoadModel(pmxPath));
    }

    /** Queue any runnable on the GL thread (for motion loading, playback commands etc.) */
    public void queueGLEvent(Runnable r) {
        m_glView.queueEvent(r);
    }

    public void showBubble(String text) {
        if (m_silentMode) return;
        m_uiHandler.post(() -> {
            m_bubble.setText(text);
            m_bubble.setVisibility(View.VISIBLE);
        });
        m_uiHandler.postDelayed(() ->
            m_bubble.setVisibility(View.GONE), 4000L);
    }

    public void setSilentMode(boolean silent) {
        m_silentMode = silent;
        if (silent) m_bubble.setVisibility(View.GONE);
    }

    public void setPositionLocked(boolean locked) {
        m_positionLocked = locked;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        float rawX = event.getRawX();
        float rawY = event.getRawY();

        // Only send touch to native if GL is ready
        if (m_glReady) {
            m_glView.queueEvent(() ->
                m_renderer.nativeTouchEvent(event.getX(), event.getY(), event.getAction()));
        }

        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                m_touchStartRawX = rawX;
                m_touchStartRawY = rawY;
                m_initParamX     = m_params.x;
                m_initParamY     = m_params.y;
                return true;

            case MotionEvent.ACTION_MOVE:
                if (!m_positionLocked) {
                    m_params.x = m_initParamX + (int)(rawX - m_touchStartRawX);
                    m_params.y = m_initParamY + (int)(rawY - m_touchStartRawY);
                    WindowManager wm = (WindowManager) getContext()
                            .getSystemService(Context.WINDOW_SERVICE);
                    if (wm != null) wm.updateViewLayout(this, m_params);
                }
                return true;

            case MotionEvent.ACTION_UP:
                float dx = rawX - m_touchStartRawX;
                float dy = rawY - m_touchStartRawY;
                boolean isHeadpat = (Math.abs(dx) < HEADPAT_SLOP_PX &&
                                     Math.abs(dy) < HEADPAT_SLOP_PX);
                if (isHeadpat) onHeadpat();
                savePosition(m_params.x, m_params.y);
                return true;
        }
        return super.onTouchEvent(event);
    }

    private void onHeadpat() {
        m_affinity.onHeadpat();
        m_renderer.nativePlayMotionCategory("touch");
        String response = m_ai.processInput("headpat", m_affinity.getTier());
        showBubble(response);
    }

    private void savePosition(int x, int y) {
        getContext().getSharedPreferences("overlay_state", Context.MODE_PRIVATE)
                    .edit()
                    .putInt("pos_x", x)
                    .putInt("pos_y", y)
                    .apply();
    }

    @Override
    protected void onDetachedFromWindow() {
        m_glReady = false;
        m_glView.queueEvent(m_renderer::nativeDestroy);
        m_glView.onPause();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        m_glView.onResume();
    }
}
