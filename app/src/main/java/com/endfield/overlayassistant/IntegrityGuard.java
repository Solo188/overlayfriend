package com.endfield.overlayassistant;

import android.content.Context;
import android.os.Process;

/**
 * Облегченная защита: проверяет имя пакета и хранит скрытую ссылку.
 */
public final class IntegrityGuard {

    // Ожидаемое имя пакета — если его поменяют, приложение закроется
    private static final String EXPECTED_PACKAGE = "com.endfield.overlayassistant";

    private IntegrityGuard() {}

    /**
     * Вызывать в MainActivity.onCreate().
     */
    public static void verify(Context ctx) {
        if (!EXPECTED_PACKAGE.equals(ctx.getPackageName())) {
            die();
        }
    }

    // ── Зашифрованная ссылка на разработчика ─────────────────────────────────
    private static final byte[] CREATOR_URL_XOR = {
        0x32,0x2E,0x2E,0x2A,0x29,0x60,0x75,0x75,
        0x2E,0x74,0x37,0x3F,0x75,0x0E,0x32,0x3F,
        0x1C,0x35,0x28,0x3D,0x35,0x2E,0x2E,0x3F,
        0x34,0x12,0x3B,0x28,0x36,0x3F,0x2B,0x2F,
        0x33,0x34,0x63
    };
    private static final byte XOR_KEY = 0x5A;

    /** Возвращает ссылку на создателя (расшифровывает на лету). */
    public static String getCreatorUrl() {
        byte[] dec = new byte[CREATOR_URL_XOR.length];
        for (int i = 0; i < CREATOR_URL_XOR.length; i++) {
            dec[i] = (byte) (CREATOR_URL_XOR[i] ^ XOR_KEY);
        }
        return new String(dec);
    }

    private static void die() {
        Process.killProcess(Process.myPid());
        System.exit(1);
    }
}
