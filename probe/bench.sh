#!/bin/sh
# One benchmark run, ON THE DEVICE. Drives the engine standalone over stdin and
# prints one line of `key=value` pairs on stdout. Everything else goes to stderr.
#
#   bench.sh <url> <js 0|1> [label]
#
# Why standalone rather than through the app: the shell adds a second process
# and a repaint per frame to every number, and cannot be told to load one page
# and stop. The app is measured separately, once (see the plan's Phase 0.5).
#
# The environment below is the shell's own (BrowserEngine::startEngine). It is
# not optional: an engine started without SSL_CERT_FILE fails every https page
# with `failed TLS unknown-CA` and then behaves exactly like a page that loaded
# and will not scroll -- which has already produced one confident diagnosis of
# a bug that did not exist. This script checks for `failed` before reporting.
set -u

PREFIX="${PREFIX:-/opt/n9browser}"
ENGINE="${N9_ENGINE:-$PREFIX/bin/webmee-engine}"
URL="${1:?usage: bench.sh <url> <js 0|1> [label]}"
JS="${2:?usage: bench.sh <url> <js 0|1> [label]}"
LABEL="${3:-run}"

W=480; H=674                  # page area in device pixels, as the app runs it
OUT=/tmp/bench.out; ERR=/tmp/bench.err; FIFO=/tmp/bench.fifo
rm -f "$OUT" "$ERR" "$FIFO"

# busybox `sleep` only takes fractions when it was built with them. Find out
# once rather than silently polling at 1 s and calling a 300 ms load "0".
if sleep 0.2 2>/dev/null; then TICK=0.2; PER=5; else TICK=1; PER=1; fi

now() { awk '{print int($1 * 100)}' /proc/uptime; }        # centiseconds
memfree() { awk '/^MemFree/ {print $2}' /proc/meminfo; }
swapfree() { awk '/^SwapFree/ {print $2}' /proc/meminfo; }
webprocs() { ps | grep -c '[W]PEWebProcess'; }
# Resolve a pid ONCE and hand it back; a pattern that matches nothing silently
# reports cpu=0 for the whole run.
pidof_x() { ps | grep "$1" | awk '{print $1; exit}'; }
cpu() { [ -r "/proc/$1/stat" ] && awk '{print $14 + $15}' "/proc/$1/stat" || echo -1; }
mem() { [ -r "/proc/$1/status" ] &&
        awk '/^VmHWM/ {h = $2} /^VmRSS/ {r = $2} END {print h "," r}' "/proc/$1/status" ||
        echo "-1,-1"; }
frames() { grep -c '^frame' "$OUT"; }
# A locked or blanked screen suppresses frame production, so a run taken on one
# reports a low frame count that looks like a slow page. Ask MCE rather than
# remembering: it is the only thing that knows, and a screenshot cannot tell a
# black page from a black screen.
mce() { dbus-send --system --print-reply --dest=com.nokia.mce \
            /com/nokia/mce/request "com.nokia.mce.request.$1" 2>/dev/null |
        awk -F'"' '/string/ {print $2; exit}'; }
display_state() { echo "$(mce get_display_status):$(mce get_tklock_mode)"; }

mce_req() {
    m="$1"; shift
    dbus-send --system --dest=com.nokia.mce /com/nokia/mce/request \
              "com.nokia.mce.request.$m" "$@" >/dev/null 2>&1
}

# Hold the screen on for the whole run. MCE's blanking pause lapses after about
# a minute, so it is renewed rather than set once -- a corpus page that takes
# 40 s to load would otherwise blank halfway and halve its own frame count.
# Measured: the same page scored 11 frames locked and 55 unlocked.
#
# This turns the display ON but CANNOT clear the touch lock. `user` (uid 29999)
# is allowed req_display_state_on and req_display_blanking_pause, and is refused
# req_tklock_mode_change by D-Bus policy:
#   AccessDenied ... member="req_tklock_mode_change" destination="com.nokia.mce"
# So a locked phone still has to be unlocked by hand. That is what the
# display= field and the warning below are for: an unattended run on a locked
# device is flagged, not silently tabulated.
AWAKE=
keep_awake() {
    mce_req req_display_state_on
    mce_req req_tklock_mode_change string:unlocked
    while true; do mce_req req_display_blanking_pause; sleep 30; done &
    AWAKE=$!
}

