#!/bin/sh
# Carmageddon II Android port - build everything from YOUR OWN copy of the game.
#
#   ./build.sh /path/to/setup_carmageddon2_2.1.0.28.exe [apk|desktop|all]     (default: apk)
#
# Steps: extract the installer -> recode the videos -> recompile (lift) the game's x86 code to C
#        -> build the Android APK (dist/carmageddon2.apk) and/or the Linux binary (./carma2).
#
# Requirements: python3 (+venv), innoextract, curl; for the APK: JDK 17, Android SDK with
# NDK 27.2.12479018 and CMake 3.22.1 (ANDROID_HOME or ANDROID_SDK_ROOT); for desktop: clang, make,
# SDL2 and GLES3 development files.
set -e
cd "$(dirname "$0")"

SETUP=${1:?usage: ./build.sh setup_carmageddon2_2.1.0.28.exe [apk|desktop|all]}
TARGET=${2:-apk}
NDK_VERSION=27.2.12479018
SDL_VERSION=2.30.9

# The recompiled code, hooks and patches are made for exactly these files (GOG release 2.1.0.28)
EXE_SHA=9b896c2cbb170c01b3e9f904ce5e1808db29fe5b51184a5d55a6d19b1799b58d
BDD_SHA=8ee8d5c1c359de9695b74f504faa6b522c7e01cdf2d88cea1c1cc389ec4e9301

say() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found ($2)"; }

need python3 "install Python 3"
need innoextract "e.g. apt install innoextract"
need curl "e.g. apt install curl"

say "Python environment"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install -q --upgrade pip >/dev/null
.venv/bin/pip install -q pefile capstone numpy pillow av
PY=".venv/bin/python -I"

say "Extracting the installer"
if [ ! -f game/app/CARMA2_HW.EXE ]; then
    mkdir -p game
    innoextract -s -d game "$SETUP"
fi
[ -f game/app/CARMA2_HW.EXE ] || die "CARMA2_HW.EXE not found in the installer"
sha() { $PY -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }
[ "$(sha game/app/CARMA2_HW.EXE)" = "$EXE_SHA" ] || die "CARMA2_HW.EXE is not the tested GOG version (2.1.0.28)"
[ "$(sha game/app/3dfx_win.bdd)" = "$BDD_SHA" ] || die "3dfx_win.bdd is not the tested GOG version (2.1.0.28)"

say "Recoding the videos (Smacker -> C2V)"
for f in game/app/DATA/CUTSCENE/*.smk game/app/DATA/CUTSCENE/*.SMK; do
    [ -f "$f" ] || continue
    out="${f%.*}.c2v"
    [ -f "$out" ] || $PY tools/recode_video.py "$f" "$out"
done

say "Recompiling the game code (x86 -> C)"
$PY tools/lift.py game/app/CARMA2_HW.EXE gen/exe --name exe --hooks tools/hooks.txt --patches tools/patches.txt
$PY tools/lift.py game/app/3dfx_win.bdd gen/bdd --name bdd --hooks tools/hooks_bdd.txt

if [ "$TARGET" = desktop ] || [ "$TARGET" = all ]; then
    say "Desktop build"
    need make "e.g. apt install build-essential"
    make CC="${CC:-clang}" -j"$(nproc)"
    echo "built ./carma2  (run: ./carma2 game/app save)"
fi

if [ "$TARGET" = apk ] || [ "$TARGET" = all ]; then
    say "Android build"
    SDK=${ANDROID_HOME:-$ANDROID_SDK_ROOT}
    [ -n "$SDK" ] && [ -d "$SDK" ] || die "set ANDROID_HOME to your Android SDK"
    [ -d "$SDK/ndk/$NDK_VERSION" ] || die "Android NDK $NDK_VERSION missing (sdkmanager \"ndk;$NDK_VERSION\")"
    [ -d "$SDK/cmake/3.22.1" ] || die "CMake 3.22.1 missing (sdkmanager \"cmake;3.22.1\")"
    need java "install JDK 17"
    echo "sdk.dir=$SDK" > android/local.properties
    if [ ! -d android/deps/SDL2-$SDL_VERSION ]; then
        mkdir -p android/deps
        curl -sfL -o android/deps/sdl2.tar.gz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
        tar -xzf android/deps/sdl2.tar.gz -C android/deps
        rm android/deps/sdl2.tar.gz
    fi
    (cd android && PYTHON="$(pwd)/../.venv/bin/python" ./prepare_assets.sh ../game/app && ./gradlew assembleRelease)
    mkdir -p dist
    cp android/app/build/outputs/apk/release/app-release.apk dist/carmageddon2.apk
    echo "built dist/carmageddon2.apk  (install: adb install -r dist/carmageddon2.apk)"
fi
