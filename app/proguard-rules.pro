# Keep our own JNI-facing classes
-keep class com.endfield.overlayassistant.** { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep all androidx.lifecycle classes intact — R8 must not remove inner classes
# like ProcessLifecycleOwner$initializationListener or ReportFragment$ActivityInitializationListener
-keep class androidx.lifecycle.** { *; }
-keepclassmembers class androidx.lifecycle.** { *; }
-dontwarn androidx.lifecycle.**

# Keep androidx.startup initializers
-keep class androidx.startup.** { *; }
-keepclassmembers class * implements androidx.startup.Initializer {
    public <init>();
}

# Keep all AppCompat/Material internals that lifecycle depends on
-keep class androidx.core.app.** { *; }
-dontwarn androidx.core.app.**
