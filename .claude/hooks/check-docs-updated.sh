#!/bin/bash
# Stop hook: when core threading/display/reload files changed in this
# round (working tree or the most recent commit) but none of the
# architecture docs / todos were touched, ask Claude to update them
# before stopping.  Modeled on veri-dns's Stop hook.
#
# stop_hook_active guard: if we already blocked once this turn, allow
# the stop -- Claude either updated the docs or judged no update
# necessary; never loop.

IN=$(cat)
echo "$IN" | jq -e '.stop_hook_active == true' >/dev/null 2>&1 && exit 0

cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}" || exit 0

CHANGED=$({ git diff --name-only HEAD; git show --name-only --pretty=format: HEAD; git ls-files --others --exclude-standard; } 2>/dev/null | sort -u)

CORE='^(src/(thread|systhread|keyboard|eval|alloc|emacs|webterm|web_event_loop|webfns|webfont)\.|lisp/(session-reload|web-term|slop-term)\.el|web-display/|web-client/)'
DOCS='^(PREEMPTIVE-THREADS-ARCHITECTURE\.md|WEB-DISPLAY-ARCHITECTURE\.md|todos\.md)$'

if echo "$CHANGED" | grep -qE "$CORE" && ! echo "$CHANGED" | grep -qE "$DOCS"; then
  cat <<'EOF'
{"decision": "block", "reason": "Core threading/display/reload files changed this round, but PREEMPTIVE-THREADS-ARCHITECTURE.md, WEB-DISPLAY-ARCHITECTURE.md, and todos.md were not updated. Update whichever of those (and your memory notes) the changes affect, then stop. If no documentation update is warranted, just stop again."}
EOF
fi
exit 0
