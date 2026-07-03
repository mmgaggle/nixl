# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Shared SPDK nvmf-target bring-up/teardown for the VFIOUSER round-trip harnesses
# (run_roundtrip.sh, run_block_roundtrip.sh, run_block_metadata_reject.sh). Sourced
# by each script; it factors out the identical mktemp sock-dir setup, cleanup trap,
# nvmf_tgt launch, RPC readiness poll, and the (identical) VFIOUSER transport +
# listener RPCs. Each caller keeps ONLY its own bdev/namespace-create RPCs, its
# per-script bits (heap size, nqn/serial, sock-dir prefix), and its test invocation.
#
# Requires the caller to have set SPDK_ROOT. Exports for the caller after tgt_setup:
#   sock_dir  muser_dir  rpc_sock  rpc_py  nvmfpid
#
# Usage:
#   source "$(dirname "$0")/tgt_common.sh"
#   tgt_setup <sock-dir-prefix>     # e.g. spdk_kv_rt
#   tgt_start <heap-size-MiB>       # deliberate per-script -s heap size
#   tgt_create_vfiouser_transport
#   ... caller's bdev/kvdev + subsystem + namespace RPCs ...
#   tgt_add_vfiouser_listener "$nqn"
#   "$TEST_BIN" "trtype:VFIOUSER traddr:$muser_dir"

# EXIT trap: kill the target and remove the sock dir. Safe before tgt_start (no pid).
tgt_cleanup() {
    [[ -n "${nvmfpid:-}" ]] && kill "$nvmfpid" 2>/dev/null || true
    rm -rf "$sock_dir"
}

# Create the per-run socket directory + vfio-user muser path and arm the cleanup
# trap. $1 is an mktemp prefix distinguishing the three harnesses under /tmp.
tgt_setup() {
    sock_dir="$(mktemp -d "/tmp/${1}.XXXXXX")"
    muser_dir="$sock_dir/domain/muser0/0"
    rpc_sock="$sock_dir/rpc.sock"
    rpc_py="$SPDK_ROOT/scripts/rpc.py -s $rpc_sock"
    mkdir -p "$muser_dir"
    nvmfpid=""
    trap tgt_cleanup EXIT
}

# Poll the RPC socket until the target is ready (50 x 0.2s = up to 10s).
tgt_wait_rpc() {
    for _ in $(seq 1 50); do
        if $rpc_py rpc_get_methods >/dev/null 2>&1; then break; fi
        sleep 0.2
    done
}

# Launch nvmf_tgt and wait for it. $1 is the -s heap size in MiB; the per-script
# value is DELIBERATE (KV/metadata need less; the block malloc bdev needs more)
# so it is passed in rather than flattened to one number here.
tgt_start() {
    local heap_mb="$1"
    echo "== starting nvmf_tgt =="
    "$SPDK_ROOT/build/bin/nvmf_tgt" -r "$rpc_sock" -m 0x1 --no-huge -s "$heap_mb" &
    nvmfpid=$!
    tgt_wait_rpc
}

# The VFIOUSER transport is identical across all three harnesses.
tgt_create_vfiouser_transport() {
    $rpc_py nvmf_create_transport -t VFIOUSER
}

# Add the VFIOUSER listener for the given subsystem nqn at the muser socket.
tgt_add_vfiouser_listener() {
    $rpc_py nvmf_subsystem_add_listener "$1" -t VFIOUSER -a "$muser_dir" -s 0
}
