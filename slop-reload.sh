#!/bin/bash
# Rebuild Emacs and hot-restart the running instance, preserving the
# session (buffers, windows, points).  See lisp/session-reload.el.
#
# Usage: ./slop-reload.sh [--no-build]
#
# 1. Builds Emacs (unless --no-build).
# 2. Asks the running Emacs to save its session and exit with code 42
#    (via emacsclient); web-display/emacs-wrapper.sh then restarts the
#    new binary, which restores the session.
# 3. If Emacs is wedged and emacsclient fails, falls back to dropping
#    the reload marker and force-killing it (session save best-effort
#    skipped in that case).

set -e
cd "$(cd "$(dirname "$0")" && pwd)"

RELOAD_MARKER="/tmp/emacs-reload-requested"
EMACSCLIENT="./lib-src/emacsclient"
[ -x "$EMACSCLIENT" ] || EMACSCLIENT="emacsclient"

# Build unless --no-build.
if [ "$1" != "--no-build" ]; then
    echo "Building..."
    make -C src -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" || exit 1
    echo "Build succeeded."
fi

# Graceful path: Emacs saves the session and exits 42; the wrapper
# restarts it.  Always target the dedicated "web-emacs" server so we
# can never reach a different Emacs session on this machine.  The
# reload itself runs from a timer so our eval gets its reply before
# Emacs exits; treat an *ERROR* reply as failure.
echo "Requesting hot reload..."
REPLY=$("$EMACSCLIENT" -s web-emacs -e '(progn (require (quote session-reload)) (run-at-time 0.1 nil (function session-reload)) t)' 2>/dev/null) || REPLY=""
if [ "$REPLY" = "t" ]; then
    echo "Reload requested; the wrapper will restart Emacs with the new binary."
    exit 0
fi

# Fallback: force-kill; the wrapper still restarts (marker), but the
# session is whatever was last saved.
echo "emacsclient failed; falling back to force-kill."
EMACS_PID=$(pgrep -f 'src/emacs' 2>/dev/null | head -1)
if [ -z "$EMACS_PID" ]; then
    echo "No running Emacs found."
    exit 1
fi
touch "$RELOAD_MARKER"
kill -9 "$EMACS_PID"
echo "Killed Emacs ($EMACS_PID); wrapper will restart it with the new binary."
