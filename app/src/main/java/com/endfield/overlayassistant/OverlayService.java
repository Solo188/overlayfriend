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

    public static final String ACTION_REFRESH_SETTINGS = "REFRESH_SETTINGS";

    // ── Folder layout ──────────────────────────────────────────────────────
    //
    //   /sdcard/Documents/Assistant/
    //   ├── Yvonne.pmx
    //   ├── textures/
    //   └── motions/
    //       ├── idle/      (constant idle / breath animations)
    //       ├── poses/     (static poses used as idle base)
    //       ├── waiting/   (random events, 70 % chance every 1-10 min)
    //       ├── dance/     (random events, 30 % chance every 1-10 min)
    //       └── touch/     (reactions on finger tap)
    //
    static final String ASSISTANT_BASE = "/sdcard/Documents/Assistant/";
    static final String PMX_PATH       = ASSISTANT_BASE + "Yvonne.pmx";

    // Delay between model load and motion scan (waits for GL to initialise).
    private static final long MOTIONS_LOAD_DELAY = 2_000L;
    private static final long TICK_INTERVAL      = 60_000L;

    private WindowManager   m_windowManager;
    private OverlayView     m_overlayView;
    private NativeRenderer  m_nativeRenderer;
    private AffinityManager m_affinity;
    private IAiAssistant    m_ai;

    private Handler m_handler;
    private boolean m_nightMode = false;

    // Tracks user-dropped VMD paths that are already registered with the engine.
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

    // ── Lifecycle ──────────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();

        // !! MUST happen first — Android gives 5 s before raising
        // ForegroundServiceDidNotStartInTimeException.  Create the channel
        // synchronously then call startForeground() before any slow init.
        createNotificationChannel();
        startForeground(NOTIF_ID, buildNotification());

        // Heavy initialisation happens AFTER startForeground().
        m_handler        = new Handler(Looper.getMainLooper());
        m_nativeRenderer = new NativeRenderer();
        m_affinity       = new AffinityManager(this);
        m_ai             = new AiAssistantImpl(this);
        m_affinity.setOnTierChangedListener(m_nativeRenderer::nativeSetAffinityTier);

        // Create the full folder tree on first launch so the user knows where
        // to place files without having to dig through the file manager.
        ensureFolderStructure();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Belt-and-suspenders: re-call startForeground() at the very top of
        // onStartCommand() so it fires even if onCreate() was somehow delayed.
        startForeground(NOTIF_ID, buildNotification());

        if (intent == null) return START_STICKY;

        if (ACTION_REFRESH_SETTINGS.equals(intent.getAction())) {
            if (m_overlayView != null) m_overlayView.applySettings();
            return START_STICKY;
        }

        // First real start — add the overlay window.
        if (m_overlayView == null) {
            addOverlayWindow();
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

    // ── Folder structure creation ──────────────────────────────────────────

    /**
     * Create the full /sdcard/Documents/Assistant/ tree on first launch.
     * mkdirs() is a no-op if a folder already exists.
     */
    private void ensureFolderStructure() {
        String[] dirs = {
            ASSISTANT_BASE + "textures",
            ASSISTANT_BASE + "motions/idle",
            ASSISTANT_BASE + "motions/poses",
            ASSISTANT_BASE + "motions/waiting",
            ASSISTANT_BASE + "motions/dance",
            ASSISTANT_BASE + "motions/touch",
        };
        for (String dir : dirs) {
            File f = new File(dir);
            if (!f.exists()) {
                //noinspection ResultOfMethodCallIgnored
                f.mkdirs();
                Log.i(TAG, "Created folder: " + dir);
            }
        }
    }

    // ── Window management ──────────────────────────────────────────────────

    private void addOverlayWindow() {
        m_windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH,
                PixelFormat.TRANSLUCENT);

        // Use TOP|LEFT gravity so params.x/y are screen-relative pixel offsets.
        params.gravity = Gravity.TOP | Gravity.LEFT;

        SharedPreferences prefs = getSharedPreferences("overlay_state", MODE_PRIVATE);
        params.x = prefs.getInt("pos_x", 0);
        params.y = prefs.getInt("pos_y", 200);

        m_overlayView = new OverlayView(this, m_nativeRenderer, m_affinity, m_ai, params);
        m_windowManager.addView(m_overlayView, params);

        // loadModel() enqueues nativeLoadModel on the GL thread — never blocks UI.
        m_overlayView.loadModel(PMX_PATH);

        // After the GL thread has initialised and loaded the model,
        // scan the motions folder.  nativeScanMotions goes via queueGLEvent.
        m_handler.postDelayed(() -> {
            if (m_overlayView == null) return;
            m_overlayView.queueGLEvent(() ->
                    m_nativeRenderer.nativeScanMotions(ASSISTANT_BASE));
            startMotionObserver();
        }, MOTIONS_LOAD_DELAY);

        m_handler.postDelayed(m_tickRunnable, TICK_INTERVAL);
        checkNightMode();
    }

    private void removeOverlayWindow() {
        if (m_overlayView != null && m_windowManager != null) {
            m_windowManager.removeView(m_overlayView);
            m_overlayView = null;
        }
    }

    // ── User VMD folder watcher ────────────────────────────────────────────

    /**
     * Watch for .vmd files dropped into motions sub-folders at runtime.
     * Registers each new file with the engine on the GL thread.
     */
    private void startMotionObserver() {
        stopMotionObserver();

        String watchDir = ASSISTANT_BASE + "motions/";
        File dir = new File(watchDir);
        if (!dir.exists()) dir.mkdirs();

        m_motionObserver = new FileObserver(watchDir,
                FileObserver.CREATE | FileObserver.MOVED_TO) {
            @Override
            public void onEvent(int event, @Nullable String path) {
                if (path == null || !path.toLowerCase().endsWith(".vmd")) return;
                Log.i(TAG, "FileObserver: new VMD detected: " + path);
                m_handler.postDelayed(() -> scanUserVmds(), 500);
            }
        };
        m_motionObserver.startWatching();
        Log.i(TAG, "Watching for VMD files in: " + watchDir);
    }

    private void stopMotionObserver() {
        if (m_motionObserver != null) {
            m_motionObserver.stopWatching();
            m_motionObserver = null;
        }
    }

    /**
     * Walk all motion sub-folders looking for .vmd files not yet registered.
     * New files are loaded into the native engine on the GL thread.
     */
    private void scanUserVmds() {
        if (m_overlayView == null) return;
        String[] subDirs = { "idle", "poses", "waiting", "dance", "touch" };
        for (String cat : subDirs) {
            File folder = new File(ASSISTANT_BASE + "motions/" + cat);
            if (!folder.isDirectory()) continue;
            File[] vmds = folder.listFiles((d, n) -> n.toLowerCase().endsWith(".vmd"));
            if (vmds == null) continue;
            for (File vmd : vmds) {
                String abs = vmd.getAbsolutePath();
                if (m_loadedUserVmds.contains(abs)) continue;
                m_loadedUserVmds.add(abs);
                final String p = abs;
                final String c = cat;
                Log.i(TAG, "Hot-loading VMD [" + c + "]: " + p);
                m_overlayView.queueGLEvent(() ->
                        m_nativeRenderer.nativeLoadMotion(p, c));
            }
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
                .setContentText("Yvonne is running")
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setContentIntent(openPi)
                .addAction(android.R.drawable.ic_media_pause, "Stop", stopPi)
                .setOngoing(true)
                .setPriority(NotificationCompat.PRIORITY_LOW)
                .build();
    }
}