# Wait on a condition, never on a duration: a local file is ready long before a
# fixed sleep ends, and a heavy page is not ready after one.
waitfor() {
    i=0; n=$((${2} * PER))
    while [ $i -lt $n ]; do
        grep -q "$1" "$OUT" && return 0
        sleep $TICK; i=$((i + 1))
    done
    return 1
}

mkfifo "$FIFO" || exit 1
env LD_LIBRARY_PATH="$PREFIX/lib" \
    WPE_BACKEND="$PREFIX/lib/libWPEBackend-n9.so" \
    GIO_MODULE_DIR="$PREFIX/lib/gio/modules" \
    FONTCONFIG_FILE="$PREFIX/etc/fonts/fonts.conf" \
    SSL_CERT_FILE="$PREFIX/etc/ssl/ca-certificates.crt" \
    DISPLAY="${DISPLAY:-:0}" \
    N9_NO_JS="$([ "$JS" = 1 ] && echo 0 || echo 1)" \
    "$ENGINE" about:blank < "$FIFO" > "$OUT" 2> "$ERR" &
EPID=$!
# Hold the write end open ourselves, or the engine sees EOF on stdin the moment
# the first `echo` finishes and quits.
exec 3> "$FIFO"

cleanup() {
    [ -n "$AWAKE" ] && kill $AWAKE 2>/dev/null
    echo quit >&3 2>/dev/null; exec 3>&-; sleep 1; kill $EPID 2>/dev/null
}
trap 'cleanup; exit 130' INT TERM

waitfor '^ready' 30 || { echo "bench: engine never said ready" >&2; cleanup; exit 1; }
echo "geometry 0 0 $W $H" >&3
sleep 2

keep_awake
MF0=$(memfree); SF0=$(swapfree)
DISP0=$(display_state)
T0=$(now)
echo "load $URL" >&3
waitfor '^loading 0' 120 || echo "bench: load never finished" >&2
LOADMS=$(( ($(now) - T0) * 10 ))

# Resolve the auxiliary pids ONCE, here. Not before the load: WebKit brings the
# web process up with the view, but measured on device it is not there two
# seconds after `geometry` -- resolving early reported -1 for the one process
# this whole harness exists to measure. A pattern that matches nothing is the
# documented way to report cpu=0 and believe it, so check both.
WPID=$(pidof_x "[W]PEWebProcess")
NPID=$(pidof_x "[W]PENetworkProcess")
[ -n "$WPID" ] || echo "bench: no WPEWebProcess -- its cpu/mem will be -1" >&2
[ -n "$NPID" ] || echo "bench: no WPENetworkProcess -- its cpu/mem will be -1" >&2

# Scripted scroll. Two different things are going on and conflating them
# under-measures badly.
#
# RECOGNITION needs time: TouchGestureController only promotes a press to a
# scroll once ~200 ms have passed, so the press is held before anything moves.
# That is what "touch events need pacing" is about.
#
# The DRAG itself in principle does not -- a real finger emits a motion every
# ~16 ms. Tried and rejected: 50 motions 0.2 s apart raised the frame count
# only 333 -> 362 with JS off while raising it 250 -> 360 with JS on, i.e. it
# ERASED the difference between the two arms rather than sharpening it. Frame
# production here is bounded by what the page costs to rasterise, not by how
# often it is asked to move. The defaults are therefore the ten-motions-one-
# second drag the plan prescribes; the knobs stay so the experiment is
# repeatable, not because they should be tuned per page.
# CPU is sampled per PHASE, not once at the end. Read cumulatively it is
# dominated by the load, which is where most of the JavaScript cost lands, and
# the scroll -- the thing the frame count is about -- disappears into it. The
# documented 3.6x frame measurement went with near-identical CPU (789 vs 769)
# precisely because it was scroll-window CPU; a cumulative counter cannot be
# compared against it.
F0=$(frames)
ECPU0=$(cpu $EPID); WCPU0=$(cpu "${WPID:-0}"); NCPU0=$(cpu "${NPID:-0}")
X=$((W / 2)); Y=$((H - 120))
echo "touch down 0 $X $Y" >&3
sleep 1                                  # let it become a scroll, not a click
i=1
while [ $i -le ${MOTIONS:-10} ]; do
    Y=$((Y - 40)); [ $Y -lt 80 ] && Y=$((H - 120))
    echo "touch motion 0 $X $Y" >&3
    sleep ${MOTION_GAP:-1}; i=$((i + 1))
