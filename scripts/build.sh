#!/usr/bin/env bash
set -euo pipefail

pushd "$PWD/$(dirname "$0")/.." > /dev/null

echo "=== Building baseline (no prefetcher) ==="
./config.sh champsim_config.json
make -j"$(nproc)"
echo "  -> bin/champsim"

echo ""
echo "=== Building next_line L2C prefetcher ==="
./config.sh scripts/next_line_l2c.json
make -j"$(nproc)"
echo "  -> bin/champsim_next_line"

echo ""
echo "=== Building IMP L2C prefetcher ==="
./config.sh scripts/imp_l2c.json
make -j"$(nproc)"
echo "  -> bin/champsim_imp"

echo ""
echo "Done. All binaries are in bin/"
