#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Device-mode (PCIE) BLOCK round-trip runner for the generic SPDK NIXL backend.
#
# Runs the SAME direct-engine block round-trip test as run_block_roundtrip.sh,
# but against a REAL local NVMe controller over the PCIE transport
# ("trtype:PCIE traddr:<BDF>") instead of an SPDK vfio-user target. The plugin
# selects the transport purely from the transport-ID string, so this is a
# config-only change with NO code fork: the datapath, the region-bounded SGL,
# and the byte->LBA / alignment / capacity validation are identical to the
# VFIOUSER path.
#
# =============================================================================
# !!  DESTRUCTIVE  --  THIS TEST WRITES LBAs (INCLUDING LBA 0) ON THE DEVICE  !!
# =============================================================================
# The round-trip WRITES test patterns to the namespace (LBA 0 and a sweep up to
# ~60 MiB), overwriting whatever is there -- partition tables, filesystems, data.
# Point PCI_BDF at a DEDICATED SCRATCH NVMe device/namespace ONLY: a spare M.2, a
# scratch namespace, or a loopback -- NEVER a device holding a mounted filesystem
# or data you care about. There is no undo.
#
# -----------------------------------------------------------------------------
# Safety validation (runs BEFORE any bind or write; fails safe)
# -----------------------------------------------------------------------------
# Before touching anything the runner self-validates PCI_BDF and REFUSES (exits
# non-zero, no bind, no write) unless the target is a safe scratch NVMe:
#   * the PCI device must EXIST (/sys/bus/pci/devices/$PCI_BDF present);
#   * it must be an NVMe controller (PCI class 0x0108xx);
#   * it must NOT be in active/precious use -- any of the controller's kernel
#     block namespaces (or their partitions) that is MOUNTED, or holds the ROOT
#     filesystem, is refused UNCONDITIONALLY (FORCE cannot override this);
#   * a namespace that carries a recognized filesystem/partition SIGNATURE
#     (blkid / lsblk FSTYPE|PTTYPE) is refused unless you pass FORCE=1 to
#     deliberately overwrite it. A truly blank scratch device passes without
#     FORCE. This also covers a whole-disk filesystem with no partition table.
# After an optional bind (BIND=1) the runner asserts the target actually landed
# on the vfio-pci driver before it writes; otherwise it aborts without writing.
#
# -----------------------------------------------------------------------------
# Host requirements
# -----------------------------------------------------------------------------
#   * IOMMU enabled. The shim brings up its SPDK env with no_huge=true, which
#     selects DPDK IOVA=VA; a real NVMe controller bound to vfio-pci can only
#     DMA to those virtual IOVAs through an IOMMU. Boot with the IOMMU on:
#       - Intel: intel_iommu=on   (kernel cmdline)
#       - AMD:   amd_iommu=on iommu=pt
#     Verify /sys/kernel/iommu_groups/ is non-empty.
#   * The target NVMe controller bound to the vfio-pci userspace driver (see
#     "Binding" below). SPDK cannot attach a controller still owned by the
#     kernel nvme driver.
#
# -----------------------------------------------------------------------------
# Binding the scratch device to vfio-pci
# -----------------------------------------------------------------------------
# SPDK ships scripts/setup.sh to bind NVMe controllers to vfio-pci. Its DEFAULT
# behavior binds EVERY NVMe controller it finds -- which would rip your boot
# drive out from under the kernel. ALWAYS scope it to the single scratch BDF with
# PCI_ALLOWED so nothing else is touched. Pass the vars THROUGH `sudo env` so a
# hardened sudoers (env_reset) cannot strip PCI_ALLOWED and turn the scoped bind
# back into a bind-everything:
#
#     sudo env PCI_ALLOWED="$PCI_BDF" HUGEMEM=64 \
#         "$SPDK_ROOT/scripts/setup.sh"
#
# To hand the device back to the kernel afterwards (also via `sudo env`):
#
#     sudo env PCI_ALLOWED="$PCI_BDF" "$SPDK_ROOT/scripts/setup.sh" reset
#
# This runner can perform the scoped bind for you if you pass BIND=1 (it invokes
# the command above via sudo env); otherwise it assumes the device is already
# bound and asserts vfio-pci before writing.
#
# -----------------------------------------------------------------------------
# Usage (one command, tomorrow, on the scratch M.2)
# -----------------------------------------------------------------------------
#     PCI_BDF=0000:xx:00.0 BIND=1 ./run_block_pcie.sh
#
#   or, if the device is already bound to vfio-pci:
#
#     PCI_BDF=0000:xx:00.0 ./run_block_pcie.sh
#
#   a device with an existing signature you INTEND to overwrite:
#
#     PCI_BDF=0000:xx:00.0 BIND=1 FORCE=1 ./run_block_pcie.sh
#
# Env overrides:
#   PCI_BDF     (REQUIRED) PCI bus:device.function of the SCRATCH NVMe controller
#               to test, e.g. 0000:65:00.0. No default -- the runner fails
#               cleanly if unset, so it can never pick a device for you. A bare
#               bus:dev.func (no domain) is normalized to 0000:bus:dev.func.
#   SPDK_ROOT   Path to an SPDK tree whose scripts/setup.sh binds the device.
#               Required only when BIND=1; no default. (With BIND=0 the device
#               is assumed already bound to vfio-pci and SPDK_ROOT is unused.)
#   TEST_BIN    Path to the built spdk_kv_block_roundtrip_test binary.
#   BIND        1 => run the scoped `sudo env ... setup.sh` bind before testing.
#               Default 0 (assume the device is already bound to vfio-pci).
#   FORCE       1 => proceed even when the device carries an existing filesystem
#               / partition signature (deliberate overwrite). Does NOT override
#               a mounted / root-filesystem device (those are always refused).
#   ASSUME_YES  1 => skip the interactive scratch-device confirmation prompt
#               (for non-interactive / automated runs). Default: prompt.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

