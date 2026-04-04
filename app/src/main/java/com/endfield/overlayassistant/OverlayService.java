package com.endfield.overlayassistant;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.WindowManager;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;

import java.io.File;
import java.util.Calendar;

public class OverlayService extends Service {

    private static final String CHANNEL_ID = "endfield_overlay";
    private static final int    NOTIF_ID   = 1001;

    public static final String EXTRA_PMX_PATH  = "pmx_path";
    public static final String EXTRA_CHAR_NAME = "char_name";

    private static final String MOTIONS_BASE   = "/sdcard/Documents/Assistant/Models/";
    private static final long   TICK_INTERVAL  = 60_000L;

    // Delay before loading motions — gives GL thread time to finish nativeLoadModel
    private static final long MOTIONS_LOAD_DELAY = 2_000L;

    private WindowManager   m_windowManager;
    private OverlayView     m_overlayView;
    private NativeRenderer  m_nativeRenderer;
    private AffinityManager m_affinity;
    private IAiAssistant    m_ai;

    private Handler m_handler;
    private String  m_charName  = "DefaultChar";
    private boolean m_nightMode = false;

    private final Runnable m_tickRunnable = () -> {
        m_affinity.onSessionTick();
        checkNightMode();
        m_handler.postDelayed(m_tickRunnable, TICK_INTERVAL);
    };

    @Override
    public void onCreate() {
        super.onCreate();
        m_handler        = new Handler(Looper.getMainLooper());
        m_nativeRenderer = new NativeRenderer();
        m_affinity       = new AffinityManager(this);
        m_ai             = new AiAssistantImpl(this);
        m_affinity.setOnTierChangedListener(m_nativeRenderer::nativeSetAffinityTier);
        createNotificationChannel();
        startForeground(NOTIF_ID, buildNotification());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && m_overlayView == null) {
            m_charName = intent.getStringExtra(EXTRA_CHAR_NAME);
            if (m_charName == null) m_charName = "DefaultChar";
            String pmxPath = intent.getStringExtra(EXTRA_PMX_PATH);
            addOverlayWindow(pmxPath);
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        m_handler.removeCallbacksAndMessages(null);
        removeOverlayWindow();
        super.onDestroy();
    }

    @Nullable @Override
    public IBinder onBind(Intent i) { return null; }

    // ── Window management ──────────────────────────────────────────────────

    private void addOverlayWindow(String pmxPath) {
        m_windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                PixelFormat.TRANSLUCENT);

        SharedPreferences prefs = getSharedPreferences("overlay_state", MODE_PRIVATE);
        params.x = prefs.getInt("pos_x", 0);
        params.y = prefs.getInt("pos_y", 200);

        // OverlayView constructor and addView must run on UI/main thread — OK here
        // because onStartCommand is called on the main thread.
        m_overlayView = new OverlayView(this, m_nativeRenderer, m_affinity, m_ai, params);
        m_windowManager.addView(m_overlayView, params);

        if (pmxPath != null && !pmxPath.isEmpty()) {
            // nativeLoadModel is queued to GL thread inside loadModel()
            m_overlayView.loadModel(pmxPath);

            // Load motions after a delay to let nativeLoadModel finish on GL thread.
            // 2 s is conservative but safe for a 5 MB model on mid-range devices.
            final String charName = m_charName;
            m_handler.postDelayed(() -> loadMotionsForCharacter(charName), MOTIONS_LOAD_DELAY);
        }

        m_handler.postDelayed(m_tickRunnable, TICK_INTERVAL);
        checkNightMode();
    }

    private void removeOverlayWindow() {
        if (m_overlayView != null && m_windowManager != null) {
            m_windowManager.removeView(m_overlayView);
            m_overlayView = null;
        }
    }

    // ── Motion loading ─────────────────────────────────────────────────────

    private void loadMotionsForCharacter(String charName) {
        if (m_overlayView == null) return;

        String base = MOTIONS_BASE + charName + "/motions/";
        String[] categories = {"idle", "touch", "night", "friend"};
        boolean anyLoaded = false;

        for (String cat : categories) {
            File folder = new File(base + cat);
            if (!folder.isDirectory()) continue;
            File[] vmds = folder.listFiles((d, n) -> n.toLowerCase().endsWith(".vmd"));
            if (vmds == null) continue;

            for (File vmd : vmds) {
                final String path = vmd.getAbsolutePath();
                final String category = cat;
                // nativeLoadMotion must run on GL thread — it accesses MMDModel
                m_overlayView.queueGLEvent(() ->
                    m_nativeRenderer.nativeLoadMotion(path, category));
                anyLoaded = true;
            }
        }

        if (anyLoaded) {
            m_overlayView.queueGLEvent(() ->
                m_nativeRenderer.nativePlayMotionCategory("idle"));
        }
    }

    // ── Night mode ─────────────────────────────────────────────────────────

    private void checkNightMode() {
        int hour  = Calendar.getInstance().get(Calendar.HOUR_OF_DAY);
        boolean night = (hour < 6);

        if (night && !m_nightMode) {
            m_nightMode = true;
            if (m_overlayView != null
                    && m_affinity.getTier() >= AffinityManager.TIER_PARTNER) {
                m_nativeRenderer.nativePlayMotionCategory("night");
                m_overlayView.showBubble(m_ai.getTimeGreeting(hour));
            }
        } else if (!night && m_nightMode) {
            m_nightMode = false;
            m_nativeRenderer.nativePlayMotionCategory("idle");
        }
        m_nativeRenderer.nativeSetPowerSave(night);
    }

    // ── Notification ───────────────────────────────────────────────────────

    private void createNotificationChannel() {
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Endfield Overlay", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(ch);
    }

    private Notification buildNotification() {
        PendingIntent openPi = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        PendingIntent stopPi = PendingIntent.getService(this, 1,
                new Intent(this, OverlayService.class).setAction("STOP"),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Endfield Overlay")
                .setContentText("Tap to open · " + m_charName)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentIntent(openPi)
                .addAction(android.R.drawable.ic_media_pause, "Stop", stopPi)
                .setOngoing(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }
}
