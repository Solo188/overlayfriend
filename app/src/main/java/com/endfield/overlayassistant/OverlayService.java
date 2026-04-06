package com.endfield.overlayassistant;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.os.FileObserver;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowManager;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;

import java.io.File;
import java.util.Calendar;
import java.util.HashSet;
import java.util.Set;

public class OverlayService extends Service {

    private static final String TAG        = "OverlayService";
    private static final String CHANNEL_ID = "endfield_overlay";
    private static final int    NOTIF_ID   = 1001;

    public static final String EXTRA_PMX_PATH          = "pmx_path";
    public static final String EXTRA_CHAR_NAME         = "char_name";
    public static final String ACTION_REFRESH_SETTINGS = "REFRESH_SETTINGS";

    private static final String MOTIONS_BASE  = "/sdcard/Documents/Assistant/Models/";

    /**
     * Directory scanned for user-supplied VMD files.
     * Drop any .vmd file here and the model will start playing it automatically.
     */
    private static final String USER_MOTION_DIR = "/sdcard/Documents/Assistant/motion/";

    private static final long TICK_INTERVAL    = 60_000L;
    private static final long MOTIONS_LOAD_DELAY = 2_000L;

    private WindowManager   m_windowManager;
    private OverlayView     m_overlayView;
    private NativeRenderer  m_nativeRenderer;
    private AffinityManager m_affinity;
    private IAiAssistant    m_ai;

    private Handler m_handler;
    private String  m_charName  = "DefaultChar";
    private boolean m_nightMode = false;

    // ── User motion folder watcher ─────────────────────────────────────────
    // Tracks which VMD paths have already been loaded so we don't re-load
    // files that were already registered with the native engine.
    private final Set<String> m_loadedUserVmds = new HashSet<>();
    private FileObserver      m_motionObserver;

    private final Runnable m_tickRunnable = new Runnable() {
        @Override
        public void run() {
            m_affinity.onSessionTick();
            checkNightMode();
            m_handler.postDelayed(this, TICK_INTERVAL);
        }
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
        if (intent != null) {
            if (ACTION_REFRESH_SETTINGS.equals(intent.getAction())) {
                if (m_overlayView != null) m_overlayView.applySettings();
                return START_STICKY;
            }
            if (m_overlayView == null) {
                m_charName = intent.getStringExtra(EXTRA_CHAR_NAME);
                if (m_charName == null) m_charName = "DefaultChar";
                String pmxPath = intent.getStringExtra(EXTRA_PMX_PATH);
                addOverlayWindow(pmxPath);
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        m_handler.removeCallbacksAndMessages(null);
        stopMotionObserver();
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

        // Use TOP|LEFT gravity so that params.x/y are screen-relative pixel offsets
        // from the top-left corner.  Without this Android defaults to Gravity.CENTER,
        // which makes (0,0) the screen centre and breaks boundary clamping.
        params.gravity = Gravity.TOP | Gravity.LEFT;

        SharedPreferences prefs = getSharedPreferences("overlay_state", MODE_PRIVATE);
        params.x = prefs.getInt("pos_x", 0);
        params.y = prefs.getInt("pos_y", 200);

        m_overlayView = new OverlayView(this, m_nativeRenderer, m_affinity, m_ai, params);
        m_windowManager.addView(m_overlayView, params);

        if (pmxPath != null && !pmxPath.isEmpty()) {
            m_overlayView.loadModel(pmxPath);

            final String charName = m_charName;
            m_handler.postDelayed(() -> {
                loadMotionsForCharacter(charName);
                // Scan the user motion folder after character motions are loaded
                scanUserMotionFolder();
                // Start watching for new VMD files dropped into the folder
                startMotionObserver();
            }, MOTIONS_LOAD_DELAY);
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

    // ── Character motion loading ───────────────────────────────────────────

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
                final String path     = vmd.getAbsolutePath();
                final String category = cat;
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

    // ── User VMD folder scanning ───────────────────────────────────────────

    /**
     * Scans USER_MOTION_DIR for .vmd files that haven't been loaded yet,
     * registers each with the native engine as category "user", then
     * triggers playback.  Safe to call multiple times — skips already-loaded files.
     */
    private void scanUserMotionFolder() {
        if (m_overlayView == null) return;

        File dir = new File(USER_MOTION_DIR);
        if (!dir.exists()) {
            // Create the folder so the user can easily find it
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        if (!dir.isDirectory()) return;

        File[] vmds = dir.listFiles((d, n) -> n.toLowerCase().endsWith(".vmd"));
        if (vmds == null || vmds.length == 0) return;

        boolean anyNew = false;
        for (File vmd : vmds) {
            String absPath = vmd.getAbsolutePath();
            if (m_loadedUserVmds.contains(absPath)) continue;

            m_loadedUserVmds.add(absPath);
            final String path = absPath;
            Log.i(TAG, "Loading user VMD: " + path);
            m_overlayView.queueGLEvent(() ->
                    m_nativeRenderer.nativeLoadMotion(path, "user"));
            anyNew = true;
        }

        if (anyNew) {
            // Switch to playing user motions (they loop with 10 s pause in VMDManager)
            m_overlayView.queueGLEvent(() ->
                    m_nativeRenderer.nativePlayMotionCategory("user"));
        }
    }

    // ── FileObserver — watch for new VMD files in real time ───────────────

    private void startMotionObserver() {
        stopMotionObserver();

        File dir = new File(USER_MOTION_DIR);
        if (!dir.exists()) dir.mkdirs();

        // FileObserver fires on the calling thread pool; dispatch to main thread.
        m_motionObserver = new FileObserver(USER_MOTION_DIR,
                FileObserver.CREATE | FileObserver.MOVED_TO) {
            @Override
            public void onEvent(int event, @Nullable String path) {
                if (path == null) return;
                if (!path.toLowerCase().endsWith(".vmd")) return;

                Log.i(TAG, "FileObserver: new VMD detected: " + path);
                // Give the OS a moment to finish writing the file before reading it
                m_handler.postDelayed(() -> scanUserMotionFolder(), 500);
            }
        };
        m_motionObserver.startWatching();
        Log.i(TAG, "Watching for VMD files in: " + USER_MOTION_DIR);
    }

    private void stopMotionObserver() {
        if (m_motionObserver != null) {
            m_motionObserver.stopWatching();
            m_motionObserver = null;
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
