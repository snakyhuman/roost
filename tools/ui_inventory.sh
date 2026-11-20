#!/bin/sh
# Captures every state of the roost window as PNGs, at scale 1 and 2.
#   tools/ui_inventory.sh <out-dir>
# Offscreen only: no window is shown, the real config and roostd are not used.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: ui_inventory.sh <out-dir>}
mkdir -p "$out"
cmake --preset dev -S "$root" -B "$root/build/dev" > /dev/null
cmake --build "$root/build/dev" -j4 --target ui_inventory
for scale in 1 2; do
    QT_QPA_PLATFORM=offscreen QML_DISABLE_DISK_CACHE=1 QT_SCALE_FACTOR=$scale \
        UI_INVENTORY_OUT="$out" "$root/build/dev/bin/ui_inventory"
done
