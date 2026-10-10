# PSP2i on Android

An Android build of the recompiled game. It contains the psprecomp runtime, an SDL3 host (`port/host/sdl_main.c`), the GE renderer on OpenGL ES 3.0 (`port/host/gl_ge.c`) and the ATRAC music decoder.

## What you need

- A phone or tablet with Android 7.0 or later, arm64, and OpenGL ES 3.0.
- Your own copy of the game: the disc's files, with a decrypted `EBOOT.BIN`. The Windows build uses the same files.

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

3. Install `app/build/outputs/apk/release/app-release.apk`. It is signed with the debug key, so it installs directly. Sign it with your own key before publishing it anywhere.

## Setting up the game files

Start the app once. It creates its folder and tells you where it is:

```
Android/data/com.psp2i.recomp/files/
    GameData/disc/     the disc's files (PSP_GAME/...)
    GameData/disc_lba.txt, GameData/flash/   as for the Windows build
    EBOOT.BIN          only if the disc's PSP_GAME/SYSDIR/EBOOT.BIN is encrypted: a decrypted one
    GameData/ms/       the memory stick, holding saves (created when the game first saves)
    psp2i.ini          optional settings
    psp2i_log.txt      the last run's log (include it when reporting a problem)
```

To copy files there, connect the device over USB, or use a file manager that can open `Android/data`.

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
