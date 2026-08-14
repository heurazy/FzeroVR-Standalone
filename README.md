# F-Zero X VR — Quest 3 standalone

Native Android/OpenXR Quest port built from Zorkats/G-Diffuser and the pinned F-Zero X decomp.
No Nintendo ROM or extracted game data is included in this project or APK.

## Current Quest runtime

- arm64-v8a NativeActivity; no PC runtime required after installation.
- OpenXR `PRIMARY_STEREO` with two independent eye swapchains.
- 6DoF head tracking from the predicted OpenXR eye poses.
- Exact asymmetric per-eye OpenXR frustum is translated into F-Zero X's existing projection-view matrix builder.
- The same Fast3D display list is rendered twice per game graphics task with separate left/right camera matrices; gameplay advances only once at the original ~60 Hz rate.
- Quest Touch input is exposed directly through G-Diffuser's `gdx_lus_read_pads` host ABI, with the Quest 3/3S Meta Touch Plus OpenXR 1.1 profile preferred and older Touch profiles retained as fallbacks.
- Both controller grip poses are tracked in 6DoF; the game's N64 Rumble Pak state is bridged to sustained OpenXR haptics on both Touch controllers.
- G-Diffuser/libultraship/Fast3D, F-Zero X US rev0 decomp, cooperative fiber scheduler, HLE/LLE audio, SRAM and ghost code all build for Android arm64.
- When no `.o2r` archive is installed, generated common compressed assets fall back to exact offsets in the user's validated US ROM.

## Quest Touch mapping

- Left stick: N64 analog stick / steering
- Right trigger or A: N64 A / accelerate
- Left trigger: N64 C-Down / brake
- B: N64 B / boost
- Left grip: N64 Z / left side attack-drift
- Right grip: N64 R / right side attack-drift
- X: N64 L
- Y: C-Up
- Right stick: C buttons
- Menu or left-stick click: Start

## Build

From PowerShell in the project root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\scripts\build-quest.ps1 -Configuration Debug
```

Generated APK:

```text
app\build\outputs\apk\debug\app-debug.apk
```

The build script uses the installed Android SDK/NDK and JDK 17, and bootstraps Gradle locally under `.tools` when required.

## Install with your own ROM

Connect the Quest in developer/ADB mode, then run:

```powershell
.\scripts\install-quest.ps1 -Rom "D:\path\to\your\F-Zero X (USA).z64"
```

The debug installer:

1. installs the APK;
2. copies your ROM into the app-private `files/baserom.us.rev0.z64` path using `run-as`;
3. launches the NativeActivity.

The ROM is never copied into the project or APK.

## Useful logs

```powershell
adb logcat -s FZeroXVR FZeroXVR/OpenXR FZeroXVR/GameHost FZeroXVR/GDiffuser
```

If no compatible US ROM is present, the app keeps the native stereo OpenXR verification scene instead of silently booting blank game data.

## Source revisions pinned by the build

- G-Diffuser: `719fd82a3af605b064fb53ad6eecb020090b4c5d`
- libultraship: `a4919b181e637193f2b8ae975e31505abbf99e71`
- F-Zero X decomp: `f7fd0fd0242f8dfb5f357f604bb73b6a4e990809`
- Torch: `c1bdc6fde97fbaa4495c9e859f635290840a12d3`
- libucontext: `49e671dd52ff6791295d8161ad3b6da7dc5f6f9d`

## Validation status

The complete Android arm64 project and APK compile/link successfully. A headset runtime smoke test still requires a Quest 3 visible through ADB; no Quest was connected to this workstation during the build session. The first on-device pass should therefore focus on camera scale/orientation, framebuffer orientation, audio output, and controller mapping feel before treating the port as release-ready.
