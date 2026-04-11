package com.endfield.overlayassistant;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.widget.Button;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";

    private Switch   m_switchOverlay;
    private Switch   m_switchNotifs;
    private Button   m_btnStartStop;
    private Button   m_btnSettings;
    private Button   m_btnCreator;
    private Button   m_btnCopyPath;
    private Button   m_btnOpenFolder;
    private TextView m_tvAffinity;
    private TextView m_tvCharSelected;

    private TextView m_iconPmx,      m_statusPmx;
    private TextView m_iconTextures, m_statusTextures;
    private TextView m_iconIdle,     m_statusIdle;
    private TextView m_iconPoses,    m_statusPoses;
    private TextView m_iconWaiting,  m_statusWaiting;
    private TextView m_iconDance,    m_statusDance;
    private TextView m_iconTouch,    m_statusTouch;

    private AffinityManager m_affinity;
    private boolean         m_serviceRunning = false;
    private boolean         m_modelReady     = false;

    private final ActivityResultLauncher<Intent> m_overlayPermLauncher =
        registerForActivityResult(new ActivityResultContracts.StartActivityForResult(),
            result -> refreshPermissionSwitches());

    private final ActivityResultLauncher<String> m_notifPermLauncher =
        registerForActivityResult(new ActivityResultContracts.RequestPermission(),
            granted -> refreshPermissionSwitches());

    private final ActivityResultLauncher<Intent> m_storagePermLauncher =
        registerForActivityResult(new ActivityResultContracts.StartActivityForResult(),
            result -> { ensureFolderStructure(); refreshAll(); });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        IntegrityGuard.verify(this);
        setContentView(R.layout.activity_main);

        m_affinity = new AffinityManager(this);
        bindViews();
        setupListeners();
        refreshPermissionSwitches();

        // Create folder tree immediately on launch.
        // Even if storage permission is missing mkdirs() fails silently —
        // the UI will show "missing" rows, guiding the user to grant access.
        ensureFolderStructure();
        requestStoragePermissionIfNeeded();
    }

    @Override
    protected void onResume() {
        super.onResume();
        m_serviceRunning = OverlayService.sRunning;
        m_btnStartStop.setText(m_serviceRunning ? R.string.btn_stop : R.string.btn_start);
        refreshAll();
    }

    // ── View binding ───────────────────────────────────────────────────────────

    private void bindViews() {
        m_switchOverlay  = findViewById(R.id.switch_overlay_perm);
        m_switchNotifs   = findViewById(R.id.switch_notif_perm);
        m_btnStartStop   = findViewById(R.id.btn_start_stop);
        m_btnSettings    = findViewById(R.id.btn_settings);
        m_btnCreator     = findViewById(R.id.btn_creator);
        m_btnCopyPath    = findViewById(R.id.btn_copy_path);
        m_btnOpenFolder  = findViewById(R.id.btn_open_folder);
        m_tvAffinity     = findViewById(R.id.tv_affinity);
        m_tvCharSelected = findViewById(R.id.tv_char_selected);

        m_iconPmx        = findViewById(R.id.icon_pmx);
        m_statusPmx      = findViewById(R.id.status_pmx);
        m_iconTextures   = findViewById(R.id.icon_textures);
        m_statusTextures = findViewById(R.id.status_textures);
        m_iconIdle       = findViewById(R.id.icon_idle);
        m_statusIdle     = findViewById(R.id.status_idle);
        m_iconPoses      = findViewById(R.id.icon_poses);
        m_statusPoses    = findViewById(R.id.status_poses);
        m_iconWaiting    = findViewById(R.id.icon_waiting);
        m_statusWaiting  = findViewById(R.id.status_waiting);
        m_iconDance      = findViewById(R.id.icon_dance);
        m_statusDance    = findViewById(R.id.status_dance);
        m_iconTouch      = findViewById(R.id.icon_touch);
        m_statusTouch    = findViewById(R.id.status_touch);
    }

    // ── Listeners ──────────────────────────────────────────────────────────────

    private void setupListeners() {
        m_switchOverlay.setOnCheckedChangeListener((v, checked) -> {
            if (checked && !Settings.canDrawOverlays(this)) {
                m_overlayPermLauncher.launch(new Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            }
        });

        m_switchNotifs.setOnCheckedChangeListener((v, checked) -> {
            if (checked && ContextCompat.checkSelfPermission(this,
                    android.Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                m_notifPermLauncher.launch(android.Manifest.permission.POST_NOTIFICATIONS);
            }
        });

        m_btnStartStop.setOnClickListener(v -> toggleService());

        m_btnSettings.setOnClickListener(v ->
            startActivity(new Intent(this,
                com.endfield.overlayassistant.settings.SettingsActivity.class)));

        m_btnCreator.setOnClickListener(v ->
            startActivity(new Intent(Intent.ACTION_VIEW,
                Uri.parse(IntegrityGuard.getCreatorUrl()))));

        m_btnCopyPath.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("path", OverlayService.ASSISTANT_BASE));
            Toast.makeText(this, R.string.toast_path_copied, Toast.LENGTH_SHORT).show();
        });

        m_btnOpenFolder.setOnClickListener(v -> openFolderInFiles());
    }

    // ── Folder creation ────────────────────────────────────────────────────────

    /**
     * Create the complete folder tree under /sdcard/Documents/Assistant/.
     * Safe to call multiple times — mkdirs() is a no-op on existing dirs.
     * Shows a toast only when at least one folder is newly created.
     */
    private void ensureFolderStructure() {
        String[] dirs = {
            OverlayService.ASSISTANT_BASE + "textures",
            OverlayService.ASSISTANT_BASE + "motions/idle",
            OverlayService.ASSISTANT_BASE + "motions/poses",
            OverlayService.ASSISTANT_BASE + "motions/waiting",
            OverlayService.ASSISTANT_BASE + "motions/dance",
            OverlayService.ASSISTANT_BASE + "motions/touch",
        };
        boolean anyCreated = false;
        for (String path : dirs) {
            File f = new File(path);
            if (!f.exists() && f.mkdirs()) {
                Log.i(TAG, "Created: " + path);
                anyCreated = true;
            }
        }
        if (anyCreated) {
            Toast.makeText(this, R.string.toast_folders_created, Toast.LENGTH_SHORT).show();
        }
    }

    // ── Status refresh ─────────────────────────────────────────────────────────

    private void refreshAll() {
        refreshPermissionSwitches();
        refreshFolderStatus();
        checkModelFile();
        updateAffinityDisplay();
    }

    /**
     * Scan each path and update icon + status label:
     *   green  "✓ OK"       — file present / folder has matching files
     *   orange "⚠ empty"    — folder exists but has no .vmd files
     *   red    "✗ missing"  — path does not exist
     */
    private void refreshFolderStatus() {
        // PMX model file
        File pmx = new File(OverlayService.PMX_PATH);
        setStatus(m_iconPmx, m_statusPmx, pmx.exists() ? Status.OK : Status.MISSING, 0);

        // textures/ folder — just check existence
        File texDir = new File(OverlayService.ASSISTANT_BASE + "textures");
        setStatus(m_iconTextures, m_statusTextures,
                  texDir.isDirectory() ? Status.OK : Status.MISSING, 0);

        // Motion folders — count .vmd files
        refreshMotionRow(m_iconIdle,    m_statusIdle,    "idle");
        refreshMotionRow(m_iconPoses,   m_statusPoses,   "poses");
        refreshMotionRow(m_iconWaiting, m_statusWaiting, "waiting");
        refreshMotionRow(m_iconDance,   m_statusDance,   "dance");
        refreshMotionRow(m_iconTouch,   m_statusTouch,   "touch");
    }

    private void refreshMotionRow(TextView icon, TextView status, String category) {
        File dir = new File(OverlayService.ASSISTANT_BASE + "motions/" + category);
        if (!dir.isDirectory()) { setStatus(icon, status, Status.MISSING, 0); return; }
        File[] vmds = dir.listFiles((d, n) -> n.toLowerCase().endsWith(".vmd"));
        int count = (vmds != null) ? vmds.length : 0;
        setStatus(icon, status, count > 0 ? Status.OK : Status.EMPTY, count);
    }

    private enum Status { OK, EMPTY, MISSING }

    private void setStatus(TextView icon, TextView label, Status s, int fileCount) {
        switch (s) {
            case OK:
                icon.setText("●");  icon.setTextColor(0xFF00FF41);
                label.setText(fileCount > 0
                    ? getString(R.string.status_files, fileCount)
                    : getString(R.string.status_ok));
                label.setTextColor(0xFF00FF41);
                break;
            case EMPTY:
                icon.setText("●");  icon.setTextColor(0xFFFF9900);
                label.setText(R.string.status_empty);
                label.setTextColor(0xFFFF9900);
                break;
            case MISSING:
                icon.setText("○");  icon.setTextColor(0xFF555555);
                label.setText(R.string.status_missing);
                label.setTextColor(0xFFFF4444);
                break;
        }
    }

    // ── Open folder in Files app ───────────────────────────────────────────────

    private void openFolderInFiles() {
        // Try the standard Documents/Assistant path via external storage provider
        try {
            Uri folderUri = Uri.parse(
                "content://com.android.externalstorage.documents/document/" +
                "primary%3ADocuments%2FAssistant");
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(folderUri, "vnd.android.document/directory");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(intent);
            return;
        } catch (Exception ignored) {}

        // Fallback: MIUI File Manager
        try {
            Intent intent = new Intent();
            intent.setClassName("com.mi.android.globalFileexplorer",
                                "com.mi.android.globalFileexplorer.mop.FileExplorerTabActivity");
            startActivity(intent);
            return;
        } catch (Exception ignored) {}

        // Last resort: show path in toast
        Toast.makeText(this, "Open: " + OverlayService.ASSISTANT_BASE, Toast.LENGTH_LONG).show();
    }

    // ── Permission & service ───────────────────────────────────────────────────

    private void requestStoragePermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Toast.makeText(this,
                    "Storage permission needed to read Yvonne.pmx",
                    Toast.LENGTH_LONG).show();
                m_storagePermLauncher.launch(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
            } else {
                refreshAll();
            }
        } else {
            refreshAll();
        }
    }

    private void toggleService() {
        if (!Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Overlay permission required", Toast.LENGTH_SHORT).show();
            return;
        }
        if (!m_modelReady) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && !Environment.isExternalStorageManager()) {
                Toast.makeText(this, "Grant 'All Files Access' first", Toast.LENGTH_LONG).show();
                requestStoragePermissionIfNeeded();
            } else {
                Toast.makeText(this,
                    "Yvonne.pmx not found in " + OverlayService.ASSISTANT_BASE,
                    Toast.LENGTH_LONG).show();
            }
            return;
        }
        if (m_serviceRunning) {
            stopService(new Intent(this, OverlayService.class));
            m_serviceRunning = false;
            m_btnStartStop.setText(R.string.btn_start);
        } else {
            startForegroundService(new Intent(this, OverlayService.class));
            m_serviceRunning = true;
            m_btnStartStop.setText(R.string.btn_stop);
        }
    }

    private void checkModelFile() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && !Environment.isExternalStorageManager()) {
            m_tvCharSelected.setText("⚠ Grant 'All Files Access' in settings above");
            m_modelReady = false;
            return;
        }
        File pmx = new File(OverlayService.PMX_PATH);
        m_modelReady = pmx.exists();
        m_tvCharSelected.setText(m_modelReady
            ? getString(R.string.selected_char, "Yvonne")
            : "Place Yvonne.pmx in " + OverlayService.ASSISTANT_BASE);
    }

    private void refreshPermissionSwitches() {
        m_switchOverlay.setChecked(Settings.canDrawOverlays(this));
        m_switchNotifs.setChecked(ContextCompat.checkSelfPermission(this,
            android.Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED);
    }

    private void updateAffinityDisplay() {
        m_tvAffinity.setText(getString(R.string.affinity_display,
            m_affinity.getScore(), m_affinity.getTierName()));
    }
}
