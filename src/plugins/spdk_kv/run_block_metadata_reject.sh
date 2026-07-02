#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Negative test for the block open guard: brings up an SPDK nvmf target with a
# metadata-formatted (interleaved / extended-LBA) malloc bdev exposed as an NVM
# (block) namespace over VFIOUSER, then runs the SPDK direct-engine BLOCK
# round-trip test against it and asserts the engine/shim REFUSES it CLEANLY at
# init -- non-zero exit, a distinct -ENOTSUP (rc=-95) refusal in the log -- rather
# than faulting later at SGL build.
#
# Rationale: the block datapath sizes each transfer from the DATA-only sector
# size, but lib/nvme sizes the DMA payload of a metadata-formatted namespace from
# the EXTENDED sector size (data + metadata). The shim detects the metadata
# format at open (spdk_nvme_ns_get_md_size > 0 / supports_extended_lba) and fails
# with -ENOTSUP so init fails informatively instead of the payload/SGL mismatch
# faulting mid-transfer. Full metadata / DIF/DIX support is out of scope.
#
# Env overrides:
#   SPDK_ROOT  (required) path to a target-capable SPDK build providing a built
#              build/bin/nvmf_tgt whose bdev_malloc_create supports the metadata
#              options (--md-size / --md-interleave).
#   TEST_BIN   path to the built spdk_kv_block_roundtrip_test binary
#   BLOCK_SIZE malloc bdev data block size in bytes (default 512)
#   MD_SIZE    malloc bdev metadata size in bytes (default 8; must be > 0)
#   DEV_SIZE_MB malloc bdev size in MiB (default 64)
set -euo pipefail

if [[ -z "${SPDK_ROOT:-}" ]]; then
    echo "error: SPDK_ROOT must be set to a target-capable SPDK build" >&2
    exit 1
fi
TEST_BIN="${TEST_BIN:-$(dirname "$0")/../../../builddir/src/plugins/spdk_kv/spdk_kv_block_roundtrip_test}"
TEST_BIN="$(readlink -f "$TEST_BIN")"
BLOCK_SIZE="${BLOCK_SIZE:-512}"
MD_SIZE="${MD_SIZE:-8}"
DEV_SIZE_MB="${DEV_SIZE_MB:-64}"

nqn="nqn.2026-06.io.spdk:spdk-blkmd-cnode0"
bdev_name="SpdkBlkMdMalloc0"
sock_dir="$(mktemp -d /tmp/spdk_blkmd_rt.XXXXXX)"
muser_dir="$sock_dir/domain/muser0/0"
rpc_sock="$sock_dir/rpc.sock"
rpc_py="$SPDK_ROOT/scripts/rpc.py -s $rpc_sock"
mkdir -p "$muser_dir"

nvmfpid=""
cleanup() {
    [[ -n "$nvmfpid" ]] && kill "$nvmfpid" 2>/dev/null || true
    rm -rf "$sock_dir"
}
trap cleanup EXIT

echo "== starting nvmf_tgt =="
"$SPDK_ROOT/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x1 --no-huge -s 1024 &
nvmfpid=$!

# Wait for the RPC socket to be ready.
for _ in $(seq 1 50); do
    if $rpc_py rpc_get_methods >/dev/null 2>&1; then break; fi
    sleep 0.2
done

# Create a malloc bdev with INTERLEAVED per-LBA metadata: the extended sector
# size is BLOCK_SIZE + MD_SIZE, so extended_sector_size > sector_size and the
# host sees md_size > 0. If this SPDK build cannot create a metadata malloc bdev,
# the guard cannot be exercised here -- report and skip cleanly (rc 0) rather than
# fail, so the harness treats "no metadata-capable target" as not-applicable.
echo "== configuring metadata (interleaved / extended-LBA) block namespace =="
$rpc_py nvmf_create_transport -t VFIOUSER
if ! $rpc_py bdev_malloc_create -b "$bdev_name" -m "$MD_SIZE" -i "$DEV_SIZE_MB" "$BLOCK_SIZE"; then
    echo "SKIP: this SPDK build cannot create a metadata-formatted malloc bdev" >&2
    echo "SKIP: block metadata-rejection guard not exercised (no metadata-capable target)" >&2
    exit 0
fi
$rpc_py nvmf_create_subsystem "$nqn" -s SPDKBLKMD01 -a
# Do NOT pass --hide-metadata: the host must SEE the metadata format so the guard
# (md_size > 0) fires.
$rpc_py nvmf_subsystem_add_ns "$nqn" "$bdev_name"
$rpc_py nvmf_subsystem_add_listener "$nqn" -t VFIOUSER -a "$muser_dir" -s 0

echo "== running block round-trip test (expecting CLEAN refusal at init) =="
set +e
out="$("$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir" 2>&1)"
rc=$?
set -e
echo "$out"
echo "== test exit code: $rc =="

# The engine must have REFUSED the metadata namespace: non-zero exit AND the
# distinct -ENOTSUP (rc=-95) metadata refusal in the log (not a crash or an
# unrelated init failure, and NOT a successful round-trip on a faulting payload).
if [[ $rc -eq 0 ]]; then
    echo "FAIL: engine accepted a metadata-formatted block namespace (expected clean refusal)" >&2
    exit 1
fi
if ! grep -q "rc=-95" <<<"$out"; then
    echo "FAIL: init failed but not via the -ENOTSUP metadata guard (expected 'rc=-95')" >&2
    exit 1
fi
if ! grep -qi "per-LBA metadata" <<<"$out"; then
    echo "FAIL: missing the informative metadata-refusal message" >&2
    exit 1
fi
echo "== metadata-formatted block namespace correctly REFUSED (clean -ENOTSUP init failure) =="
echo "run_block_metadata_reject: PASS"
exit 0
