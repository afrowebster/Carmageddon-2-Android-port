Carmageddon 2 Android port - APK builder
=========================================

Builds an Android APK of Carmageddon II from YOUR OWN copy of the game. Tested with the GOG release
(setup_carmageddon2_2.1.0.28.exe); other versions are refused. The APK contains your game files:
keep it for yourself, do not distribute it.

Nothing else needs to be installed.
  * "full" download: includes the Zig C compiler, works offline.
  * "light" download: downloads the Zig C compiler (about 50-90 MB, checksum verified) the first time.

Use
---
Start carma2-apk-builder (double-click, or run it from a terminal without arguments), choose the GOG
installer (or the folder where the game is installed), press "Build APK".

Command line:
    carma2-apk-builder setup_carmageddon2_2.1.0.28.exe -o carmageddon2.apk
    options: --abi arm64-v8a,x86_64   (x86_64 = PC emulators)   --jobs N   (parallel compile jobs)

The first build takes 10-40 minutes depending on the computer (the game's code is recompiled to C and
compiled for Android). Later builds reuse that work.

Install: copy the APK to the phone and open it (allow installing apps from that source), or
"adb install -r carmageddon2.apk".

Files the builder keeps:
  work files and the downloaded compiler: the user cache folder, "carma2-apk-builder"
      (Windows %LOCALAPPDATA%, macOS ~/Library/Caches, Linux ~/.cache) - can be deleted after the build
  your signing key: the user config folder, "carma2-apk-builder/signing-key.txt"
      (Windows %APPDATA%, macOS ~/Library/Application Support, Linux ~/.config) - keep it: APKs signed with
      the same key install as updates and keep your saves.

macOS: the program is not notarised. If macOS refuses to open it, run once in a terminal:
    xattr -dr com.apple.quarantine /path/to/carma2-apk-builder

Source code, license (GPL-3.0) and documentation:
    https://github.com/afrowebster/Carmageddon-2-Android-port
