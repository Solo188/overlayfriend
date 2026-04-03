package com.endfield.overlayassistant;

import android.content.Context;
import android.content.SharedPreferences;

public class AffinityManager {

    public static final int TIER_STRANGER = 0;
    public static final int TIER_FRIEND   = 1;
    public static final int TIER_PARTNER  = 2;

    private static final String PREFS_FILE    = "affinity_data";
    private static final String KEY_SCORE     = "affinity_score";
    private static final String KEY_SESSION_S = "session_start_epoch";
    private static final String KEY_TOTAL_MS  = "total_active_ms";

    private static final int POINTS_HEADPAT        = 1;
    private static final int POINTS_PER_30MIN_SESS  = 10;
    private static final long SESSION_INTERVAL_MS   = 30L * 60L * 1000L;

    private final SharedPreferences m_prefs;
    private       int               m_score;
    private       long              m_sessionStartMs;
    private       long              m_lastSessionCheck;

    private OnTierChangedListener   m_tierListener;

    public interface OnTierChangedListener {
        void onTierChanged(int newTier);
    }

    public AffinityManager(Context context) {
        m_prefs          = context.getSharedPreferences(PREFS_FILE, Context.MODE_PRIVATE);
        m_score          = m_prefs.getInt(KEY_SCORE, 0);
        m_sessionStartMs = System.currentTimeMillis();
        m_lastSessionCheck = m_sessionStartMs;
    }

    public void setOnTierChangedListener(OnTierChangedListener l) {
        m_tierListener = l;
    }

    public void onHeadpat() {
        int prevTier = getTier();
        m_score += POINTS_HEADPAT;
        save();
        notifyTierChange(prevTier);
    }

    public void onSessionTick() {
        long now     = System.currentTimeMillis();
        long elapsed = now - m_lastSessionCheck;
        if (elapsed >= SESSION_INTERVAL_MS) {
            int prevTier = getTier();
            m_score += POINTS_PER_30MIN_SESS;
            m_lastSessionCheck = now;
            save();
            notifyTierChange(prevTier);
        }
    }

    public int getScore() { return m_score; }

    public int getTier() {
        if (m_score >= 501) return TIER_PARTNER;
        if (m_score >= 101) return TIER_FRIEND;
        return TIER_STRANGER;
    }

    public String getTierName() {
        switch (getTier()) {
            case TIER_FRIEND:  return "Friend";
            case TIER_PARTNER: return "Partner";
            default:           return "Stranger";
        }
    }

    private void save() {
        m_prefs.edit()
               .putInt(KEY_SCORE, m_score)
               .apply();
    }

    private void notifyTierChange(int prevTier) {
        int newTier = getTier();
        if (newTier != prevTier && m_tierListener != null) {
            m_tierListener.onTierChanged(newTier);
        }
    }
}
