#!/bin/sh
# Drive probe/bench.sh over ssh, N runs per cell, and print the medians as a
# markdown table row ready for docs/performance.md.
#
#   bench-host.sh <label> <url> [runs]          both JS modes
#   JS=0 bench-host.sh <label> <url> [runs]     one mode only
#
# Medians, not means, and never a single run: run-to-run variance on this
# device is about 16 % on CPU for an identical configuration, so one A/B cannot
# resolve anything smaller than that.
set -eu

HOST="${HOST:-user@n9}"
LABEL="${1:?usage: bench-host.sh <label> <url> [runs]}"
URL="${2:?usage: bench-host.sh <label> <url> [runs]}"
RUNS="${3:-5}"
RAW="${RAW:-/tmp/bench-$LABEL.raw}"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
scp -q "$ROOT/probe/bench.sh" "$HOST:/home/user/bench.sh"
ssh "$HOST" 'chmod +x /home/user/bench.sh'

# N9_ENGINE is forwarded so a freshly scp'd engine can be measured against the
# installed tree without root. Note there is no --single-instance anywhere
# here: it hands off to an already-running instance and silently measures the
# installed build instead.
one_run() {
    ssh "$HOST" "N9_ENGINE='${N9_ENGINE:-}' DISPLAY=:0 sh /home/user/bench.sh '$URL' $1 '$LABEL'" \
        2>>/tmp/bench-$LABEL.err
}

: > "$RAW"; : > /tmp/bench-$LABEL.err

# One discarded warm-up. The corpus is read off eMMC, and the first pass over a
# freshly copied 5 MB page pays for the kernel page cache that every later run
# gets free: measured cold-first, JavaScript OFF looked SLOWER to load than ON,
# which is backwards.
echo "== $LABEL warm-up (discarded)" >&2
one_run "${JS:-0}" > /dev/null || true

# Interleave the modes rather than running all of one then all of the other, so
# anything that drifts over the session -- page cache, device temperature, a
# background daemon waking up -- lands on both arms instead of just the first.
n=1
while [ "$n" -le "$RUNS" ]; do
    for js in ${JS:-0 1}; do
        echo "== $LABEL js=$js run $n/$RUNS" >&2
        one_run "$js" >> "$RAW" || echo "run failed, see /tmp/bench-$LABEL.err" >&2
    done
    n=$((n + 1))
done

echo >&2
awk -v label="$LABEL" '
function med(a, n,   i, t) {           # insertion sort, n is small
    for (i = 2; i <= n; i++) { t = a[i]; for (j = i - 1; j >= 1 && a[j] > t; j--) a[j+1] = a[j]; a[j+1] = t }
    return (n % 2) ? a[int(n/2) + 1] : int((a[n/2] + a[n/2 + 1]) / 2)
}
{
    delete f
    for (i = 1; i <= NF; i++) { split($i, kv, "="); f[kv[1]] = kv[2] }
    js = f["js"]; c = ++n[js]
    split(f["web_hwm_rss"], m, ",")
    load[js, c] = f["load_ms"]; fr[js, c] = f["frames"]
    wcpu[js, c] = f["web_cpu"]; scr[js, c] = f["web_cpu_scroll"]; hwm[js, c] = m[1]; rss[js, c] = m[2]
    bad[js] += (f["failed"] > 0)
}
END {
    printf "| page | JS | load ms | frames | web cpu scroll | web cpu total | web VmHWM kB | web VmRSS kB | runs |\n"
    printf "|---|---|---|---|---|---|---|---|---|\n"
    for (js = 0; js <= 1; js++) {
        if (!n[js]) continue
        for (c = 1; c <= n[js]; c++) { L[c] = load[js, c]; F[c] = fr[js, c]; C[c] = wcpu[js, c]; S[c] = scr[js, c]; H[c] = hwm[js, c]; R[c] = rss[js, c] }
        printf "| %s | %s | %d | %d | %d | %d | %d | %d | %d%s |\n", label, (js ? "on" : "off"),
               med(L, n[js]), med(F, n[js]), med(S, n[js]), med(C, n[js]), med(H, n[js]), med(R, n[js]),
               n[js], (bad[js] ? " **" bad[js] " with `failed`**" : "")
    }
}' "$RAW"

echo >&2
echo "raw: $RAW" >&2
# A frame count of zero is a broken harness, not a slow page: stdout buffering
# and unpaced touches have each produced it. Say so rather than tabulating it.
grep -q 'frames=0 ' "$RAW" && echo "WARNING: some runs counted 0 frames -- check pacing and that stdout reached the file" >&2
exit 0
