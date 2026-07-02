<!--
SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# NIXL Generic SPDK NVMe-KV Plugin (`SPDK_KV`)

A clean-sheet, **transport-agnostic** and **backend-agnostic** NIXL backend that
speaks the **ratified NVMe Key-Value command set** over the SPDK NVMe driver
(`lib/nvme`). It works against **any** SPDK NVMe-KV target — an in-memory
`kvdev`, a librados-backed `kvdev`, a DPU-presented VF, an emulated NVMe in a
guest — and is intended to be upstreamable to `ai-dynamo/nixl` as *the* generic
SPDK KV backend.

It carries **no backend-specific behavior, no KV Exec, and no long-key path** by
design.

This directory implements the **walking skeleton** plus **Exist (QUERY) and
value auto-sizing**.

## Scope

In:
- NIXL `WRITE` → KV **Store**, NIXL `READ` → KV **Retrieve**.
- NIXL `queryMem` / QUERY → KV **Exist** (cache hit/miss, **no data transfer**).
- **Value auto-sizing** on Retrieve: a short host buffer surfaces the device's
  **TRUE value length** (completion `cdw0`) so the caller can **resize and
  re-Retrieve** instead of silently truncating.
- **16-byte inline keys**, taken **verbatim** (opaque; no lineage parsing).
- **Small/moderate values** in **host DRAM** (single staging buffer per op).
- Connect via a **standard SPDK transport ID** (`trtype:VFIOUSER traddr:<sock>`).

Out:
- VRAM / P2PDMA.
- Large-value **region-bounded (multi-region) SGL** up to ~64 MiB.
- Delete/List, as needed.
- Device-mode (`PCIE`) transport parity.
- KV **Exec** and **long keys** — permanently out of scope for this generic
  plugin.

## Operation and memory-type mapping

| NIXL operation | Local mem  | Remote mem | NVMe KV command |
| -------------- | ---------- | ---------- | --------------- |
| `NIXL_WRITE`   | `DRAM_SEG` | `OBJ_SEG`  | KV **Store**    |
| `NIXL_READ`    | `DRAM_SEG` | `OBJ_SEG`  | KV **Retrieve** |
| `queryMem`     | —          | `OBJ_SEG`  | KV **Exist**    |

The remote `OBJ_SEG` descriptor carries the NIXL block identifier in its
`metaInfo` blob. The engine takes those bytes **verbatim** as the inline NVMe-KV
key (1–16 bytes). An empty or over-16-byte key is **rejected**
(`NIXL_ERR_INVALID_PARAM`), never truncated (truncation would alias distinct
keys sharing a prefix and corrupt data).

`queryMem` returns per the OBJ/file convention: `resp[i]` **engaged** (an empty
params map) on a **hit**, `std::nullopt` on a **miss**. A submit-/transport-level
failure returns an error status (never masked as a miss).

## Design decisions

- **Build on SPDK `lib/nvme`, not the raw vfio-user client.** The
  plugin submits KV commands through SPDK's public NVMe-KV API
  (`spdk_nvme_kv_store` / `spdk_nvme_kv_retrieve` in `include/spdk/nvme_kv.h`)
  over a controller attached with `spdk_nvme_probe`. SPDK's transport layer is
  what makes the datapath transport-agnostic for free (VFIOUSER now, PCIE
  later). This is why the plugin does **not** reuse the raw `nkv_vfu` client.
- **Own generic shim.** The C++ backend talks to a small, self-contained C shim
  (`spdk_kv_shim.{h,c}`) compiled into its own static lib. This isolates the
  C-only SPDK headers from the C++ TUs and keeps the plugin free of any external
  shim dependency. The shim is generic (no backend-specific behavior). It
  intentionally does **not** carry per-op identity-token / reconnect hardening;
  that robustness (for target-death mid-op) is deferred — this skeleton targets
  a healthy in-memory/vfio-user target.
