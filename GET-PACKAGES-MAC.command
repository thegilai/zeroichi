#!/bin/sh
# Zero Ichi package downloader for macOS (also works on Linux).
# Downloads everything listed in offline/download-list.txt onto this SD card.

cd "$(dirname "$0")" || exit 1
OFF="$(pwd)/offline"
LIST="$OFF/download-list.txt"
TAB="$(printf '\t')"

echo ""
echo "  Zero Ichi package downloader"
echo "  -------------------------"
if [ ! -f "$LIST" ]; then
    echo "  Nothing to download - the Pi has not asked for anything."
    echo "  Put the card back in the Pi and power it on."
    exit 0
fi

# clean copy of the list (Windows line endings, comments and blank lines removed)
WORK="$(mktemp -t zeroichi.XXXXXX)" || exit 1
tr -d '\r' < "$LIST" | grep -v '^#' | grep "$TAB" > "$WORK"
total=$(wc -l < "$WORK" | tr -d ' ')

ok=0; skipped=0; failed=0; n=0
while IFS="$TAB" read -r kind url rel; do
    n=$((n + 1))
    target="$OFF/$rel"
    name="$(basename "$target")"
    mkdir -p "$(dirname "$target")"
    case "$rel" in
        *InRelease|*Packages.xz|*Packages.gz) ;;              # always refresh lists
        *) if [ -s "$target" ]; then skipped=$((skipped + 1)); continue; fi ;;
    esac
    printf '  [%s/%s] %s' "$n" "$total" "$name"
    if curl -fsL --retry 3 --connect-timeout 20 -o "$target.part" "$url"; then
        mv -f "$target.part" "$target"
        ok=$((ok + 1))
        echo "  OK"
    else
        rm -f "$target.part"
        if [ "$kind" = "OPT" ]; then
            echo "  (not on server - that is fine)"
        else
            failed=$((failed + 1))
            echo "  FAILED"
        fi
    fi
done < "$WORK"
rm -f "$WORK"

# remove the hidden "._" files macOS creates on the SD card
dot_clean -m "$OFF" 2>/dev/null
sync

echo ""
if [ "$failed" -gt 0 ]; then
    echo "  $failed required download(s) failed. Check your internet connection and run this again."
else
    echo "  Done! ($ok downloaded, $skipped already there)"
    echo "  Now eject the SD card (drag 'bootfs' to the Trash / Eject), put it back"
    echo "  in the Pi and power it on."
fi
echo ""
