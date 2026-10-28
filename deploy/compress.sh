#!/usr/bin/env bash
# Compresses captures the recorder has finished with.
#
# JSON compresses roughly 8-10x, which is the difference between 31 GB and 4 GB over a
# two-week run. Done out of process on a timer rather than inside md_record, so the
# capture path never spends CPU on anything but not dropping messages.
#
# Rotation is hourly, so a file untouched for 70 minutes is certainly closed.

set -euo pipefail

DATA_DIR="${1:-$HOME/data}"

find "$DATA_DIR" -name '*.mdlog' -mmin +70 -print0 | xargs -0 -r gzip -9

echo "compressed; $(du -sh "$DATA_DIR" | cut -f1) in use"
