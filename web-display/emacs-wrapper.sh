#!/bin/bash
# emacs-wrapper.sh --- run Emacs, restarting it on hot-reload requests.
#
# Usage: emacs-wrapper.sh /path/to/emacs [args...]
#
# Restarts Emacs (picking up a freshly rebuilt binary at the same
# path) when it exits with code 42 -- the exit code used by
# M-x session-reload (lisp/session-reload.el) -- or when the reload
# marker file exists, which slop-reload.sh drops before force-killing
# a wedged Emacs.  Any other exit terminates the wrapper.
#
# The Electron app (web-client/electron/main.js) uses this script
# automatically when it exists.

EMACS_BIN="${1:?usage: emacs-wrapper.sh /path/to/emacs [args...]}"
shift

RELOAD_MARKER="/tmp/emacs-reload-requested"

while true; do
  rm -f "$RELOAD_MARKER"
  "$EMACS_BIN" --eval "(when (require 'session-reload nil t) (session-reload-init))" "$@"
  code=$?
  if [ "$code" = "42" ] || [ -f "$RELOAD_MARKER" ]; then
    echo "emacs-wrapper: reload requested (exit code $code); restarting Emacs..." >&2
    continue
  fi
  echo "emacs-wrapper: Emacs exited with code $code." >&2
  exit "$code"
done