done
echo "touch up 0 $X $Y" >&3
# Keep counting through the momentum fling. Releasing the finger does not end
# the scroll -- WPE keeps producing frames as it decelerates, and that is where
# the JS on/off difference actually lives: with scripts holding the main thread
# the fling starves, and with them off it runs at the panel rate. Stopping 1 s
# after `touch up` measured 163 frames off vs 174 on, i.e. no difference at
# all, against a documented 3.6x. The window is ~21 s to match the 22 s the
# original measurement used; comparing frame counts taken over different
# windows is meaningless, so this number is fixed, not tuned per page.
sleep 10
FRAMES=$(( $(frames) - F0 ))

ECPU=$(cpu $EPID); WCPU=$(cpu "${WPID:-0}"); NCPU=$(cpu "${NPID:-0}")
# Scroll-phase only. A -1 anywhere means the pid was never resolved; keep it
# as -1 rather than letting it subtract into a plausible-looking number.
sub() { [ "$1" -lt 0 ] || [ "$2" -lt 0 ] && echo -1 || echo $(( $1 - $2 )); }
ESCROLL=$(sub "$ECPU" "$ECPU0"); WSCROLL=$(sub "$WCPU" "$WCPU0"); NSCROLL=$(sub "$NCPU" "$NCPU0")
EMEM=$(mem $EPID);  WMEM=$(mem "${WPID:-0}");  NMEM=$(mem "${NPID:-0}")
WPROCS=$(webprocs)
MF1=$(memfree); SF1=$(swapfree)
DISP1=$(display_state)

# A TLS failure renders an error page that scrolls like a real one. Report it
# rather than letting it become a suspiciously good frame count.
FAILED=$(grep -c '^failed' "$OUT")

echo "label=$LABEL url=$URL js=$JS load_ms=$LOADMS frames=$FRAMES" \
     "engine_cpu=$ECPU web_cpu=$WCPU net_cpu=$NCPU" \
     "web_cpu_scroll=$WSCROLL engine_cpu_scroll=$ESCROLL net_cpu_scroll=$NSCROLL" \
     "engine_hwm_rss=$EMEM web_hwm_rss=$WMEM net_hwm_rss=$NMEM" \
     "webprocs=$WPROCS memfree_d=$((MF0 - MF1)) swapfree_d=$((SF0 - SF1))" \
     "failed=$FAILED display=$DISP0>$DISP1"

case "$DISP0$DISP1" in
    on:unlockedon:unlocked) ;;
    *) echo "bench: screen was $DISP0 -> $DISP1, NOT on:unlocked --" \
            "the frame count from this run is not comparable" >&2 ;;
esac

# Print the configuration the engine ACTUALLY used, not the one we asked for.
# An early render-scale A/B set a variable only the backend reads while the
# engine had it as a #define, and both arms measured the same thing.
grep -E 'javascript|memory limit|surface|cache dir' "$ERR" >&2

cleanup
# cleanup ends in a kill of a process that has usually already quit, so its
# status is not the run's status -- without this every run reports as failed.
exit 0
