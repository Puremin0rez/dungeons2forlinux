#!/bin/sh
# Copies xgameruntime.dll (from this folder, or dist/ in a source checkout) next to the game's executables.
# --uninstall removes it and the sign-in state.
set -eu

APPID=1912410
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
DLL="$ROOT/xgameruntime.dll"
[ -f "$DLL" ] || DLL="$ROOT/dist/xgameruntime.dll"

die() { echo "install.sh: $*" >&2; exit 1; }

# Prints each Steam library folder, one per line.
steam_libraries() {
    for root in ${STEAM_ROOT:+"$STEAM_ROOT"} "$HOME/.local/share/Steam" "$HOME/.steam/steam" \
                "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"; do
        vdf="$root/steamapps/libraryfolders.vdf"
        [ -f "$vdf" ] || continue
        sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g'
    done
}

LIB=$(steam_libraries | while IFS= read -r lib; do
    if [ -f "$lib/steamapps/appmanifest_$APPID.acf" ]; then printf '%s\n' "$lib"; break; fi
done)
[ -n "$LIB" ] || die "Minecraft Dungeons II (app $APPID) is not in any Steam library. Set STEAM_ROOT if Steam lives elsewhere."
GAME="$LIB/steamapps/common/Minecraft Dungeons II"
PREFIX="$LIB/steamapps/compatdata/$APPID/pfx"
TARGETS="$GAME
$GAME/Dungeons/Binaries/Win64"

if pgrep -f 'Dungeons-Win64-Shipping\.exe|Minecraft Dungeons II.Dungeons\.exe' >/dev/null 2>&1; then
    die "the game is running; quit it first (a running game keeps the old DLL loaded)"
fi

if [ "${1:-}" = "--uninstall" ]; then
    printf '%s\n' "$TARGETS" | while IFS= read -r dir; do
        if [ -f "$dir/xgameruntime.dll" ]; then
            rm -f -- "$dir/xgameruntime.dll"
            echo "removed $dir/xgameruntime.dll"
        fi
    done
    STATE="$PREFIX/drive_c/users/steamuser/AppData/Local/dungeons2forlinux"
    if [ -d "$STATE" ]; then
        rm -rf -- "$STATE"
        echo "removed $STATE (sign-in and log)"
    fi
    exit 0
fi

[ -f "$DLL" ] || die "xgameruntime.dll is missing; download a release, or run 'make' in a source checkout"

want=$(sha256sum "$DLL" | cut -d' ' -f1)
printf '%s\n' "$TARGETS" | while IFS= read -r dir; do
    [ -d "$dir" ] || die "missing $dir"
    install -m 0644 -- "$DLL" "$dir/xgameruntime.dll"
    [ "$(sha256sum "$dir/xgameruntime.dll" | cut -d' ' -f1)" = "$want" ] || die "copy to $dir did not verify"
    echo "installed $dir/xgameruntime.dll"
done
echo "Done. Start the game from Steam; no launch options are needed."