# --- Require an explicit scratch BDF; never guess a device. -----------------
if [[ -z "${PCI_BDF:-}" ]]; then
    cat >&2 <<EOF
error: PCI_BDF is not set.

This runner writes LBAs to a REAL NVMe device, so it will not pick one for you.
Set PCI_BDF to the PCI address of a DEDICATED SCRATCH NVMe controller, e.g.:

    PCI_BDF=0000:65:00.0 BIND=1 $0

List NVMe controllers and their BDFs with:  lspci -Dnn -d ::0108
EOF
    exit 2
fi

# Normalize a bare bus:dev.func (single colon) to a fully-qualified domain BDF,
# which is what /sys/bus/pci/devices uses.
if [[ "$PCI_BDF" != *:*:* ]]; then
    PCI_BDF="0000:$PCI_BDF"
fi

SPDK_ROOT="${SPDK_ROOT:-}"
TEST_BIN="${TEST_BIN:-$here/../../../builddir/src/plugins/spdk_kv/spdk_kv_block_roundtrip_test}"
BIND="${BIND:-0}"
FORCE="${FORCE:-0}"
ASSUME_YES="${ASSUME_YES:-0}"

if [[ ! -x "$TEST_BIN" ]]; then
    echo "error: test binary not found/executable: $TEST_BIN" >&2
    echo "       build it with: ninja -C builddir src/plugins/spdk_kv/spdk_kv_block_roundtrip_test" >&2
    exit 1
fi
TEST_BIN="$(readlink -f "$TEST_BIN")"

