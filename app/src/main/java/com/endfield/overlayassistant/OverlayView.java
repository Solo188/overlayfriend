package com.endfield.overlayassistant;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
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
    private final WindowManager              m_wm;

    private GLSurfaceView m_glView;
    private TextView      m_bubble;
    private final Handler m_uiHandler = new Handler(Looper.getMainLooper());

    private volatile boolean m_glReady       = false;
    private volatile String  m_pendingPmxPath = null;

    // ── Touch / drag ──────────────────────────────────────────────────────
    private float m_touchStartRawX, m_touchStartRawY;
    private int   m_initParamX, m_initParamY;
    private boolean m_positionLocked = false;
    private boolean m_silentMode     = false;

    private static final int HEADPAT_SLOP_PX = 20;

    // ── Pinch-to-zoom ─────────────────────────────────────────────────────
    private ScaleGestureDetector m_scaleDetector;
    private float  m_currentScale  = 1.0f;
    private static final float SCALE_MIN = 0.4f;
    private static final float SCALE_MAX = 3.0f;

    // ── GL surface size ───────────────────────────────────────────────────
    // Base size that matches the native renderer's aspect ratio (400:600 = 2:3)
    private static final int GL_W_BASE = 400;
    private static final int GL_H_BASE = 600;

    // Current physical pixel size of the GL view (changes with scale)
    private int m_glPixW = GL_W_BASE;
    private int m_glPixH = GL_H_BASE;

    private static final String PREFS_SETTINGS = "overlay_settings";
    private static final String KEY_SCALE   = "scale";
    private static final String KEY_OPACITY = "opacity";

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
        m_wm       = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);

        // Load saved scale from settings
        SharedPreferences prefs = context.getSharedPreferences(PREFS_SETTINGS, Context.MODE_PRIVATE);
        m_currentScale = prefs.getFloat(KEY_SCALE, 1.0f);
        float opacity  = prefs.getFloat(KEY_OPACITY, 1.0f);

        // Init pinch detector
        m_scaleDetector = new ScaleGestureDetector(context,
                new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override
                    public boolean onScale(ScaleGestureDetector detector) {
                        m_currentScale *= detector.getScaleFactor();
                        m_currentScale  = Math.max(SCALE_MIN, Math.min(SCALE_MAX, m_currentScale));
                        applyScale();
                        return true;
                    }
                });

        buildLayout(context);

        // Apply initial scale and opacity
        m_glPixW = (int)(GL_W_BASE * m_currentScale);
        m_glPixH = (int)(GL_H_BASE * m_currentScale);
        updateGLViewSize();
        if (m_glReady) {
            m_renderer.nativeSetTransform(0, 0, 1.0f, opacity);
        }
    }

    private void buildLayout(Context context) {
        m_glView = new GLSurfaceView(context);
        m_glView.setEGLContextClientVersion(3);
        m_glView.setZOrderOnTop(true);
        m_glView.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        m_glView.setEGLConfigChooser(8, 8, 8, 8, 16, 0);

        m_glView.setRenderer(new GLSurfaceView.Renderer() {
            @Override
            public void onSurfaceCreated(GL10 gl, EGLConfig config) {
                m_glReady = false;
                boolean ok = m_renderer.nativeInit(GL_W_BASE, GL_H_BASE);
                if (ok) {
                    m_glReady = true;
                    String pending = m_pendingPmxPath;
                    if (pending != null) {
                        m_pendingPmxPath = null;
                        m_renderer.nativeLoadModel(pending);
                    }
                    // Apply saved opacity
                    SharedPreferences prefs = context.getSharedPreferences(
                            PREFS_SETTINGS, Context.MODE_PRIVATE);
                    float opacity = prefs.getFloat(KEY_OPACITY, 1.0f);
                    m_renderer.nativeSetTransform(0, 0, 1.0f, opacity);
                }
            }

            @Override
            public void onSurfaceChanged(GL10 gl, int width, int height) {
                m_renderer.nativeSurfaceChanged(width, height);
                if (!m_glReady && width > 0 && height > 0) {
                    boolean ok = m_renderer.nativeInit(width, height);
                    if (ok) {
                        m_glReady = true;
                        String pending = m_pendingPmxPath;
                        if (pending != null) {
                            m_pendingPmxPath = null;
                            m_renderer.nativeLoadModel(pending);
                        }
                    }
                }
            }

            @Override
            public void onDrawFrame(GL10 gl) {
                if (!m_glReady) return;
                m_renderer.nativeRender();
            }
        });

        m_glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        addView(m_glView, new FrameLayout.LayoutParams(m_glPixW, m_glPixH));

        // Speech bubble
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

    // ── Scale helpers ─────────────────────────────────────────────────────

    private void applyScale() {
        m_glPixW = (int)(GL_W_BASE * m_currentScale);
        m_glPixH = (int)(GL_H_BASE * m_currentScale);
        m_uiHandler.post(this::updateGLViewSize);

        // Save scale so Settings shows correct value
        getContext().getSharedPreferences(PREFS_SETTINGS, Context.MODE_PRIVATE)
                .edit().putFloat(KEY_SCALE, m_currentScale).apply();
    }

    private void updateGLViewSize() {
        if (m_glView == null) return;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) m_glView.getLayoutParams();
        if (lp == null) return;
        lp.width  = m_glPixW;
        lp.height = m_glPixH;
        m_glView.setLayoutParams(lp);

        // Also resize the overlay window itself
        m_params.width  = m_glPixW;
        m_params.height = m_glPixH;
        if (m_wm != null) {
            try { m_wm.updateViewLayout(this, m_params); } catch (Exception ignored) {}
        }
    }

    // ── Called by OverlayService when settings change ─────────────────────
    public void applySettings() {
        SharedPreferences prefs = getContext().getSharedPreferences(
                PREFS_SETTINGS, Context.MODE_PRIVATE);
        float scale   = prefs.getFloat(KEY_SCALE,   1.0f);
        float opacity = prefs.getFloat(KEY_OPACITY, 1.0f);
        m_currentScale = scale;
        applyScale();
        if (m_glReady) {
            m_glView.queueEvent(() -> m_renderer.nativeSetTransform(0, 0, 1.0f, opacity));
        }
    }

    // ── Public API ────────────────────────────────────────────────────────

    public void loadModel(String pmxPath) {
        if (m_glReady) {
            m_glView.queueEvent(() -> m_renderer.nativeLoadModel(pmxPath));
        } else {
            m_pendingPmxPath = pmxPath;
        }
    }

    public void queueGLEvent(Runnable r) { m_glView.queueEvent(r); }

    public void showBubble(String text) {
        if (m_silentMode) return;
        m_uiHandler.post(() -> {
            m_bubble.setText(text);
            m_bubble.setVisibility(View.VISIBLE);
        });
        m_uiHandler.postDelayed(() -> m_bubble.setVisibility(View.GONE), 4000L);
    }

    public void setSilentMode(boolean s) {
        m_silentMode = s;
        if (s) m_uiHandler.post(() -> m_bubble.setVisibility(View.GONE));
    }

    public void setPositionLocked(boolean l) { m_positionLocked = l; }

    // ── Touch ─────────────────────────────────────────────────────────────

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // Always pass to scale detector first
        m_scaleDetector.onTouchEvent(event);

        // If scaling — don't drag or headpat
        if (m_scaleDetector.isInProgress()) return true;

        float rawX = event.getRawX();
        float rawY = event.getRawY();

        if (m_glReady) {
            final float lx = event.getX();
            final float ly = event.getY();
            final int act  = event.getAction();
            m_glView.queueEvent(() -> m_renderer.nativeTouchEvent(lx, ly, act));
        }

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                m_touchStartRawX = rawX;
                m_touchStartRawY = rawY;
                m_initParamX     = m_params.x;
                m_initParamY     = m_params.y;
                return true;

            case MotionEvent.ACTION_MOVE:
                if (!m_positionLocked && event.getPointerCount() == 1) {
                    m_params.x = m_initParamX + (int)(rawX - m_touchStartRawX);
                    m_params.y = m_initParamY + (int)(rawY - m_touchStartRawY);
                    if (m_wm != null) {
                        try { m_wm.updateViewLayout(this, m_params); }
                        catch (Exception ignored) {}
                    }
                }
                return true;

            case MotionEvent.ACTION_UP:
                float dx = rawX - m_touchStartRawX;
                float dy = rawY - m_touchStartRawY;
                if (Math.abs(dx) < HEADPAT_SLOP_PX && Math.abs(dy) < HEADPAT_SLOP_PX)
                    onHeadpat();
                savePosition(m_params.x, m_params.y);
                return true;
        }
        return super.onTouchEvent(event);
    }

    private void onHeadpat() {
        m_affinity.onHeadpat();
        if (m_glReady)
            m_glView.queueEvent(() -> m_renderer.nativePlayMotionCategory("touch"));
        showBubble(m_ai.processInput("headpat", m_affinity.getTier()));
    }

    private void savePosition(int x, int y) {
        getContext().getSharedPreferences("overlay_state", Context.MODE_PRIVATE)
                .edit().putInt("pos_x", x).putInt("pos_y", y).apply();
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        m_glView.onResume();
    }

    @Override
    protected void onDetachedFromWindow() {
        m_glReady = false;
        m_glView.queueEvent(m_renderer::nativeDestroy);
        m_glView.onPause();
        super.onDetachedFromWindow();
    }
}
