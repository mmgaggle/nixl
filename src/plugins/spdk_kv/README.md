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

This directory implements the **walking skeleton** — the small-value Store /
Retrieve datapath.

## Scope

In:
- NIXL `WRITE` → KV **Store**, NIXL `READ` → KV **Retrieve**.
- **16-byte inline keys**, taken **verbatim** (opaque; no lineage parsing).
- **Small values** in **host DRAM**.
- Connect via a **standard SPDK transport ID** (`trtype:VFIOUSER traddr:<sock>`).

Out:
- VRAM / P2PDMA.
- Large values via a region-bounded SGL, and value auto-sizing.
- Exist/QUERY, Delete/List.
- Device-mode (`PCIE`) transport parity.
- KV **Exec** and **long keys** — permanently out of scope for this generic
  plugin.

## Operation and memory-type mapping

| NIXL operation | Local mem  | Remote mem | NVMe KV command |
| -------------- | ---------- | ---------- | --------------- |
| `NIXL_WRITE`   | `DRAM_SEG` | `OBJ_SEG`  | KV **Store**    |
| `NIXL_READ`    | `DRAM_SEG` | `OBJ_SEG`  | KV **Retrieve** |

The remote `OBJ_SEG` descriptor carries the NIXL block identifier in its
`metaInfo` blob. The engine takes those bytes **verbatim** as the inline NVMe-KV
key (1–16 bytes). An empty or over-16-byte key is **rejected**
(`NIXL_ERR_INVALID_PARAM`), never truncated (truncation would alias distinct
keys sharing a prefix and corrupt data).

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
byte-for-byte. It also checks the opaque key guards and the short-read (no
silent truncation) behavior.

```bash
SPDK_ROOT=/path/to/spdk ./run_roundtrip.sh
```
