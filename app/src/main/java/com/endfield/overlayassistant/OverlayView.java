package com.endfield.overlayassistant;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class OverlayView extends FrameLayout {

    private static final String TAG = "OverlayView";

    private final NativeRenderer             m_renderer;
    private final AffinityManager            m_affinity;
    private final IAiAssistant               m_ai;
    private final WindowManager.LayoutParams m_params;
    private final WindowManager              m_wm;

    private GLSurfaceView m_glView;
    private TextView      m_bubble;
    private final Handler m_uiHandler = new Handler(Looper.getMainLooper());

    // true once nativeInit() has succeeded on the GL thread.
    private volatile boolean m_glReady = false;

    // PMX path stored here when loadModel() is called before GL is ready.
    // Consumed inside onSurfaceCreated() on the GL thread.
    private volatile String m_pendingPmxPath = null;

    // Guard against duplicate concurrent model loads (e.g. fast service restart).
    // Set to true before queuing nativeLoadModel; cleared after it returns.
    private final AtomicBoolean m_modelLoading = new AtomicBoolean(false);

    // ── Touch / drag ──────────────────────────────────────────────────────
    private float   m_touchStartRawX, m_touchStartRawY;
    private int     m_initParamX, m_initParamY;
    private boolean m_positionLocked = false;
    private boolean m_silentMode     = false;

    private static final int HEADPAT_SLOP_PX = 20;

    // ── Drag velocity for jiggle physics ──────────────────────────────────
    // Tracks screen-space window movement velocity (pixels / second) and
    // forwards it to the native physics engine on each ACTION_MOVE event.
    private long  m_lastMoveTimeNs = 0;
    private float m_lastRawX       = 0f;
    private float m_lastRawY       = 0f;

    // ── Pinch-to-zoom ─────────────────────────────────────────────────────
    private ScaleGestureDetector m_scaleDetector;
    private float m_currentScale = 1.0f;
    private static final float SCALE_MIN = 0.4f;
    private static final float SCALE_MAX = 3.0f;

    // ── GL surface size ───────────────────────────────────────────────────
    // Base size that matches the native renderer's 2:3 aspect ratio.
    private static final int GL_W_BASE = 400;
    private static final int GL_H_BASE = 600;

    // Current physical pixel size (changes with pinch-to-zoom scale).
    private int m_glPixW = GL_W_BASE;
    private int m_glPixH = GL_H_BASE;

    private static final String PREFS_SETTINGS = "overlay_settings";
    private static final String KEY_SCALE      = "scale";
    private static final String KEY_OPACITY    = "opacity";

    // ─────────────────────────────────────────────────────────────────────
    // Constructor
    // ─────────────────────────────────────────────────────────────────────

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

        SharedPreferences prefs = context.getSharedPreferences(PREFS_SETTINGS, Context.MODE_PRIVATE);
        m_currentScale = prefs.getFloat(KEY_SCALE, 1.0f);

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

        m_glPixW = (int)(GL_W_BASE * m_currentScale);
        m_glPixH = (int)(GL_H_BASE * m_currentScale);
        updateGLViewSize();
    }

    // ─────────────────────────────────────────────────────────────────────
    // GL surface
    // ─────────────────────────────────────────────────────────────────────

    private void buildLayout(Context context) {
        m_glView = new GLSurfaceView(context);
        m_glView.setEGLContextClientVersion(3);
        m_glView.setZOrderOnTop(true);
        m_glView.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        m_glView.setEGLConfigChooser(8, 8, 8, 8, 16, 0);

        m_glView.setRenderer(new GLSurfaceView.Renderer() {
            @Override
            public void onSurfaceCreated(GL10 gl, EGLConfig config) {
                // Running on the GL thread.
                m_glReady = false;
                boolean ok = m_renderer.nativeInit(GL_W_BASE, GL_H_BASE);
                if (!ok) { Log.e(TAG, "nativeInit failed in onSurfaceCreated"); return; }

                m_glReady = true;

                SharedPreferences prefs = context.getSharedPreferences(
                        PREFS_SETTINGS, Context.MODE_PRIVATE);
                float opacity = prefs.getFloat(KEY_OPACITY, 1.0f);
                m_renderer.nativeSetTransform(0, 0, 1.0f, opacity);

                // If a model load was requested before GL was ready, run it now.
                String pending = m_pendingPmxPath;
                if (pending != null) {
                    m_pendingPmxPath = null;
                    doLoadModel(pending);
                }
            }

            @Override
            public void onSurfaceChanged(GL10 gl, int width, int height) {
                m_renderer.nativeSurfaceChanged(width, height);
                // Recover if onSurfaceCreated was somehow skipped.
                if (!m_glReady && width > 0 && height > 0) {
                    boolean ok = m_renderer.nativeInit(width, height);
                    if (ok) {
                        m_glReady = true;
                        String pending = m_pendingPmxPath;
                        if (pending != null) {
                            m_pendingPmxPath = null;
                            doLoadModel(pending);
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

        // Speech bubble overlay
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

    // ─────────────────────────────────────────────────────────────────────
    // Model loading — always on the GL thread, never on the UI thread
    // ─────────────────────────────────────────────────────────────────────

    /**
     * Request a model load.  Safe to call from any thread.
     *
     * If the GL context is already ready the load is enqueued on the GL thread
     * immediately.  If not, the path is stored and picked up in onSurfaceCreated().
     *
     * The duplicate-load guard (m_modelLoading) prevents concurrent or repeated
     * calls from triggering multiple loads on fast restarts.
     */
    public void loadModel(String pmxPath) {
        if (pmxPath == null || pmxPath.isEmpty()) return;

        if (!m_modelLoading.compareAndSet(false, true)) {
            Log.w(TAG, "loadModel: load already in progress — ignoring duplicate call");
            return;
        }

        if (m_glReady) {
            m_glView.queueEvent(() -> doLoadModel(pmxPath));
        } else {
            // GL thread not ready yet — store for onSurfaceCreated().
            m_pendingPmxPath = pmxPath;
            // Note: m_modelLoading stays true until doLoadModel() is eventually called.
        }
    }

    /**
     * Must be called ONLY from the GL thread.
     * Performs the actual nativeLoadModel call and clears the loading guard.
     */
    private void doLoadModel(String pmxPath) {
        try {
            Log.i(TAG, "doLoadModel: " + pmxPath);
            m_renderer.nativeLoadModel(pmxPath);
        } finally {
            m_modelLoading.set(false);
        }
    }

    /** Enqueue an arbitrary GL-thread event.  Used by OverlayService. */
    public void queueGLEvent(Runnable r) {
        m_glView.queueEvent(r);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Scale helpers
    // ─────────────────────────────────────────────────────────────────────

    private void applyScale() {
        m_glPixW = (int)(GL_W_BASE * m_currentScale);
        m_glPixH = (int)(GL_H_BASE * m_currentScale);
        m_uiHandler.post(this::updateGLViewSize);
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
        m_params.width  = m_glPixW;
        m_params.height = m_glPixH;
        if (m_wm != null) {
            try { m_wm.updateViewLayout(this, m_params); } catch (Exception ignored) {}
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Screen bounds
    // ─────────────────────────────────────────────────────────────────────

    private int[] getScreenBounds() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            android.graphics.Rect bounds = m_wm.getCurrentWindowMetrics().getBounds();
            return new int[]{ 0, 0, bounds.width(), bounds.height() };
        }
        DisplayMetrics dm = new DisplayMetrics();
        if (m_wm != null) m_wm.getDefaultDisplay().getRealMetrics(dm);
        return new int[]{ 0, 0, dm.widthPixels, dm.heightPixels };
    }

    private int[] clampToScreen(int desiredX, int desiredY) {
        int[] s  = getScreenBounds();
        int maxX = s[2] - m_glPixW;
        int maxY = s[3] - m_glPixH;
        return new int[]{ Math.max(s[0], Math.min(maxX, desiredX)),
                          Math.max(s[1], Math.min(maxY, desiredY)) };
    }

    // ─────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────

    public void applySettings() {
        SharedPreferences prefs = getContext().getSharedPreferences(
                PREFS_SETTINGS, Context.MODE_PRIVATE);
        m_currentScale = prefs.getFloat(KEY_SCALE, 1.0f);
        float opacity  = prefs.getFloat(KEY_OPACITY, 1.0f);
        applyScale();
        if (m_glReady) {
            m_glView.queueEvent(() -> m_renderer.nativeSetTransform(0, 0, 1.0f, opacity));
        }
    }

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

    // ─────────────────────────────────────────────────────────────────────
    // Touch
    // ─────────────────────────────────────────────────────────────────────

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        m_scaleDetector.onTouchEvent(event);
        if (m_scaleDetector.isInProgress()) return true;

        float rawX = event.getRawX();
        float rawY = event.getRawY();

        if (m_glReady) {
            final float lx  = event.getX();
            final float ly  = event.getY();
            final int   act = event.getAction();
            m_glView.queueEvent(() -> m_renderer.nativeTouchEvent(lx, ly, act));
        }

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                m_touchStartRawX  = rawX;
                m_touchStartRawY  = rawY;
                m_initParamX      = m_params.x;
                m_initParamY      = m_params.y;
                m_lastRawX        = rawX;
                m_lastRawY        = rawY;
                m_lastMoveTimeNs  = System.nanoTime();
                return true;

            case MotionEvent.ACTION_MOVE:
                if (!m_positionLocked && event.getPointerCount() == 1) {
                    int desiredX = m_initParamX + (int)(rawX - m_touchStartRawX);
                    int desiredY = m_initParamY + (int)(rawY - m_touchStartRawY);
                    int[] clamped = clampToScreen(desiredX, desiredY);
                    m_params.x = clamped[0];
                    m_params.y = clamped[1];
                    if (m_wm != null) {
                        try { m_wm.updateViewLayout(this, m_params); }
                        catch (Exception ignored) {}
                    }

                    // ── Compute drag velocity for jiggle physics ──────────
                    // velocity (pixels/second) = displacement / elapsed time
                    long nowNs = System.nanoTime();
                    long dtNs  = nowNs - m_lastMoveTimeNs;
                    if (dtNs > 0) {
                        float dtSec = dtNs / 1_000_000_000f;
                        final float vx = (rawX - m_lastRawX) / dtSec;
                        final float vy = (rawY - m_lastRawY) / dtSec;
                        if (m_glReady) {
                            m_glView.queueEvent(() ->
                                    m_renderer.nativeSetDragVelocity(vx, vy));
                        }
                    }
                    m_lastMoveTimeNs = nowNs;
                    m_lastRawX       = rawX;
                    m_lastRawY       = rawY;
                }
                return true;

            case MotionEvent.ACTION_UP:
                float dx = rawX - m_touchStartRawX;
                float dy = rawY - m_touchStartRawY;
                if (Math.abs(dx) < HEADPAT_SLOP_PX && Math.abs(dy) < HEADPAT_SLOP_PX)
                    onHeadpat();
                savePosition(m_params.x, m_params.y);
                // Reset velocity to zero — Bullet physics will damp out naturally.
                if (m_glReady) {
                    m_glView.queueEvent(() ->
                            m_renderer.nativeSetDragVelocity(0f, 0f));
                }
                return true;
        }
        return super.onTouchEvent(event);
    }

    private void onHeadpat() {
        m_affinity.onHeadpat();
        if (m_glReady) {
            // nativeOnTouch() immediately interrupts any waiting/dance animation
            // and plays a random file from the "touch" category (Layer 1 event).
            m_glView.queueEvent(() -> m_renderer.nativeOnTouch());
        }
        showBubble(m_ai.processInput("headpat", m_affinity.getTier()));
    }

    private void savePosition(int x, int y) {
        getContext().getSharedPreferences("overlay_state", Context.MODE_PRIVATE)
                .edit().putInt("pos_x", x).putInt("pos_y", y).apply();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        m_glView.onResume();
    }

    /**
     * SIGSEGV fix for Adreno (msmnile) gralloc crash in eglDestroySurface.
     *
     * Root cause:
     *   queueEvent(nativeDestroy) is asynchronous — calling onPause() right
     *   after queuing it lets onPause() destroy the EGL surface while the GL
     *   thread is still mid-render.  The Adreno gralloc ReleaseBuffer() then
     *   dereferences a partially-freed native_handle → SIGSEGV SEGV_ACCERR.
     *
     * Fix:
     *   1. m_glReady = false  — onDrawFrame() stops submitting new frames.
     *   2. Queue nativeDestroy() and block on a CountDownLatch until it finishes.
     *   3. Only then call onPause(), which destroys the EGL surface safely.
     */

    /** BUG-3: Switch render mode to save battery at night. */
    public void setPowerSave(boolean enabled) {
        if (enabled) m_glView.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        else         m_glView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    }

    @Override
    protected void onDetachedFromWindow() {
        m_glReady = false;

        final CountDownLatch latch = new CountDownLatch(1);
        m_glView.queueEvent(() -> {
            m_renderer.nativeDestroy();
            latch.countDown();
        });

        // CRASH-4: await on a background thread — blocking main thread for 2 s risks ANR.
        // onPause() is called from the background thread once nativeDestroy completes.
        new Thread(() -> {
            try {
                if (!latch.await(2, TimeUnit.SECONDS)) {
                    Log.w(TAG, "nativeDestroy did not complete within 2 s — proceeding anyway");
                }
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            m_glView.onPause();
        }, "gl-destroy").start();

        super.onDetachedFromWindow();
    }
}
