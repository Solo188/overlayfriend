# ── Обфускация пакета ────────────────────────────────────────────────────────
# Переместить все классы в короткий пакет и разрешить изменение модификаторов
-repackageclasses 'o'
-allowaccessmodification

# ── Точки входа Android (обязательно сохранять имена) ────────────────────────
# Activities, Services и BroadcastReceivers должны быть найдены по имени из манифеста
-keep public class com.endfield.overlayassistant.MainActivity
-keep public class com.endfield.overlayassistant.OverlayService
-keep public class com.endfield.overlayassistant.settings.SettingsActivity

# ── JNI — нативные методы (имена нужны C++-стороне) ─────────────────────────
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.endfield.overlayassistant.NativeRenderer { *; }

# ── Parcelable / Serializable ─────────────────────────────────────────────────
-keepclassmembers class * implements android.os.Parcelable {
    public static final android.os.Parcelable$Creator *;
}
-keepclassmembers class * implements java.io.Serializable {
    static final long serialVersionUID;
    private void writeObject(java.io.ObjectOutputStream);
    private void readObject(java.io.ObjectInputStream);
}

# ── androidx.lifecycle ────────────────────────────────────────────────────────
-keep class androidx.lifecycle.** { *; }
-keepclassmembers class androidx.lifecycle.** { *; }
-dontwarn androidx.lifecycle.**

# ── androidx.startup ──────────────────────────────────────────────────────────
-keep class androidx.startup.** { *; }
-keepclassmembers class * implements androidx.startup.Initializer {
    public <init>();
}

# ── AppCompat / Material ──────────────────────────────────────────────────────
-keep class androidx.core.app.** { *; }
-dontwarn androidx.core.app.**

# ── Убрать логи из релизной сборки ───────────────────────────────────────────
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
}
