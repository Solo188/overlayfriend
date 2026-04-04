package com.endfield.overlayassistant.settings;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import com.endfield.overlayassistant.OverlayService;
import com.endfield.overlayassistant.R;

public class SettingsActivity extends AppCompatActivity {

    private static final String PREFS_SETTINGS = "overlay_settings";
    private static final String KEY_SCALE       = "scale";
    private static final String KEY_OPACITY     = "opacity";
    private static final String KEY_SILENT      = "silent_mode";
    private static final String KEY_POS_LOCK    = "position_lock";

    private SeekBar  m_sbScale;
    private SeekBar  m_sbOpacity;
    private Switch   m_swSilent;
    private Switch   m_swPosLock;
    private TextView m_tvScaleValue;
    private TextView m_tvOpacityValue;

    private SharedPreferences m_prefs;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);

        m_prefs = getSharedPreferences(PREFS_SETTINGS, MODE_PRIVATE);

        bindViews();
        loadCurrentValues();
        setupListeners();
    }

    private void bindViews() {
        m_sbScale       = findViewById(R.id.seekbar_scale);
        m_sbOpacity     = findViewById(R.id.seekbar_opacity);
        m_swSilent      = findViewById(R.id.switch_silent);
        m_swPosLock     = findViewById(R.id.switch_pos_lock);
        m_tvScaleValue  = findViewById(R.id.tv_scale_value);
        m_tvOpacityValue = findViewById(R.id.tv_opacity_value);

        m_sbScale.setMax(200);
        m_sbOpacity.setMax(100);
    }

    private void loadCurrentValues() {
        float scale   = m_prefs.getFloat(KEY_SCALE,   1.0f);
        float opacity = m_prefs.getFloat(KEY_OPACITY, 1.0f);
        boolean silent  = m_prefs.getBoolean(KEY_SILENT,   false);
        boolean posLock = m_prefs.getBoolean(KEY_POS_LOCK, false);

        m_sbScale.setProgress((int)((scale - 0.5f) * 100f));
        m_sbOpacity.setProgress((int)(opacity * 100f));
        m_swSilent.setChecked(silent);
        m_swPosLock.setChecked(posLock);

        updateScaleLabel(scale);
        updateOpacityLabel(opacity);
    }

    private void setupListeners() {
        m_sbScale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float scale = 0.5f + (progress / 100f);
                updateScaleLabel(scale);
                if (fromUser) {
                    saveFloat(KEY_SCALE, scale);
                    notifyService();
                }
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        m_sbOpacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float opacity = progress / 100f;
                updateOpacityLabel(opacity);
                if (fromUser) {
                    saveFloat(KEY_OPACITY, opacity);
                    notifyService();
                }
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        m_swSilent.setOnCheckedChangeListener((v, checked) ->
            m_prefs.edit().putBoolean(KEY_SILENT, checked).apply());

        m_swPosLock.setOnCheckedChangeListener((v, checked) ->
            m_prefs.edit().putBoolean(KEY_POS_LOCK, checked).apply());
    }

    private void saveFloat(String key, float value) {
        m_prefs.edit().putFloat(key, value).apply();
    }

    private void notifyService() {
        // Tell OverlayService to re-read scale/opacity from SharedPreferences
        Intent i = new Intent(this, OverlayService.class);
        i.setAction(OverlayService.ACTION_REFRESH_SETTINGS);
        startService(i);
    }

    private void updateScaleLabel(float scale) {
        m_tvScaleValue.setText(String.format("%.2fx", scale));
    }

    private void updateOpacityLabel(float opacity) {
        m_tvOpacityValue.setText(String.format("%d%%", (int)(opacity * 100)));
    }
}
