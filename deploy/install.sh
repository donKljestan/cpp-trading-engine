#!/usr/bin/env bash
# Installs the recorder and compression units.
#
# systemd will not accept a variable as the executable path in ExecStart, so the units
# are templates filled in here from this checkout's real location rather than shipped
# with a directory baked in that only matches one machine.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Resolve against the invoking user, not root, when this runs under sudo.
RUN_USER="${SUDO_USER:-$(id -un)}"
USER_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
DATA_DIR="${LLT_DATA_DIR:-$USER_HOME/data}"

if [[ ! -x "$REPO_ROOT/build/release/md_record" ]]; then
    echo "md_record not built. run ./build.sh release first." >&2
    exit 1
fi

echo "repo   $REPO_ROOT"
echo "user   $RUN_USER"
echo "data   $DATA_DIR"

mkdir -p "$DATA_DIR"
sudo mkdir -p /etc/llt

# Preserved across upgrades: the capture set is an operational choice, not code.
if [[ ! -f /etc/llt/record.env ]]; then
    echo "LLT_SYMBOLS=${LLT_SYMBOLS:-TACUSDT,CFGUSDT,CVXUSDT,DATAIPUSDT,HANAUSDT}" \
        | sudo tee /etc/llt/record.env > /dev/null
    sudo chmod 600 /etc/llt/record.env
    echo "wrote /etc/llt/record.env"
else
    echo "keeping existing /etc/llt/record.env"
fi

for unit in llt-record.service llt-compress.service; do
    sed -e "s|@REPO@|$REPO_ROOT|g" \
        -e "s|@USER@|$RUN_USER|g" \
        -e "s|@DATA@|$DATA_DIR|g" \
        "$REPO_ROOT/deploy/$unit" | sudo tee "/etc/systemd/system/$unit" > /dev/null
done
sudo cp "$REPO_ROOT/deploy/llt-compress.timer" /etc/systemd/system/

sudo systemctl daemon-reload
sudo systemctl enable --now llt-compress.timer

echo
echo "installed. start recording with:"
echo "  sudo systemctl enable --now llt-record"
echo "  systemctl status llt-record"
