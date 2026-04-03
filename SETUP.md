# Endfield Overlay Assistant — Build & Setup Guide

## Prerequisites

| Tool | Version |
|------|---------|
| Android Studio | Hedgehog 2023.1+ |
| JDK | 17 (Temurin / Azul) |
| NDK | **26.1.10909125** (install via SDK Manager) |
| CMake | **3.22.1** (install via SDK Manager) |
| Gradle wrapper | 8.6 (included) |

---

## 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/YOUR_ACCOUNT/endfield-overlay.git
cd endfield-overlay
```

If you cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive app/src/main/cpp/saba
```

The Saba MMD loader lives at:
`app/src/main/cpp/saba/` → add the official repo:
```bash
git submodule add https://github.com/benikabocha/saba.git app/src/main/cpp/saba
```

---

## 2. Device / emulator

- **Minimum**: Android 12 (API 31)
- **Recommended**: Poco X3 Pro (Adreno 640 — OpenGL ES 3.0 confirmed)
- **Emulator**: Any AVD with API 31+ and `x86_64` + GPU acceleration

> **Note**: `TYPE_APPLICATION_OVERLAY` overlays do **not** render inside the emulator's "lock screen".  
> Test on a real device for the full overlay experience.

---

## 3. Permissions (runtime — first launch)

The app will prompt for:

1. **Draw Over Other Apps** — required. Grant in Settings → Apps → Special App Access.
2. **Post Notifications** — required for the foreground service on Android 13+.
3. **Read External Storage / Read Media** — required to load PMX/VMD from `/sdcard/Documents/`.

---

## 4. Model folder structure

Place your assets here (create folders as needed):

```
/sdcard/Documents/Assistant/Models/
└── CharacterName/
    ├── CharacterName.pmx          ← main model file
    └── motions/
        ├── idle/
        │   ├── breathing.vmd
        │   └── idle_sway.vmd
        ├── touch/
        │   └── headpat_react.vmd
        ├── night/
        │   └── sleepy_wave.vmd
        └── friend/
            └── friend_smile.vmd
```

The app scans `/sdcard/Documents/Assistant/Models/` on start.  
Any folder with a `.pmx` file is offered as a selectable character.

---

## 5. GitHub Actions CI

Push to `main` or `master` to trigger the build:

```
.github/workflows/build.yml
```

The workflow:
1. Checks out with all submodules.
2. Installs JDK 17 + NDK 26.1.10909125 + CMake 3.22.1.
3. Builds `app-debug.apk`.
4. Uploads the APK as a downloadable Actions artifact (14-day retention).

For a **release build** you need to add:
```
KEYSTORE_B64   # base64-encoded .jks file
KEY_ALIAS
KEY_STORE_PASS
KEY_PASS
```
as GitHub repository secrets and add a signing step to the workflow.

---

## 6. Architecture overview

```
Java layer                     C++ layer (NDK)
─────────────────────────────  ─────────────────────────────
MainActivity                   native-lib.cpp (JNI bridge)
 └─ starts OverlayService       └─ MMDRenderer (OpenGL ES 3.0)
     └─ adds OverlayView             └─ ShaderProgram (toon + outline)
         └─ GLSurfaceView            └─ VMDManager
             └─ NativeRenderer            ├─ motion pool (random pick)
                 (JNI)                   ├─ LERP blend (0.5s)
                                         └─ blink / mouth morphs
AffinityManager (SharedPrefs)
AiAssistantImpl → IAiAssistant  (swap for LLM API here)
```

---

## 7. Upgrading the AI stub

`AiAssistantImpl.processInput()` currently returns random phrases from `phrases.xml`.

To connect a real LLM:

```java
// In AiAssistantImpl.java → processInput():
// Replace the pickRandom() call with:
Response<ResponseBody> resp = yourApiService.chat(
    new ChatRequest(userInput, affinityTier)
).execute();
return resp.body().string();
```

The `IAiAssistant` interface remains unchanged — zero impact on callers.

---

## 8. Affinity system

| Score | Tier | Unlocks |
|-------|------|---------|
| 0–100 | Stranger | `idle/` motions, generic phrases |
| 101–500 | Friend | `friend/` motions, extra morphs |
| 501+ | Partner | `night/` motions, heart particles, Partner phrases |

Points are awarded:
- **+1** per headpat (tap without drag)
- **+10** per 30 minutes of active overlay runtime

Stored in `SharedPreferences` → `affinity_data.xml`.
