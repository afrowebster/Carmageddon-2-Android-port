#!/bin/sh
# Copy everything the game needs from your own installed copy (innoextract output) into the APK assets.
set -e
SRC=${1:-../game/app}
DST=app/src/main/assets/game
rm -rf "$DST"; mkdir -p "$DST"
cp "$SRC/CARMA2_HW.EXE" "$SRC/3dfx_win.bdd" "$SRC/DATA.TWT" "$DST/"
cp -r "$SRC/DATA" "$SRC/music" "$DST/"
rm -rf "$DST/DATA/SAVEGAME"                 # saves live in the app's save dir
rm -f "$DST"/DATA/CUTSCENE/*.smk            # videos ship recoded (.c2v, see tools/recode_video.py)
ls "$DST"/DATA/CUTSCENE/*.c2v >/dev/null    # fail if the recoded videos are missing
# launcher icon = the PC game's icon
PY=${PYTHON:-$(cd .. && pwd)/.venv/bin/python}
"$PY" -I ../tools/make_icon.py "$SRC/goggame-1207659963.ico" app/src/main/res "$SRC/icon.bmp"
(cd "$DST" && find . -type f ! -name manifest.txt | sed 's|^\./||' | sort > manifest.txt)
# the stamp changes whenever the data set changes, so the app re-extracts only then
(cd "$DST" && cat manifest.txt | xargs -d '\n' stat -c '%n %s' | md5sum | cut -c1-16 > stamp.txt)
echo "assets ready: $(wc -l < "$DST/manifest.txt") files, $(du -sh "$DST" | cut -f1)"