# ---------------------------------------------------------------------------
# Up-front self-validation: refuse a wrong / in-use / precious device BEFORE any
# bind or write. Returns 0 to proceed, non-zero to REFUSE (caller exits).
# ---------------------------------------------------------------------------
validate_bdf() {
    local bdf="$1"
    local sysdev="/sys/bus/pci/devices/$bdf"

    # (a) the PCI device must exist.
    if [[ ! -e "$sysdev" ]]; then
        echo "REFUSE: no PCI device at $bdf ($sysdev not present)." >&2
        echo "        list NVMe controllers with: lspci -Dnn -d ::0108" >&2
        return 1
    fi

    # (b) it must be an NVMe controller (PCI class 0x0108xx).
    local class=""
    [[ -r "$sysdev/class" ]] && class="$(cat "$sysdev/class" 2>/dev/null || true)"
    if [[ "$class" != 0x0108* ]]; then
        echo "REFUSE: $bdf is not an NVMe controller (PCI class '${class:-unknown}', expected 0x0108xx)." >&2
        return 1
    fi

    # (c) it must not be in active/precious use. Enumerate the controller's
    # kernel block namespaces; when the controller is already bound to vfio-pci
    # there are none (nothing the kernel can mount), so this is a no-op then.
    shopt -s nullglob
    local ns_dirs=("$sysdev"/nvme/nvme*/nvme*n*)
    shopt -u nullglob
    if [[ ${#ns_dirs[@]} -eq 0 ]]; then
        echo "note: $bdf exposes no kernel NVMe block namespaces (already on a" >&2
        echo "      userspace driver, or none present); mount/signature checks skipped." >&2
        return 0
    fi

    local rootsrc=""
    if command -v findmnt >/dev/null 2>&1; then
        rootsrc="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
    fi

    local hard=0 soft=0 have_probe=0 nsdir ns devnode
    for nsdir in "${ns_dirs[@]}"; do
        ns="$(basename "$nsdir")"        # e.g. nvme0n1
        devnode="/dev/$ns"
        [[ -b "$devnode" ]] || continue

        if command -v lsblk >/dev/null 2>&1; then
            have_probe=1
            # The namespace + its partitions and their mountpoints / signatures.
            local mounts fstypes names child
            mounts="$(lsblk -nro MOUNTPOINT "$devnode" 2>/dev/null | grep -v '^$' || true)"
            fstypes="$( { lsblk -nro FSTYPE "$devnode" 2>/dev/null; \
                          lsblk -nro PTTYPE "$devnode" 2>/dev/null; } | grep -v '^$' || true)"
            if [[ -n "$mounts" ]]; then
                echo "REFUSE: $devnode (or a partition) is MOUNTED:" >&2
                echo "$mounts" | sed 's/^/          at /' >&2
                hard=1
            fi
            if [[ -n "$rootsrc" ]]; then
                names="$(lsblk -nro NAME "$devnode" 2>/dev/null || true)"
                for child in $names; do
                    if [[ "/dev/$child" == "$rootsrc" ]]; then
                        echo "REFUSE: $devnode holds the ROOT filesystem ($rootsrc)." >&2
                        hard=1
                    fi
                done
            fi
            if [[ -n "$fstypes" ]]; then
                echo "REFUSE: $devnode carries filesystem/partition signature(s): $(echo "$fstypes" | tr '\n' ' ')" >&2
                soft=1
            fi
        elif command -v blkid >/dev/null 2>&1; then
            have_probe=1
            # Fallback: /proc/mounts for mounts + blkid -p for signatures.
            if grep -q "^$devnode" /proc/mounts 2>/dev/null; then
                echo "REFUSE: $devnode (or a partition) appears in /proc/mounts (mounted)." >&2
                hard=1
            fi
            if [[ -n "$rootsrc" && "$devnode" == "$rootsrc"* ]]; then
                echo "REFUSE: $devnode holds the ROOT filesystem ($rootsrc)." >&2
                hard=1
            fi
            if blkid -p "$devnode" >/dev/null 2>&1; then
                echo "REFUSE: $devnode carries a filesystem/partition signature (blkid -p)." >&2
                soft=1
            fi
        fi
    done

    if [[ $have_probe -eq 0 ]]; then
        echo "REFUSE: neither lsblk nor blkid is available to verify $bdf is scratch." >&2
        echo "        install util-linux, or pass FORCE=1 only if you are certain it is scratch." >&2
        [[ "$FORCE" == "1" ]] && { echo "        FORCE=1 set -> proceeding UNVERIFIED." >&2; return 0; }
        return 1
    fi

    # A mounted / root device is refused UNCONDITIONALLY (FORCE cannot override).
    if [[ $hard -eq 1 ]]; then
        echo "error: $bdf is in active/precious use; refusing unconditionally (FORCE cannot override)." >&2
        return 1
    fi
    # An existing signature is refused unless FORCE=1 (deliberate overwrite).
    if [[ $soft -eq 1 ]]; then
        if [[ "$FORCE" == "1" ]]; then
            echo "WARNING: $bdf carries existing signatures; FORCE=1 set -> proceeding to OVERWRITE." >&2
        else
            echo "error: $bdf carries existing filesystem/partition signatures; refusing." >&2
            echo "       If this really is a scratch device to overwrite, re-run with FORCE=1." >&2
            return 1
        fi
    fi
    return 0
}

if ! validate_bdf "$PCI_BDF"; then
    echo "aborted: $PCI_BDF did not pass scratch-device validation (no bind, no write performed)." >&2
    exit 4
fi

# --- Prominent scratch-only warning + confirmation. -------------------------
cat >&2 <<EOF

============================================================================
  DESTRUCTIVE: this test WRITES LBAs (including LBA 0) on PCI device
      $PCI_BDF
  Everything on that namespace (partition table, filesystem, data) will be
  OVERWRITTEN. Use a DEDICATED SCRATCH device ONLY. There is no undo.
============================================================================
EOF

if [[ "$ASSUME_YES" != "1" ]]; then
    if [[ -t 0 ]]; then
        read -r -p "Type the BDF ($PCI_BDF) to confirm it is scratch: " reply
        if [[ "$reply" != "$PCI_BDF" ]]; then
            echo "aborted: confirmation ('$reply') did not match PCI_BDF ('$PCI_BDF')." >&2
            exit 3
        fi
    else
        echo "error: not a TTY and ASSUME_YES!=1; refusing to write without confirmation." >&2
        echo "       re-run with ASSUME_YES=1 once you have verified $PCI_BDF is scratch." >&2
        exit 3
    fi
fi

# --- Soft IOMMU sanity check (warn, do not block). --------------------------
if [[ ! -d /sys/kernel/iommu_groups ]] || [[ -z "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]]; then
    echo "WARNING: /sys/kernel/iommu_groups is empty -- IOMMU may be disabled." >&2
    echo "         vfio-pci + the shim's no_huge IOVA=VA env need an IOMMU; the" >&2
    echo "         attach will likely fail. Boot with intel_iommu=on / amd_iommu=on." >&2
fi

# --- Optionally bind the scratch device to vfio-pci (scoped to this BDF). ----
# Pass PCI_ALLOWED/HUGEMEM through `sudo env` so a hardened sudoers (env_reset
# without SETENV) cannot strip them -- an empty PCI_ALLOWED would make setup.sh
# bind EVERY NVMe controller, including the boot drive.
if [[ "$BIND" == "1" ]]; then
    if [[ -z "$SPDK_ROOT" ]]; then
        echo "error: BIND=1 requires SPDK_ROOT (path to an SPDK tree with scripts/setup.sh)." >&2
        echo "       set SPDK_ROOT=/path/to/spdk, or bind the device yourself and run with BIND=0." >&2
        exit 1
    fi
    if [[ ! -x "$SPDK_ROOT/scripts/setup.sh" ]]; then
        echo "error: BIND=1 but $SPDK_ROOT/scripts/setup.sh not found/executable." >&2
        echo "       set SPDK_ROOT to an SPDK tree, or bind the device manually." >&2
        exit 1
    fi
    echo "== binding $PCI_BDF to vfio-pci (scoped via PCI_ALLOWED, through sudo env) ==" >&2
    sudo env PCI_ALLOWED="$PCI_BDF" HUGEMEM="${HUGEMEM:-64}" "$SPDK_ROOT/scripts/setup.sh"
fi

# --- Assert the target is actually on vfio-pci BEFORE writing. --------------
# Catches "setup.sh skipped the target", "PCI_ALLOWED got stripped so the
# target's state is unexpected", and "BIND=0 but you forgot to bind it".
drv=""
if [[ -L "/sys/bus/pci/devices/$PCI_BDF/driver" ]]; then
    drv="$(basename "$(readlink -f "/sys/bus/pci/devices/$PCI_BDF/driver" 2>/dev/null || true)")"
fi
if [[ "$drv" != "vfio-pci" ]]; then
    echo "error: $PCI_BDF is bound to driver '${drv:-<none>}', not vfio-pci; aborting before any write." >&2
    echo "       bind it first:  sudo env PCI_ALLOWED=\"$PCI_BDF\" HUGEMEM=64 \"$SPDK_ROOT/scripts/setup.sh\"" >&2
    echo "       (or re-run this script with BIND=1)" >&2
    exit 5
fi

# --- Run the block round-trip over PCIE. ------------------------------------
# Selected purely by the transport-ID string; identical datapath to VFIOUSER.
echo "== running block round-trip over PCIE against $PCI_BDF ==" >&2
# `|| rc=$?` captures the exit code without tripping `set -e`, so the
# troubleshooting hint below still runs on failure.
rc=0
"$TEST_BIN" "trtype:PCIE traddr:$PCI_BDF" || rc=$?
echo "== test exit code: $rc ==" >&2

if [[ $rc -ne 0 ]]; then
    cat >&2 <<EOF

The PCIE attach or round-trip failed. Common causes:
  * device still owned by the kernel nvme driver -> bind it to vfio-pci:
        sudo env PCI_ALLOWED="$PCI_BDF" HUGEMEM=64 "$SPDK_ROOT/scripts/setup.sh"
    (or re-run this script with BIND=1)
  * IOMMU disabled -> boot with intel_iommu=on / amd_iommu=on
  * wrong BDF -> confirm with: lspci -Dnn -d ::0108
EOF
fi
exit $rc