- **Transport as config.** The `transport_id` param is a standard SPDK transport
  ID string parsed with `spdk_nvme_transport_id_parse`. A bare socket directory
  may be passed as `vfu_addr`/`socket` and is wrapped into a VFIOUSER trid.
- **Key is opaque.** Lineage-vs-flat is the caller's key-format choice; the
  generic plugin only checks the bytes fit the ratified 1–16-byte window.
- **DRAM staging.** SPDK Store/Retrieve need DMA-capable buffers; registered
  user DRAM generally is not, so the plugin stages through a per-request SPDK
  DMA buffer and copies. A future revision registers user/GPU buffers directly.
- **Value auto-sizing.** On Retrieve the completion `cdw0` carries the device's
  TRUE stored length. Devices signal a short host buffer two ways — SUCCESS with
  `cdw0 > buf_len` (this KV target), or `0x85 INVALID_VALUE_SIZE` directly — and
  the shim **normalizes both** to a single `BUFFER_TOO_SMALL` (`0x85`) return
  that reports the true length. The
  backend then does **not** copy a truncated value; it records the true length
  and reports `NIXL_ERR_MISMATCH`, so the caller resizes and re-Retrieves. The
  true length is read back with `getReqTrueLen(handle, idx)`. This implements
  the **optimistic-then-resize** strategy; a size-cache and a 2-RTT probe-first
  variant are left for later.

## Custom backend parameters (`nixl_b_params_t`)

| Parameter      | Required | Default | Meaning |
| -------------- | -------- | ------- | ------- |
| `transport_id` | yes\*    | —       | SPDK transport ID, e.g. `trtype:VFIOUSER traddr:<socket-dir>`. |
| `vfu_addr`     | yes\*    | —       | Bare vfio-user socket dir; wrapped into a VFIOUSER `transport_id`. Aliases: `socket`, `vfio_user_path`. |
| `nsid`         | no       | `0`     | NVMe namespace id; `0`/unset auto-selects the first KV namespace. |
| `init_env`     | no       | `false` | `false`: the host/agent owns the SPDK env (multiple engines per process). `true`: the shim brings up its own no-hugepage SPDK env (single instance; for standalone tests). |

\* Provide **either** `transport_id` **or** `vfu_addr`.

## Build

The plugin is gated on the prebuilt SPDK NVMe library and the KV header; it is
skipped automatically if they are absent. It ships its own shim, so
`-Dspdk_kv_shim_dir` is **not** used by this plugin.

```bash
meson setup builddir \
    -Denable_plugins=SPDK_KV \
    -Dspdk_root=/path/to/spdk \
    -Dspdk_kv_build_test=true
ninja -C builddir src/plugins/spdk_kv/libplugin_SPDK_KV.so \
                  src/plugins/spdk_kv/spdk_kv_roundtrip_test
```

| Option              | Default | Description |
| ------------------- | ------- | ----------- |
| `spdk_root`         | `""`    | Path to the built SPDK tree to link against (unset — required). |
| `spdk_kv_build_test`| `false` | Build the direct-engine round-trip test binary. |

## Testing

`run_roundtrip.sh` stands up an SPDK `nvmf_tgt` with an **in-memory** KV
namespace (`kvdev_mem`, **no Ceph**) over VFIOUSER and runs the round-trip test:
Store a small value under a 16-byte key, Retrieve it back, and verify
byte-for-byte. It also checks the opaque key guards, **QUERY/Exist** (hit on a
stored key, miss on an absent key), and **value auto-sizing** (a short-buffer
READ reports the true length, then a correctly-sized READ returns byte-exact).

Note: the round-trip needs a **target-capable** KV SPDK build (one whose
`nvmf_tgt` has the `kvdev_mem` module + KV nvmf namespace RPCs). Point
`SPDK_ROOT` at that tree; the host-side SPDK the plugin links against may carry
only the NVMe-KV *host* driver.

```bash
SPDK_ROOT=/path/to/spdk ./run_roundtrip.sh
```
