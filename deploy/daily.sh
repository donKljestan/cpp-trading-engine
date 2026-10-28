#!/usr/bin/env bash
# One daily reading of the capture, appended to a log.
#
# Three things decide whether a day was any good, and only the third cannot be fixed
# afterwards: the recorder is running, the disk is not filling, and every symbol we
# think we are capturing actually appeared.
#
# Measured over what has arrived since the previous run rather than a fixed window, so
# running late shortens nothing and running twice measures nothing twice. Captures
# older than an hour are gzipped, so the period's files are decompressed into scratch
# and thrown away.
#
#   deploy/daily.sh >> ~/daily.log 2>&1

set -uo pipefail

# Resolved from this script rather than assumed, so the checkout can live anywhere.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO="${LLT_REPO:-$(dirname "$SCRIPT_DIR")}"
DATA="${LLT_DATA_DIR:-$HOME/data}"
ENV_FILE="${LLT_ENV_FILE:-/etc/llt/record.env}"
STAMP="${LLT_DAILY_STAMP:-$HOME/.llt-daily-stamp}"
HOURS="${LLT_DAILY_HOURS:-25}"  # only used the first time, before a stamp exists
CANCEL_PER_TRADE="${LLT_CANCEL_PER_TRADE:-6}"
BUFFER_BPS="${LLT_BUFFER_BPS:-2}"

BACKTEST="$REPO/build/release/backtest"

echo "================================================================"
echo "llt daily  $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "================================================================"

bash "$REPO/deploy/healthcheck.sh" "$DATA"

# ------------------------------------------------- did every symbol show up

if [[ -f "$STAMP" ]]; then
    echo -e "\n== since $(date -u -r "$STAMP" '+%Y-%m-%d %H:%M UTC') =="
    mapfile -t recent < <(find "$DATA" -name 'md-*.mdlog*' -newer "$STAMP" 2>/dev/null | sort)
else
    echo -e "\n== last ${HOURS}h (no previous run recorded) =="
    mapfile -t recent < <(find "$DATA" -name 'md-*.mdlog*' -mmin "-$(( HOURS * 60 ))" 2>/dev/null | sort)
fi

if [[ "${#recent[@]}" -eq 0 ]]; then
    echo "  nothing new to measure"
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

echo "  ${#recent[@]} files, $(du -sh "$WORK" | cut -f1) decompressed"
echo

# A symbol with no fills is absent from the ranking even when it is being captured, so
# presence is checked against the recording itself rather than against the results.
present=$("$BACKTEST" "$WORK"/*.mdlog --symbol __ABSENT__ 2>&1 |
          awk '/events$/ {print $1}' | sort -u)

if [[ ! -r "$ENV_FILE" ]]; then
    echo "  !! cannot read $ENV_FILE, so nothing checks that every symbol is captured"
    echo "     sudo chmod 644 $ENV_FILE"
else
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

# ------------------------------------------------------------------ the ranking

echo
"$BACKTEST" "$WORK"/*.mdlog --all \
    --cancel-per-trade "$CANCEL_PER_TRADE" --buffer-bps "$BUFFER_BPS"

# Only now, so a run that died partway measures the same period again next time.
touch "$STAMP"

echo
echo "ignore the PnL for the first week: a few hundred fills swing day to day."
echo "what matters early is that every symbol is here and the fill counts are growing."
