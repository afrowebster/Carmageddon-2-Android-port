#!/bin/sh
# Carmageddon II Android port - build from YOUR OWN copy of the game, from source.
# (Most people want the ready-made APK builder from the GitHub releases instead: no setup needed.)
#
#   ./build.sh /path/to/setup_carmageddon2_2.1.0.28.exe [apk|desktop|all]     (default: apk)
#
# apk:     runs the APK builder (builder/c2build.py) from source -> dist/carmageddon2.apk
#          Needs python3 (+venv). The runtime template APK is built with Gradle when ANDROID_HOME is set
#          (Android SDK with NDK 27.2.12479018, CMake 3.22.1, JDK 17), otherwise downloaded from the latest
#          release. The Zig C compiler is downloaded on first use.
# desktop: Linux binary ./carma2 (needs innoextract, clang, make, SDL2 and GLES3 development files).
set -e
cd "$(dirname "$0")"

SETUP=${1:?usage: ./build.sh setup_carmageddon2_2.1.0.28.exe [apk|desktop|all]}
TARGET=${2:-apk}
SDL_VERSION=2.30.9
REPO=afrowebster/Carmageddon-2-Android-port

say() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found ($2)"; }

need python3 "install Python 3"
say "Python environment"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install -q --upgrade pip >/dev/null
.venv/bin/pip install -q pefile==2024.8.26 capstone==5.0.9 numpy pillow av
PY=".venv/bin/python -I"

if [ "$TARGET" = apk ] || [ "$TARGET" = all ]; then
    mkdir -p dist
    SDK=${ANDROID_HOME:-$ANDROID_SDK_ROOT}
    if [ -n "$SDK" ] && [ -d "$SDK" ]; then
        say "Runtime template APK (Gradle)"
        need java "install JDK 17"
        echo "sdk.dir=$SDK" > android/local.properties
        if [ ! -d android/deps/SDL2-$SDL_VERSION ]; then
            mkdir -p android/deps
            curl -sfL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" | tar -xz -C android/deps
        fi
        (cd android && ./gradlew -q assembleRelease)
        cp android/app/build/outputs/apk/release/app-release-unsigned.apk dist/template.apk
    elif [ ! -f dist/template.apk ]; then
        say "Runtime template APK (download)"
        need curl "e.g. apt install curl"
        curl -sfL -o dist/template.apk "https://github.com/$REPO/releases/latest/download/carma2-runtime-template.apk" \
            || die "could not download the template APK; set ANDROID_HOME to build it"
    fi
    say "APK builder"
    $PY builder/c2build.py "$SETUP" -o dist/carmageddon2.apk --template dist/template.apk
fi

if [ "$TARGET" = desktop ] || [ "$TARGET" = all ]; then
    say "Desktop build"
    need innoextract "e.g. apt install innoextract"
    need make "e.g. apt install build-essential"
    if [ ! -f game/app/CARMA2_HW.EXE ]; then
        mkdir -p game
        innoextract -s -d game "$SETUP"
    fi
    sha() { $PY -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }
    [ "$(sha game/app/CARMA2_HW.EXE)" = 9b896c2cbb170c01b3e9f904ce5e1808db29fe5b51184a5d55a6d19b1799b58d ] \
        || die "CARMA2_HW.EXE is not the tested GOG version (2.1.0.28)"
    for f in game/app/DATA/CUTSCENE/*.smk game/app/DATA/CUTSCENE/*.SMK; do
        [ -f "$f" ] || continue
        [ -f "${f%.*}.c2v" ] || $PY tools/recode_video.py "$f" "${f%.*}.c2v"
    done
    $PY tools/lift.py game/app/CARMA2_HW.EXE gen/exe --name exe --hooks tools/hooks.txt --patches tools/patches.txt
    $PY tools/lift.py game/app/3dfx_win.bdd gen/bdd --name bdd --hooks tools/hooks_bdd.txt
    make CC="${CC:-clang}" -j"$(nproc)"
    echo "built ./carma2  (run: ./carma2 game/app save)"
fi
