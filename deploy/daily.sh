#!/usr/bin/env bash
# One daily reading of the capture, appended to a log.
#
# Three things decide whether a day was any good, and only the third cannot be fixed
# afterwards: the recorder is running, the disk is not filling, and every symbol we
# think we are capturing actually appeared.
#
# Measured over the last day rather than the whole run, because a per-day series shows
# whether a result is persistent, which a cumulative figure hides. Captures older than
# an hour are gzipped, so the day's files are decompressed into scratch and thrown away.
#
#   deploy/daily.sh [>> ~/daily.log]

set -uo pipefail

REPO="${LLT_REPO:-$HOME/cpp-trading-engine}"
DATA="${LLT_DATA_DIR:-$HOME/data}"
ENV_FILE="${LLT_ENV_FILE:-/etc/llt/record.env}"
HOURS="${LLT_DAILY_HOURS:-25}"
CANCEL_PER_TRADE="${LLT_CANCEL_PER_TRADE:-6}"
BUFFER_BPS="${LLT_BUFFER_BPS:-2}"

BACKTEST="$REPO/build/release/backtest"

echo "================================================================"
echo "llt daily  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "================================================================"

# ---------------------------------------------------------------- 1. is it running

state=$(systemctl is-active llt-record 2>/dev/null || true)
echo "recorder        $state"
if [[ "$state" != "active" ]]; then
    echo "  !! not running. sudo systemctl restart llt-record"
    systemctl status llt-record --no-pager --lines 5 2>&1 | sed 's/^/  /'
fi

# ------------------------------------------------------------------- 2. is it fed

avail_kb=$(df --output=avail -k "$DATA" | tail -1 | tr -d ' ')
avail_gb=$(( avail_kb / 1024 / 1024 ))
echo "disk free       ${avail_gb} G"
[[ "$avail_gb" -lt 10 ]] && echo "  !! under 10 G. sudo systemctl start llt-compress.service"

echo "captured        $(du -sh "$DATA" 2>/dev/null | cut -f1) in $(find "$DATA" -name 'md-*' | wc -l) files"

newest=$(find "$DATA" -name 'md-*' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
if [[ -n "$newest" ]]; then
    age_min=$(( ( $(date +%s) - $(stat -c %Y "$newest") ) / 60 ))
    echo "newest file     ${age_min} min old"
    [[ "$age_min" -gt 90 ]] && echo "  !! nothing written in over 90 minutes"
fi

# ------------------------------------------------- 3. did every symbol show up

mapfile -t recent < <(find "$DATA" -name 'md-*.mdlog*' -mmin "-$(( HOURS * 60 ))" | sort)
if [[ "${#recent[@]}" -eq 0 ]]; then
    echo
    echo "no captures in the last ${HOURS}h; nothing to measure"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for src in "${recent[@]}"; do
    case "$src" in
        *.gz) cp "$src" "$WORK/" && gunzip -q "$WORK/$(basename "$src")" ;;
        *)    cp "$src" "$WORK/" ;;
    esac
done

echo
echo "last ${HOURS}h: ${#recent[@]} files, $(du -sh "$WORK" | cut -f1) decompressed"
echo

# A symbol with no fills is absent from the ranking even when it is being captured, so
# presence is checked against the recording itself rather than against the results.
present=$("$BACKTEST" "$WORK"/*.mdlog --symbol __ABSENT__ 2>&1 |
          awk '/events$/ {print $1}' | sort -u)

if [[ -r "$ENV_FILE" ]]; then
    expected=$(grep -oP '(?<=^LLT_SYMBOLS=).*' "$ENV_FILE" | tr ',' '\n' | tr -d ' ' | sort -u)
    missing=$(comm -23 <(echo "$expected") <(echo "$present"))
    if [[ -n "$missing" ]]; then
        echo "  !! configured but not captured:"
        echo "$missing" | sed 's/^/     /'
        echo "     this is the only loss that cannot be made up later"
    else
        echo "all $(echo "$expected" | wc -l) configured symbols present"
    fi
fi

# ------------------------------------------------------------------ 4. the ranking

echo
"$BACKTEST" "$WORK"/*.mdlog --all \
    --cancel-per-trade "$CANCEL_PER_TRADE" --buffer-bps "$BUFFER_BPS"

echo
echo "ignore the PnL for the first week: a few hundred fills swing day to day."
echo "what matters early is that every symbol is here and the fill counts are growing."
