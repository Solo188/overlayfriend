package com.endfield.overlayassistant;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Process;
import android.widget.Toast;

import java.security.MessageDigest;

/**
 * Защита от перепаковки и перепродажи приложения.
 *
 * Проверяет:
 *  1. Имя пакета — не изменено ли
 *  2. SHA-256 подписи APK — подписано ли оригинальным ключом
 *
 * ПОСЛЕ ПЕРВОЙ СБОРКИ:
 *   Запустите приложение — на экране появится диалог с хэшем.
 *   Скопируйте хэш в EXPECTED_CERT_HASH ниже и пересоберите.
 */
public final class IntegrityGuard {

    // Ожидаемое имя пакета — не менять
    private static final String EXPECTED_PACKAGE = "com.endfield.overlayassistant";

    // SHA-256 подписи вашего APK.
    // Шаг 1: соберите APK и запустите — появится диалог с хэшем.
    // Шаг 2: вставьте хэш сюда и пересоберите.
    // Пока поле пустое — показывается диалог с хэшем (режим настройки).
    private static final String EXPECTED_CERT_HASH = "";

    private IntegrityGuard() {}

    /**
     * Вызывать в MainActivity.onCreate() первым делом.
     * При провале проверки убивает процесс.
     */
    public static void verify(Context ctx) {
        checkPackageName(ctx);
        checkSignature(ctx);
    }

    // ── Зашифрованная ссылка на разработчика ─────────────────────────────────
    // URL хранится в XOR-виде, чтобы не светиться в строках DEX.
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

    // ── Внутренние методы ─────────────────────────────────────────────────────

    private static void checkPackageName(Context ctx) {
        if (!EXPECTED_PACKAGE.equals(ctx.getPackageName())) {
            die();
        }
    }

    @SuppressWarnings("deprecation")
    private static void checkSignature(Context ctx) {
        String hash = getCertHash(ctx);

        if (EXPECTED_CERT_HASH.isEmpty()) {
            // Режим настройки — показать хэш на экране в диалоге
            showHashDialog(ctx, hash != null ? hash : "Ошибка получения хэша");
            return;
        }

        if (!EXPECTED_CERT_HASH.equals(hash)) {
            die();
        }
    }

    /** Показывает диалог с хэшем и кнопкой "Скопировать" */
    private static void showHashDialog(Context ctx, String hash) {
        new AlertDialog.Builder(ctx)
            .setTitle("Режим настройки подписи")
            .setMessage(
                "Скопируйте этот хэш и вставьте его в EXPECTED_CERT_HASH в IntegrityGuard.java, затем пересоберите APK:\n\n" + hash
            )
            .setPositiveButton("Скопировать", (d, w) -> {
                ClipboardManager cm = (ClipboardManager)
                    ctx.getSystemService(Context.CLIPBOARD_SERVICE);
                cm.setPrimaryClip(ClipData.newPlainText("cert_hash", hash));
                Toast.makeText(ctx, "Хэш скопирован!", Toast.LENGTH_SHORT).show();
            })
            .setNegativeButton("Закрыть", null)
            .setCancelable(false)
            .show();
    }

    @SuppressWarnings("deprecation")
    private static String getCertHash(Context ctx) {
        try {
            PackageInfo pi = ctx.getPackageManager().getPackageInfo(
                ctx.getPackageName(),
                PackageManager.GET_SIGNATURES
            );
            Signature sig = pi.signatures[0];
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] digest = md.digest(sig.toByteArray());
            StringBuilder sb = new StringBuilder();
            for (byte b : digest) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return null;
        }
    }

    private static void die() {
        Process.killProcess(Process.myPid());
        System.exit(1);
    }
}
