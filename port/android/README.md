# PSP2i on Android

An Android build of the recompiled game. It contains the psprecomp runtime, an SDL3 host (`port/host/sdl_main.c`), the GE renderer on OpenGL ES 3.0 (`port/host/gl_ge.c`) and the ATRAC music decoder.

## What you need

- A phone or tablet with Android 7.0 or later, arm64, and OpenGL ES 3.0.
- Your own copy of the game: its disc image (.iso, with a decrypted EBOOT), and the PSP fonts (GameData/flash, as for the Windows build).

## Building a playable APK

The game's code is C that `allegrexrecomp` generates from your decrypted `EBOOT.BIN` into `port/gen`. That C is derived from the game, so the repository does not include it. CI builds the APK without it: that APK installs and starts, then tells you the game code is missing.

1. Generate `port/gen` once (the same step as for Windows):

   ```
   psprecomp/build/tools/allegrexrecomp/Release/allegrexrecomp.exe emit EBOOT.BIN port/gen recomp --hooks port/hooks.txt --fixes port/fixes.txt
   ```

2. Build. You need JDK 17 and the Android SDK; Android Studio provides both. Open `port/android` in Android Studio, or from a shell:

   ```
   cd port/android
   ./gradlew assembleRelease        (gradlew.bat on Windows)
   ```

   - The Gradle wrapper downloads Gradle 8.9 itself (checksum-verified).
   - The first build downloads the newest SDL3 release's source into `port/android/deps/SDL`.
   - To use another SDL checkout, pass `-PsdlDir=<path>`.
   - To also build for emulators, pass `-Pabis=arm64-v8a,x86_64`.
   - The generated C is large, so expect the first compile to take a while.

   **On Windows without Android Studio:** `port/android/build_local.bat` builds it with a minimal toolchain kept in one folder (`PSP2I_ANDROID_TOOLS`, default `F:\psp2i-android-tools`; about 3 GB for the tools plus 1-2 GB of Gradle cache). That folder holds:
   - `jdk17\`: a JDK 17 (Temurin zip)
   - `sdk\`: the command-line tools plus `platforms;android-35`, `build-tools;34.0.0`, `ndk;27.0.12077973` and `cmake;3.22.1`, installed with `sdkmanager --sdk_root=<folder>\sdk`

   The script points Gradle's cache, the SDK's settings and temporary files into that folder too, so nothing goes to C:, and deleting the folder removes everything. It copies the APK to `FinalBuild\psp2i-android.apk`.

3. Install `app/build/outputs/apk/release/app-release.apk`. It is signed with the debug key, so it installs directly. Sign it with your own key before publishing it anywhere.

## Setting up the game files

Copy two things anywhere onto the device (Downloads is fine):

- **the game's ISO**: an `.iso` of the disc whose `PSP_GAME/SYSDIR/EBOOT.BIN` is decrypted
- **your GameData folder**: the PSP fonts in its `flash/` are copied from it once (the Windows build uses the same folder)

On first start the app asks for each with Android's own file picker: first the ISO, then the GameData folder (or its `flash` folder). It remembers the ISO and keeps a copy of the fonts, so later starts go straight into the game. No storage permission is needed. If the ISO is moved or deleted, the app asks for it again.

The app's own folder, `Android/data/com.psp2i.recomp/files/`, holds:

```
GameData/flash/    the copied fonts
GameData/ms/       the memory stick, holding saves (created when the game first saves)
psp2i.ini          optional settings
psp2i_log.txt      the last run's log (include it when reporting a problem)
```

Saves live there, so uninstalling the app deletes them; copy `GameData/ms` off the device (USB or adb) to keep them.

The extracted layout still works: a `GameData/disc/` folder in the app's folder is used instead of an ISO, and an `EBOOT.BIN` there is used if your disc's EBOOT is encrypted.

### psp2i.ini

```
fps=60        30 (the game's own rate, the default) or 60
touch=off     hide the touch controls for good (a controller is used)
```

## Controls

- **Touch:**
  - left side: the analog stick, with the D-pad above it
  - right side: the face buttons, coloured like the PSP's (triangle green, circle red, cross blue, square pink)
  - top corners: L and R
  - bottom centre: SELECT and START
- **Controllers:** any controller Android recognises.
  - The face buttons follow their positions: bottom is cross, right is circle.
  - The triggers also work as L and R.
  - The right stick turns the camera.
  - The touch controls hide while a controller is in use and come back when you touch the screen.

## Not here yet

- **Online play:** RPCN and the SEGA server are on the Windows build only.
- **The settings menu:** use `psp2i.ini` for now.
- **Movie video:** movies play their sound over a black picture.
- **Save encryption:** saves are written unencrypted. They work on the device, but they don't move to or from a PSP, PPSSPP or the Windows build.
