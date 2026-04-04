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

    private static final String CHANNEL_ID  = "endfield_overlay";
    private static final int    NOTIF_ID    = 1001;

    public static final String EXTRA_PMX_PATH  = "pmx_path";
    public static final String EXTRA_CHAR_NAME = "char_name";

    private static final String MOTIONS_BASE =
            "/sdcard/Documents/Assistant/Models/";

    private static final long TICK_INTERVAL_MS = 60_000L;

    private WindowManager   m_windowManager;
    private OverlayView     m_overlayView;
    private NativeRenderer  m_nativeRenderer;
    private AffinityManager m_affinity;
    private IAiAssistant    m_ai;

    private Handler m_handler;
    private String  m_charName  = "DefaultChar";
    private boolean m_nightMode = false;

    private final Runnable m_tickRunnable = new Runnable() {
        @Override
        public void run() {
            m_affinity.onSessionTick();
            checkNightMode();
            m_handler.postDelayed(this, TICK_INTERVAL_MS);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        m_handler        = new Handler(Looper.getMainLooper());
        m_nativeRenderer = new NativeRenderer();
        m_affinity       = new AffinityManager(this);
        m_ai             = new AiAssistantImpl(this);

        m_affinity.setOnTierChangedListener(tier ->
            m_nativeRenderer.nativeSetAffinityTier(tier));

        createNotificationChannel();
        startForeground(NOTIF_ID, buildNotification());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null) {
            String pmxPath = intent.getStringExtra(EXTRA_PMX_PATH);
            m_charName     = intent.getStringExtra(EXTRA_CHAR_NAME);
            if (m_charName == null) m_charName = "DefaultChar";

            if (m_overlayView == null) {
                addOverlayWindow(pmxPath);
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        m_handler.removeCallbacks(m_tickRunnable);
        removeOverlayWindow();
        super.onDestroy();
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) { return null; }

    private void addOverlayWindow(String pmxPath) {
        m_windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                PixelFormat.TRANSLUCENT
        );

        SharedPreferences prefs = getSharedPreferences("overlay_state", MODE_PRIVATE);
        params.x = prefs.getInt("pos_x", 0);
        params.y = prefs.getInt("pos_y", 200);

        m_overlayView = new OverlayView(this, m_nativeRenderer, m_affinity, m_ai, params);
        m_windowManager.addView(m_overlayView, params);

        // FIX: load model via OverlayView.loadModel() which correctly queues on the GL thread.
        // Load motions only AFTER the model finishes loading — use a slight delay to let
        // the GL thread process the model load first. The native side guards against
        // loadMotion calls before loadModel via getModel() null checks.
        if (pmxPath != null && !pmxPath.isEmpty()) {
            m_overlayView.loadModel(pmxPath);
            // Delay motion loading by 500 ms so the GL thread has time to finish
            // nativeLoadModel before nativeLoadMotion calls arrive.
            m_handler.postDelayed(() -> loadMotionsForCharacter(m_charName), 500);
        }

        m_handler.postDelayed(m_tickRunnable, TICK_INTERVAL_MS);
        checkNightMode();
    }

    private void removeOverlayWindow() {
        if (m_overlayView != null && m_windowManager != null) {
            m_windowManager.removeView(m_overlayView);
            m_overlayView = null;
        }
    }

    private void loadMotionsForCharacter(String charName) {
        if (m_overlayView == null) return;

        String base = MOTIONS_BASE + charName + "/motions/";
        String[] categories = {"idle", "touch", "night", "friend"};

        for (String cat : categories) {
            File folder = new File(base + cat);
            if (!folder.exists() || !folder.isDirectory()) continue;

            File[] vmds = folder.listFiles((dir, name) ->
                    name.toLowerCase().endsWith(".vmd"));
            if (vmds == null) continue;

            for (File vmd : vmds) {
                // FIX: nativeLoadMotion must run on the GL thread (it uses MMDModel
                // which was created on GL thread). Route through OverlayView.queueGLEvent.
                final String vmdPath = vmd.getAbsolutePath();
                final String category = cat;
                m_overlayView.queueGLEvent(() ->
                    m_nativeRenderer.nativeLoadMotion(vmdPath, category));
            }
        }

        // Play idle after all motions queued
        m_overlayView.queueGLEvent(() ->
            m_nativeRenderer.nativePlayMotionCategory("idle"));
    }

    private void checkNightMode() {
        int hour  = Calendar.getInstance().get(Calendar.HOUR_OF_DAY);
        boolean night = (hour >= 0 && hour < 6);

        if (night && !m_nightMode) {
            m_nightMode = true;
            if (m_overlayView != null && m_affinity.getTier() >= AffinityManager.TIER_PARTNER) {
                m_nativeRenderer.nativePlayMotionCategory("night");
                m_overlayView.showBubble(m_ai.getTimeGreeting(hour));
            }
        } else if (!night && m_nightMode) {
            m_nightMode = false;
            m_nativeRenderer.nativePlayMotionCategory("idle");
        }

        m_nativeRenderer.nativeSetPowerSave(night);
    }

    private void createNotificationChannel() {
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID,
                "Endfield Overlay",
                NotificationManager.IMPORTANCE_LOW
        );
        channel.setDescription("Keeps the MMD overlay running");
        channel.setShowBadge(false);
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        Intent stopIntent = new Intent(this, OverlayService.class);
        stopIntent.setAction("STOP");
        PendingIntent stopPi = PendingIntent.getService(
                this, 0, stopIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent openIntent = new Intent(this, MainActivity.class);
        PendingIntent openPi = PendingIntent.getActivity(
                this, 1, openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
                .setContentTitle("Endfield Overlay")
                .setContentText("Running — tap to open settings")
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentIntent(openPi)
                .addAction(android.R.drawable.ic_media_pause, "Stop", stopPi)
                .setOngoing(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }
}
