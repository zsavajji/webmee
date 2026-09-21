#!/bin/sh
# Fetch the benchmark corpus on the HOST and push it to the device.
#
#   bench-corpus.sh [--push]
#
# The corpus is saved complete with assets and served from file:// so the
# network is out of the measurement: a run over 3G measures the link, not the
# browser, and this device's link varies far more than the ~16 % the numbers
# already move on their own. The content blocker (plan 2.1) is the one change
# that must be measured with the network ON instead, as its own run set.
#
# Fetched with the browser's OWN default user agent, so each site serves the
# layout the device would actually get. A desktop-layout Wikipedia page is a
# different benchmark from the mobile one.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR="${DIR:-$ROOT/out/bench}"
HOST="${HOST:-user@n9}"
DEST=/home/user/bench
UA="Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1"

# name|url|what it is here to measure
PAGES="
minimal|http://example.com/|floor: near-empty document
wikipedia|https://en.m.wikipedia.org/wiki/Nokia_N9|text and font rasterisation
github|https://github.com/WebKit/WebKit|deep DOM, nested rounded clips
news|https://www.bbc.com/news|images and a front-page layout
spa|https://todomvc.com/examples/react/dist/|renders nothing without scripts
"

fetch() {
    name=$1; url=$2
    rm -rf "$DIR/$name"; mkdir -p "$DIR/$name"
    # -p page requisites, -H across hosts (assets live on CDNs), -k rewrite
    # links to the local copies, -E give documents an .html extension.
    # A non-zero exit is normal here -- one 404 asset out of eighty sets it --
    # so the entry document, not the exit code, is what says the fetch worked.
    # Retry once: the first attempt at Wikipedia came back 429 and saved
    # nothing at all, and the identical command succeeded seconds later.
    try=1
    while [ $try -le 2 ]; do
        wget -q -p -k -E -H -e robots=off -U "$UA" -P "$DIR/$name" "$url" || true
        [ -n "$(find "$DIR/$name" -name '*.html' -print -quit 2>/dev/null)" ] && break
        echo "  attempt $try saved no document, retrying" >&2
        sleep 5; try=$((try + 1))
    done

    # Where wget put the main document: <host>/<path>, with .html appended by
    # -E and index.html for a bare directory.
    rest=${url#*://}; h=${rest%%/*}; p=${rest#"$h"}
    case "$p" in ""|*/) entry="$DIR/$name/$h${p}index.html" ;;
                 *)     entry="$DIR/$name/$h$p" ;; esac
    [ -f "$entry" ] || entry="$DIR/$name/$h$p.html"
    # Redirects move it; fall back to the shallowest document wget saved.
    [ -f "$entry" ] || entry=$(find "$DIR/$name" -name '*.html' | awk '{print gsub(/\//,"/"), $0}' | sort -n | head -1 | cut -d' ' -f2-)
    [ -n "$entry" ] && [ -f "$entry" ] || { echo "$name: NO ENTRY DOCUMENT" >&2; return 1; }
    echo "${entry#$DIR/}"
}

mkdir -p "$DIR"
: > "$DIR/entries.txt"
echo "# Benchmark corpus" > "$DIR/README.md"
{
    echo
    echo "Fetched $(date +%Y-%m-%d) with \`probe/bench-corpus.sh\`, using the"
    echo "browser's own default user agent. Re-fetch rather than edit: a number"
    echo "is only comparable to another taken against the same bytes, so note the"
    echo "date whenever the corpus is refreshed."
    echo
    echo "| name | measures | original URL |"
    echo "|---|---|---|"
} >> "$DIR/README.md"

echo "$PAGES" | while IFS='|' read -r name url what; do
    [ -n "${name:-}" ] || continue
    echo "fetching $name ..." >&2
    if entry=$(fetch "$name" "$url"); then
        echo "$name|$DEST/$entry" >> "$DIR/entries.txt"
        echo "| \`$name\` | $what | <$url> |" >> "$DIR/README.md"
        echo "  -> $entry ($(du -sh "$DIR/$name" | cut -f1))" >&2
    fi
    sleep 3
done

echo >&2; echo "corpus in $DIR:" >&2; cat "$DIR/entries.txt" >&2

if [ "${1:-}" = "--push" ]; then
    echo >&2; echo "pushing to $HOST:$DEST ..." >&2
    ssh "$HOST" "rm -rf $DEST; mkdir -p $DEST"
    scp -qr "$DIR/." "$HOST:$DEST/"
    ssh "$HOST" "du -sh $DEST"
fi
