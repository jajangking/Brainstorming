#!/bin/bash
# Benchmark: Termux vs Alpine (native, no supervisor)
# Spawner: 50x 'true', measure total & per-spawn

set -e

N=50

# Detect PREFIX
if [ -z "$PREFIX" ]; then
  PREFIX="/data/data/com.termux/files/home/alpine-rootfs/root"
fi

FAKE_RUN="$PREFIX/bin/fake-run"

echo "=== Benchmark: Termux vs Alpine ==="
echo "Iterations: $N"
echo ""

# Termux native (/system/bin/sh -> /bin/sh)
echo "--- Termux (native /system/bin/sh) ---"
start=$(date +%s%3N)
for i in $(seq 1 $N); do
  /system/bin/sh -c 'exit 0' > /dev/null
done
end=$(date +%s%3N)
termux_total=$((end - start))
termux_per=$((termux_total / N))
echo "Total: ${termux_total}ms, Per-spawn: ${termux_per}ms"

# Alpine (fake-run, shim path — native speed)
echo "--- Alpine (via fake-run, shim path) ---"
start=$(date +%s%3N)
for i in $(seq 1 $N); do
  "$FAKE_RUN" sh -c 'exit 0' > /dev/null
done
end=$(date +%s%3N)
alpine_total=$((end - start))
alpine_per=$((alpine_total / N))
echo "Total: ${alpine_total}ms, Per-spawn: ${alpine_per}ms"

echo ""
echo "=== Ratio (Alpine / Termux) ==="
echo "Total: $(awk "BEGIN {printf \"%.2f\", $alpine_total / $termux_total}")x"
echo "Per-spawn: $(awk "BEGIN {printf \"%.2f\", $alpine_per / $termux_per}")x"
