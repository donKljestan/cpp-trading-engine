#!/usr/bin/env bash
# One-command health check for an unattended capture.
#
# Not -e: every section should report even when an earlier one has nothing to say.

set -uo pipefail

DATA_DIR="${1:-$HOME/data}"
UNIT="llt-record"

printf '== service ==\n'
state="$(systemctl is-active "$UNIT" 2>/dev/null || true)"
since="$(systemctl show -p ActiveEnterTimestamp --value "$UNIT" 2>/dev/null || true)"
restarts="$(systemctl show -p NRestarts --value "$UNIT" 2>/dev/null || echo '?')"
printf '  state     %s since %s\n' "${state:-unknown}" "${since:-?}"
printf '  restarts  %s\n' "$restarts"

printf '\n== today ==\n'
log="$(journalctl -u "$UNIT" --since today --no-pager 2>/dev/null || true)"
connects="$(grep -c 'connected' <<< "$log" || true)"
# The venue cycles long-lived connections, so a clean close is routine and only the
# seconds it cost are worth knowing. An error is a different thing and is counted apart.
closes="$(grep -c 'closed by venue' <<< "$log" || true)"
errors="$(grep -ciE 'stream dropped.*error|connect failed|write failed' <<< "$log" || true)"
gap="$(grep -oP 'gap \K[0-9.]+' <<< "$log" | awk '{s+=$1} END {printf "%.1f", s+0}')"
printf '  connections   %s\n' "$connects"
printf '  venue closes  %s (%s s not captured)\n' "$closes" "$gap"
printf '  errors        %s\n' "$errors"

printf '\n== capture ==\n'
if [[ -d "$DATA_DIR" ]]; then
    files="$(find "$DATA_DIR" -name 'md-*.mdlog*' | wc -l)"
    total="$(du -sh "$DATA_DIR" 2>/dev/null | cut -f1)"
    newest="$(find "$DATA_DIR" -name 'md-*.mdlog' -printf '%T@ %p\n' 2>/dev/null \
              | sort -rn | head -1 | cut -d' ' -f2-)"
    printf '  files     %s\n' "$files"
    printf '  on disk   %s\n' "${total:-0}"
    if [[ -n "$newest" ]]; then
        age=$(( $(date +%s) - $(stat -c %Y "$newest") ))
        printf '  newest    %s (written %ss ago, %s)\n' \
            "$(basename "$newest")" "$age" "$(du -h "$newest" | cut -f1)"
    fi
else
    printf '  %s does not exist\n' "$DATA_DIR"
fi

printf '\n== disk ==\n'
df -h "$DATA_DIR" 2>/dev/null | tail -1 | awk '{printf "  %s used of %s, %s free (%s)\n", $3, $2, $4, $5}'

printf '\n== verdict ==\n'
problem=0
[[ "$state" == "active" ]] || { printf '  SERVICE NOT RUNNING\n'; problem=1; }
if [[ -n "${age:-}" ]] && (( age > 120 )); then
    printf '  no data written for %ss - the feed may be stalled\n' "$age"
    problem=1
fi
# A handful of reconnects a day is the venue cycling long-lived connections.
if [[ "$connects" =~ ^[0-9]+$ ]] && (( connects > 20 )); then
    printf '  %s reconnects today - investigate\n' "$connects"
    problem=1
fi
if [[ "$errors" =~ ^[0-9]+$ ]] && (( errors > 0 )); then
    printf '  %s error(s) today, not routine venue closes\n' "$errors"
    problem=1
fi
# Gaps are the only loss that cannot be made good later.
if [[ -n "${gap:-}" ]] && awk -v g="$gap" 'BEGIN {exit !(g > 60)}'; then
    printf '  %s s of capture missing today\n' "$gap"
    problem=1
fi
(( problem == 0 )) && printf '  healthy\n'
