package com.endfield.overlayassistant;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Button;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;

import java.io.File;

public class MainActivity extends AppCompatActivity {

    private static final String MODELS_BASE = "/sdcard/Documents/Assistant/Models/";

    private Switch   m_switchOverlay;
    private Switch   m_switchNotifs;
    private Button   m_btnStartStop;
    private Button   m_btnSettings;
    private TextView m_tvAffinity;
    private TextView m_tvCharSelected;

    private AffinityManager m_affinity;
    private String          m_selectedPmxPath  = "";
    private String          m_selectedCharName = "";
    private boolean         m_serviceRunning   = false;

    private final ActivityResultLauncher<Intent> m_overlayPermLauncher =
        registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(),
            result -> refreshPermissionSwitches()
        );

    private final ActivityResultLauncher<String> m_notifPermLauncher =
        registerForActivityResult(
            new ActivityResultContracts.RequestPermission(),
            granted -> refreshPermissionSwitches()
        );

    // Launcher for MANAGE_EXTERNAL_STORAGE special settings screen
    private final ActivityResultLauncher<Intent> m_storagePermLauncher =
        registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(),
            result -> {
                // After user returns from settings — re-scan models
                scanCharacterFolders();
            }
        );

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        m_affinity = new AffinityManager(this);

        bindViews();
        setupListeners();
        refreshPermissionSwitches();

        // Request full storage access if not granted — needed to read .pmx files
        // from /sdcard/Documents/ which are not media files
        requestStoragePermissionIfNeeded();
    }

    @Override
    protected void onResume() {
        super.onResume();;
        refreshPermissionSwitches();
        scanCharacterFolders();
        updateAffinityDisplay();
    }

    private void bindViews() {
        m_switchOverlay  = findViewById(R.id.switch_overlay_perm);
        m_switchNotifs   = findViewById(R.id.switch_notif_perm);
        m_btnStartStop   = findViewById(R.id.btn_start_stop);
        m_btnSettings    = findViewById(R.id.btn_settings);
        m_tvAffinity     = findViewById(R.id.tv_affinity);
        m_tvCharSelected = findViewById(R.id.tv_char_selected);
    }

    private void setupListeners() {
        m_switchOverlay.setOnCheckedChangeListener((v, checked) -> {
            if (checked && !Settings.canDrawOverlays(this)) {
                Intent intent = new Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
                m_overlayPermLauncher.launch(intent);
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
    }

    /**
     * On Android 11+ (API 30+) reading arbitrary files from /sdcard/Documents/
     * requires MANAGE_EXTERNAL_STORAGE. READ_EXTERNAL_STORAGE only covers
     * media files; .pmx is not a media file so it requires full storage access.
     */
    private void requestStoragePermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Toast.makeText(this,
                    "Storage permission needed to find .pmx models",
                    Toast.LENGTH_LONG).show();
                Intent intent = new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
                m_storagePermLauncher.launch(intent);
            } else {
                // Already have permission — scan immediately
                scanCharacterFolders();
                updateAffinityDisplay();
            }
        } else {
            // API 29 and below — READ_EXTERNAL_STORAGE is enough
            scanCharacterFolders();
            updateAffinityDisplay();
        }
    }

    private void toggleService() {
        if (!Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Overlay permission required", Toast.LENGTH_SHORT).show();
            return;
        }
        if (m_selectedPmxPath.isEmpty()) {
            // Check storage permission first
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && !Environment.isExternalStorageManager()) {
                Toast.makeText(this,
                    "Grant 'All Files Access' permission first",
                    Toast.LENGTH_LONG).show();
                requestStoragePermissionIfNeeded();
                return;
            }
            Toast.makeText(this, "No .pmx model found in Models/", Toast.LENGTH_SHORT).show();
            return;
        }

        if (m_serviceRunning) {
            stopService(new Intent(this, OverlayService.class));
            m_serviceRunning = false;
            m_btnStartStop.setText(R.string.btn_start);
        } else {
            Intent svc = new Intent(this, OverlayService.class);
            svc.putExtra(OverlayService.EXTRA_PMX_PATH,  m_selectedPmxPath);
            svc.putExtra(OverlayService.EXTRA_CHAR_NAME, m_selectedCharName);
            startForegroundService(svc);
            m_serviceRunning = true;
            m_btnStartStop.setText(R.string.btn_stop);
        }
    }

    private void scanCharacterFolders() {
        // Check storage access before scanning
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && !Environment.isExternalStorageManager()) {
            m_tvCharSelected.setText("⚠ Grant 'All Files Access' in settings above");
            return;
        }

        File base = new File(MODELS_BASE);
        if (!base.exists()) {
            m_tvCharSelected.setText(R.string.no_models_found);
            return;
        }

        // Search sub-folders (each is a character folder containing a .pmx)
        File[] chars = base.listFiles(File::isDirectory);
        if (chars != null) {
            for (File charDir : chars) {
                File[] pmxFiles = charDir.listFiles(
                    (d, n) -> n.toLowerCase().endsWith(".pmx"));
                if (pmxFiles != null && pmxFiles.length > 0) {
                    m_selectedCharName = charDir.getName();
                    m_selectedPmxPath  = pmxFiles[0].getAbsolutePath();
                    m_tvCharSelected.setText(
                        getString(R.string.selected_char, m_selectedCharName));
                    return;
                }
            }
        }

        // Also check for .pmx files directly in the Models/ root (flat layout)
        File[] rootPmx = base.listFiles((d, n) -> n.toLowerCase().endsWith(".pmx"));
        if (rootPmx != null && rootPmx.length > 0) {
            m_selectedCharName = rootPmx[0].getName().replaceFirst("\\.pmx$", "");
            m_selectedPmxPath  = rootPmx[0].getAbsolutePath();
            m_tvCharSelected.setText(
                getString(R.string.selected_char, m_selectedCharName));
            return;
        }

        m_tvCharSelected.setText(R.string.no_models_found);
    }

    private void refreshPermissionSwitches() {
        boolean overlay = Settings.canDrawOverlays(this);
        boolean notifs  = ContextCompat.checkSelfPermission(this,
            android.Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED;

        m_switchOverlay.setChecked(overlay);
        m_switchNotifs.setChecked(notifs);
    }

    private void updateAffinityDisplay() {
        int score    = m_affinity.getScore();
        String tier  = m_affinity.getTierName();
        m_tvAffinity.setText(getString(R.string.affinity_display, score, tier));
    }
}
