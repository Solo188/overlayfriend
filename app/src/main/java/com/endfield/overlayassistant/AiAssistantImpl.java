package com.endfield.overlayassistant;

import android.content.Context;
import android.content.res.Resources;

import java.util.Random;

public class AiAssistantImpl implements IAiAssistant {

    private final Context  m_context;
    private final Random   m_rng;

    public AiAssistantImpl(Context context) {
        m_context = context.getApplicationContext();
        m_rng     = new Random();
    }

    @Override
    public String processInput(String userInput, int affinityTier) {
        int[] arrayResIds;
        switch (affinityTier) {
            case AffinityManager.TIER_PARTNER:
                arrayResIds = new int[]{
                    R.array.phrases_partner,
                    R.array.phrases_friend,
                    R.array.phrases_stranger
                };
                break;
            case AffinityManager.TIER_FRIEND:
                arrayResIds = new int[]{
                    R.array.phrases_friend,
                    R.array.phrases_stranger
                };
                break;
            default:
                arrayResIds = new int[]{R.array.phrases_stranger};
                break;
        }

        int poolIdx   = m_rng.nextInt(arrayResIds.length);
        int resId     = arrayResIds[poolIdx];
        return pickRandom(resId);
    }

    @Override
    public String getTimeGreeting(int hour) {
        if (hour >= 0 && hour < 6) {
            return pickRandom(R.array.phrases_night);
        } else if (hour < 12) {
            return pickRandom(R.array.phrases_morning);
        } else if (hour < 18) {
            return pickRandom(R.array.phrases_afternoon);
        } else {
            return pickRandom(R.array.phrases_evening);
        }
    }

    private String pickRandom(int arrayResId) {
        Resources res    = m_context.getResources();
        String[]  arr    = res.getStringArray(arrayResId);
        if (arr == null || arr.length == 0) return "...";
        return arr[m_rng.nextInt(arr.length)];
    }
}
